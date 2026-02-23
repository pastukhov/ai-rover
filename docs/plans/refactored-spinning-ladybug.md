# Plan: Регистрация mDNS с перечислением эндпоинтов

## Контекст

Ровер доступен только по IP-адресу (например, 192.168.11.X). Нужно добавить mDNS-регистрацию, чтобы устройство было доступно как `ai-rover.local` и публиковало свои HTTP-эндпоинты через DNS-SD.

## Изменения

### Файл: `src/main_idf.cpp`

1. **Добавить include:**
   ```cpp
   #include "mdns.h"
   ```

2. **Создать функцию `start_mdns()`** — вызывать после подключения WiFi (рядом с `start_web_server()`):
   - `mdns_init()` — инициализация
   - `mdns_hostname_set("ai-rover")` — hostname → `ai-rover.local`
   - `mdns_instance_name_set("AI Rover Web Interface")` — человекочитаемое имя
   - `mdns_service_add("AI Rover", "_http", "_tcp", 80, ...)` — регистрация HTTP-сервиса с TXT-записями

3. **TXT-записи** для описания эндпоинтов (DNS-SD стандарт):
   ```
   path=/
   api_cmd=/cmd
   api_status=/status
   api_vision=/vision
   api_chat=/chat
   api_chat_result=/chat_result
   ```

4. **Точки вызова** — вызвать `start_mdns()` в двух местах, где сейчас вызывается `start_web_server()`:
   - При первом подключении WiFi (~строка 1629)
   - При реконнекте WiFi (~строка 1426)

5. **Добавить `mdns_free()` в cleanup** перед deep sleep (рядом с `httpd_stop`).

## Верификация

- `pio run --target upload` — сборка и прошивка
- Проверить доступность `ai-rover.local` через `ping ai-rover.local` или открыть в браузере
- `dns-sd -B _http._tcp` (macOS) или `avahi-browse -r _http._tcp` (Linux) — должен показать сервис с TXT-записями
