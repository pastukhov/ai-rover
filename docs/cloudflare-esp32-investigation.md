# Расследование: ai error 0x7004 — ESP32 + Cloudflare

## Симптом

После попытки миграции на ESP-Claw (и последующего отката) ровер перестал отвечать через AI-чат.  
Веб-эндпоинт `/chat_result` возвращал:

```
ai error: 0x7004
```

## Расшифровка кода ошибки

`0x7004 = ESP_ERR_HTTP_FETCH_HEADER` — HTTP-клиент установил TCP+TLS соединение, отправил запрос, но не смог прочитать заголовки ответа.

Серийный монитор показал:

```
E esp-tls-mbedtls: read error :-0x0050
E transport_base: esp_tls_conn_read error, errno=Connection reset by peer
E OPENROUTER: HTTP_EVENT_ERROR
W OPENROUTER: HTTP request failed with error ESP_ERR_HTTP_FETCH_HEADER (28676), will retry
```

`-0x0050 = MBEDTLS_ERR_NET_CONN_RESET` — сервер прислал TCP RST вместо HTTP-ответа.

## Что было исключено

### ❌ API-ключ
```bash
curl https://openrouter.ai/api/v1/chat/completions \
  -H "Authorization: Bearer $KEY" \
  -d '{"model":"openai/gpt-4o-mini","messages":[{"role":"user","content":"hi"}]}'
# → HTTP 200 OK
```
Ключ рабочий.

### ❌ TLS 1.2 не принимается
```bash
curl --tls-max 1.2 https://openrouter.ai/...
# → HTTP/1.1 200 OK
```
TLS 1.2 сервер принимает.

### ❌ HTTP/1.1 не принимается
```bash
curl --http1.1 https://openrouter.ai/...
# → HTTP/1.1 200 OK
```

### ❌ User-Agent: ESP32 HTTP Client/1.0
```bash
curl -A "ESP32 HTTP Client/1.0" https://openrouter.ai/...
# → HTTP 200 OK
```

### ❌ Размер/содержимое запроса (tools + system prompt)
```bash
# Тот же JSON-body с tools, temperature=0, top_p=0 — с хоста:
curl --http1.1 ... -d '<1843-байтное тело>' https://openrouter.ai/...
# → HTTP 200 OK
```

### ❌ Смена TLS 1.3 / отключение RSA cipher suites
Добавление `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` и `CONFIG_MBEDTLS_KEY_EXCHANGE_RSA=n` в sdkconfig — ошибка не исчезла.

### ❌ Уменьшение `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` с 4096 до 1250
Изменение размера TLS-записи — ошибка не исчезла.

### ❌ Сертификат openrouter.ai
Сертификат сменился 24 апреля 2026 (Google Trust Services / GTS Root R4).  
`GTS Root R4` присутствует в CA-бандле ESP-IDF 5.5.3 — проверено в `cacrt_all.pem`.

### ❌ Версия ESP-IDF (5.5.0 → 5.5.3)
ESP-Claw коммит поднял ESP-IDF с 5.5.0 до 5.5.3 и перегенерировал sdkconfig.  
Но прошивка, собранная с 5.5.3, даёт ту же ошибку.

## Ключевые диагностические тесты

### Тест 1: HTTPS GET к openrouter.ai с ESP32
```
GET /api/v1/models → HTTP 200 OK ✓
```
TLS-соединение к Cloudflare с ESP32 работает.

### Тест 2: Маленький POST к openrouter.ai (77 байт тела)
```
POST /api/v1/chat/completions, body=77 байт → HTTP 200 OK ✓
```

### Тест 3: Большой POST к openrouter.ai (1843 байт тела с tools)
```
POST /api/v1/chat/completions, body=1843 байт → 0x7004 ✗
```

### Тест 4: Тело без tools, но такого же размера (1253 байт)
```
POST body=1253 байт (без tools) → HTTP 200 OK ✓
POST body=1400 байт (без tools) → 0x7004 ✗
```
Порог — где-то между 1253 и 1400 байтами тела.

