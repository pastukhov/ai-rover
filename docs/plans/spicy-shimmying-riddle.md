# План: интеграция openrouter_client в PlatformIO Arduino проект

## Контекст

Библиотека `openrouter_client` (автор — пользователь) — ESP-IDF компонент для работы с OpenRouter API. Нужно добавить её в PlatformIO Arduino проект ровера. Все ESP-IDF заголовки (`esp_http_client.h`, `cJSON.h`, `esp_tls.h`, `esp_crt_bundle.h`) уже доступны в Arduino-ESP32 — переписывать HTTP-клиент не нужно. Единственная проблема — 6 `CONFIG_*` макросов из Kconfig без fallback-дефолтов в корневом `include/openrouter.h` (в `components/openrouter/openrouter.h` фикс уже есть).

## Шаги

### 1. Фикс библиотеки: `include/openrouter.h`

Заменить строки 13-21 (6 макросов без `#ifdef`) на версию из `components/openrouter/openrouter.h` (строки 13-53) — с `#ifdef CONFIG_* / #else / #endif` и дефолтами из Kconfig.

### 2. Добавить `library.json` в корень библиотеки

```json
{
  "name": "openrouter_client",
  "version": "1.0.1",
  "description": "OpenRouter API client for ESP32",
  "license": "MIT",
  "frameworks": ["arduino", "espidf"],
  "platforms": "espressif32",
  "headers": "openrouter.h",
  "build": {
    "srcFilter": ["+<openrouter.c>"],
    "includeDir": "include",
    "flags": ["-std=gnu11"]
  }
}
```

Важно: `srcFilter` — только `openrouter.c` из корня, чтобы не собирать дубликаты из `components/`, `examples/`, `test_apps/`.

### 3. Коммит и пуш изменений библиотеки

В `/home/artem/repos/ai-rover/libraries/openrouter_client/`:
- `git add include/openrouter.h library.json`
- `git commit` + `git push`

### 4. Добавить зависимость в `platformio.ini`

```ini
lib_deps =
  m5stack/M5Unified
  m5stack/M5-RoverC
  https://github.com/pastukhov/openrouter_client.git
```

### 5. Обновить `include/secrets.h.example`

Добавить `#define OPENROUTER_API_KEY "YOUR_OPENROUTER_API_KEY"`.

### 6. Минимальный тест-вызов в `src/main.cpp`

Добавить `#include "openrouter.h"` и тестовый вызов в `setup()` после WiFi-подключения:
- Создать `openrouter_handle_t` с конфигом (бесплатная модель, `max_tokens=128`)
- Вызвать `openrouter_call()` с тестовым промптом
- Напечатать ответ в Serial
- Уничтожить хэндл

Вызов блокирующий (2-10 сек), но в `setup()` это допустимо — WiFi connect тоже блокирует.

## Файлы для изменения

| Файл | Действие |
|------|----------|
| `libraries/openrouter_client/include/openrouter.h` | Фикс: добавить `#ifdef` fallback-дефолты |
| `libraries/openrouter_client/library.json` | Новый: PlatformIO манифест |
| `platformio.ini` | Добавить lib_deps запись |
| `include/secrets.h.example` | Добавить OPENROUTER_API_KEY |
| `src/main.cpp` | Добавить include и тест-вызов |

## Проверка

1. `pio run` — компиляция без ошибок
2. `pio run --target upload` — прошивка
3. `pio device monitor --baud 115200` — в логе появится "AI response: ..."
