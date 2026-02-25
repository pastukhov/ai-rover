# AI Rover (M5StickC Plus + RoverC Pro)

## Русский

Проект прошивки для **M5StickC Plus** (контроллер) и **RoverC Pro** (база с моторами и захватом) на PlatformIO.

### Возможности
- Управление движением RoverC Pro.
- Управление захватом (открыть/закрыть).
- Demo-сценарий по кнопке `BtnA`.
- Аварийный стоп по кнопке `BtnB`.
- Веб‑интерфейс управления по Wi‑Fi.
- Статус на экране: текущее действие, батарея, Wi‑Fi.
- Структурные JSON‑логи в UART и syslog через единый логгер `rover_log(...)`.

### Структура
- `src/main_idf.cpp` — основная логика прошивки (ESP‑IDF / PlatformIO).
- `src/logger_json.{h,cpp}` — единый structured logger (UART + syslog mirror).
- `platformio.ini` — конфигурация PlatformIO.
- `include/secrets.h` — локальные Wi‑Fi креды (не коммитится).
- `include/secrets.h.example` — шаблон секретов.
- `docs/` — документация и планы.
- `docs/logging-conventions.md` — схема и naming для JSON‑логов.

### Быстрый старт
1. Создайте `include/secrets.h` по шаблону `include/secrets.h.example`.
2. Сборка:
```bash
pio run
```
3. Прошивка:
```bash
pio run --target upload
```
4. Монитор порта:
```bash
pio device monitor --baud 115200
```

### Веб‑управление
После подключения к Wi‑Fi ровер поднимает HTTP-сервер на порту `80`.
IP отображается на экране устройства (`WiFi: x.x.x.x`).
Откройте в браузере:
```text
http://<IP_ровера>/
```
Ровер так же аннонсирует себя посредством mDNS и доступен по адресу [ai-rover.local](http://ai-rover.local)

Откройте в браузере:

```text
http://<IP_ровера>/
```

или

```text
http://ai-rover.local/
```


### Безопасность
- Не коммитьте `include/secrets.h`.
- Проверьте `.gitignore` перед пушем.

### Логи и наблюдаемость
- Все runtime-логи приложения идут в JSON через `rover_log(...)`.
- Один и тот же JSON пишется в UART и зеркалится в syslog.
- Бизнес-логика не должна вызывать `send_syslog()` напрямую.
- Формат и naming событий: `docs/logging-conventions.md`.

---

## English

Firmware project for **M5StickC Plus** (controller) and **RoverC Pro** (motor/gripper base) using PlatformIO.

### Features
- RoverC Pro motion control.
- Gripper control (open/close).
- Demo sequence on `BtnA`.
- Emergency stop on `BtnB`.
- Wi‑Fi web control page.
- On-screen status: current action, battery, Wi‑Fi.
- Structured JSON logs to UART and syslog via the unified `rover_log(...)` logger.

### Structure
- `src/main_idf.cpp` — main firmware logic (ESP-IDF / PlatformIO).
- `src/logger_json.{h,cpp}` — unified structured logger (UART + syslog mirror).
- `platformio.ini` — PlatformIO configuration.
- `include/secrets.h` — local Wi‑Fi credentials (ignored by git).
- `include/secrets.h.example` — credentials template.
- `docs/` — hardware notes and plans.
- `docs/logging-conventions.md` — JSON log schema and event naming rules.

### Quick Start
1. Create `include/secrets.h` from `include/secrets.h.example`.
2. Build:
```bash
pio run
```
3. Flash:
```bash
pio run --target upload
```
4. Serial monitor:
```bash
pio device monitor --baud 115200
```

### Web Control
After joining Wi‑Fi, the rover starts an HTTP server on port `80`.
The device screen shows its IP (`WiFi: x.x.x.x`).
Open:
```text
http://<rover_ip>/
```

### Security
- Do not commit `include/secrets.h`.
- Verify `.gitignore` before pushing.

### Logs & Observability
- All application runtime logs are emitted as JSON through `rover_log(...)`.
- The same JSON line is written to UART and mirrored to syslog.
- Business logic should not call `send_syslog()` directly.
- Log schema and event naming conventions are documented in `docs/logging-conventions.md`.