### Тест 5: Большой POST к httpbin.org (не Cloudflare) с ESP32
```
POST https://httpbin.org/post, body=1843 байт → HTTP 200 OK ✓
```

## Вывод: корневая причина

**Cloudflare специфично блокирует большие POST-запросы от ESP32.**

| Факт | Подтверждение |
|------|---------------|
| Тот же запрос с хоста (curl) → 200 | ✓ проверено |
| Большой POST к не-Cloudflare серверу → 200 | ✓ httpbin.org |
| Большой POST к Cloudflare → TCP RST | ✗ openrouter.ai |
| Маленький POST к Cloudflare → 200 | ✓ |

**Механизм:** Cloudflare RST-ирует TCP-соединение ПОСЛЕ получения полного запроса, не отправляя HTTP-ответа. Это не TLS-ошибка — TLS-рукопожатие и отправка запроса проходят успешно. RST приходит при попытке читать ответ (`MBEDTLS_ERR_NET_CONN_RESET`).

**Почему порог ~1280 байт:** ESP32 использует lwIP с `CONFIG_LWIP_TCP_MSS=1440` и mbedTLS. При небольших телах TLS-запись тела укладывается в один TCP-сегмент. При теле >~1280 байт TLS-запись тела выходит за границу TCP-сегмента (с учётом TLS-overhead и реального MSS Cloudflare). По всей видимости, именно паттерн фрагментации TCP/TLS-сегментов от lwIP/mbedTLS выделяет ESP32 на фоне обычных клиентов и триггерит Cloudflare Bot Fight Mode или аналогичный механизм.

**Почему это началось именно тогда:** Предположительно Cloudflare ужесточил правила ботозащиты примерно в конце апреля 2026, совпав по времени с ротацией сертификата openrouter.ai (24 апреля). Пользователь заметил проблему после попытки миграции на ESP-Claw (26 апреля).

## Решение: локальный HTTP→HTTPS прокси

Cloudflare видит HTTPS-соединение от обычной Linux-машины вместо mbedTLS ESP32.  
ESP32 видит простой HTTP-эндпоинт в локальной сети.

```
ESP32 → HTTP → прокси на хосте (порт 8080) → HTTPS → openrouter.ai
```

### Прокси-скрипт
`tools/openrouter_proxy.py` — стандартная библиотека Python, без зависимостей.

```bash
python3 tools/openrouter_proxy.py
```

### Конфигурация прошивки
`include/secrets.h`:
```c
#define OPENROUTER_PROXY_URL "http://<ip-хоста>:8080/api/v1/chat/completions"
```

`src/main_idf.cpp` в `init_ai()`:
```c
cfg.api_base_url = (sizeof(OPENROUTER_PROXY_URL) > 1) ? OPENROUTER_PROXY_URL : NULL;
```

Библиотека `openrouter_client` была расширена: добавлено поле `api_base_url` в `openrouter_config_t`.  
При заданном `api_base_url` библиотека подключается по HTTP без TLS вместо стандартного HTTPS.

### Альтернативные прокси

Подойдёт любой L7-прокси, который принимает HTTP локально и пробрасывает HTTPS на upstream:

```bash
# Caddy
caddy reverse-proxy --from :8080 --to https://openrouter.ai

# Nginx
location / { proxy_pass https://openrouter.ai; proxy_ssl_server_name on; }
```

## Хронология миграции (что пошло не так)

1. ~24 апреля 2026 — openrouter.ai меняет TLS-сертификат на Google Trust Services
2. Одновременно (предположительно) — Cloudflare ужесточает Bot Fight Mode
3. 26 апреля — пользователь пытается мигрировать на ESP-Claw как обходной путь,  
   в процессе ESP-IDF обновляется с 5.5.0 до 5.5.3 (sdkconfig перегенерируется)
4. 26 апреля — откат с ESP-Claw: код возвращён, но sdkconfig остался в состоянии 5.5.3
5. Прошивка с openrouter_client не работает ни со старым, ни с новым ESP-IDF

**Настоящая причина одна:** Cloudflare, а не версия ESP-IDF или сертификат.
