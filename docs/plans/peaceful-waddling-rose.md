# План: приведение прошивки к эталонной архитектуре AI-агента

## Контекст

Аудит `docs/esp32_agent_architecture_audit.md` выявил системные проблемы в `src/main_idf.cpp`:
- Data race на shared-переменных между задачами (main loop, HTTP server, chat_worker)
- Отсутствие явного FSM — состояние ровера имплицитно
- Задачи не привязаны к ядрам
- Нет offline fallback, watchdog не настроен
- Динамическая аллокация в tool callbacks

Цель — устранить все Critical/High/Medium проблемы из аудита, сохранив текущую функциональность.

## Файлы для изменения

- `src/main_idf.cpp` — основная прошивка (все изменения кода)
- `sdkconfig.idf.defaults` — настройки watchdog

---

## Фаза 1: Явный FSM + логирование переходов [Critical]

Ввести enum состояний и централизованную функцию перехода.

```c
typedef enum {
    STATE_IDLE,
    STATE_WEB_CONTROL,
    STATE_AI_THINKING,
    STATE_AI_EXECUTING,
    STATE_OFFLINE_FALLBACK,
} rover_state_t;

static rover_state_t s_rover_state = STATE_IDLE;

static void transition_to(rover_state_t new_state);
```

**Что делать:**
1. Добавить `rover_state_t` enum и `s_rover_state` (защищается `s_state_mutex`)
2. Реализовать `transition_to()` — проверяет допустимость перехода, логирует через syslog и ESP_LOGI
3. Вызывать `transition_to()` в ключевых точках:
   - Web command → `STATE_WEB_CONTROL` → `STATE_IDLE` (по таймауту/stop)
   - Chat submit → `STATE_AI_THINKING`
   - Tool callback начинается → `STATE_AI_EXECUTING`
   - Tool callback завершается / ошибка AI → `STATE_IDLE`
   - WiFi потеряна → `STATE_OFFLINE_FALLBACK`
4. Отображать текущее состояние на дисплее (`update_local_display`)

## Фаза 2: Синхронизация shared-данных [Critical]

Устранить data race между задачами.

**Что делать:**
1. `apply_motion()` — вызывать только под `s_state_mutex`:
   - строка 645 (`handle_cmd`): перенести `apply_motion()` внутрь блока мьютекса
   - строка 1001 (main loop): обернуть в `xSemaphoreTake/Give`
2. Tool callbacks `cb_gripper_open/close` — обернуть запись `s_gripper_open` и вызов `rover_set_servo_angle` в `s_state_mutex`
3. `mark_activity()` — заменить на atomic-запись:
   ```c
   #include <stdatomic.h>
   static _Atomic uint32_t s_last_activity_tick = 0;
   ```
   Альтернатива: оставить `TickType_t` и всегда вызывать под мьютексом.
4. `cb_move()` и `cb_turn()` — I2C вызовы (`rover_set_speed`) уже внутри воркера, конфликт с main loop через `apply_motion()`. Решение: добавить `s_state_mutex` вокруг I2C в tool callbacks (или ввести отдельный I2C-мьютекс).

**Решение по I2C:** Ввести `s_i2c_mutex` — все вызовы `rover_write()` через этот мьютекс. Это изолирует I2C от data race и позволяет не держать `s_state_mutex` на всё время движения.

## Фаза 3: Привязка задач к ядрам [High]

**Что делать:**
1. `chat_worker_task` — `xTaskCreatePinnedToCore(..., 1)` (Core 1 — agent)
2. Main loop — вынести из `app_main` в отдельную `main_loop_task`, привязать к Core 0:
   ```c
   xTaskCreatePinnedToCore(main_loop_task, "main_loop", 4096, NULL, 5, NULL, 0);
   ```
   `app_main` завершается после инициализации (FreeRTOS scheduler уже работает).

## Фаза 4: Offline fallback [High]

**Что делать:**
1. При ошибке WiFi (`wifi_err != ESP_OK`) — не перезагружаться, а:
   - `transition_to(STATE_OFFLINE_FALLBACK)`
   - Показать на дисплее "OFFLINE — buttons only"
   - Войти в main loop без web server и AI
   - Кнопки A/B работают (движение, gripper)
2. При восстановлении WiFi (добавить periodic reconnect) — перейти в `STATE_IDLE`, запустить web server

## Фаза 5: Watchdog [High]

**Что делать в `sdkconfig.idf.defaults`:**
```
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y
```

В коде — подписать `main_loop_task` на WDT (`esp_task_wdt_add(NULL)` + `esp_task_wdt_reset()` каждую итерацию). `chat_worker_task` **не** подписывать — там долгие HTTP-вызовы.

Убрать `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y` (или заменить на `SILENT`), чтобы не маскировать проблемы.

## Фаза 6: Memory — убрать лишние аллокации [Medium]

> **Факт:** openrouter_client вызывает `free()` на возвращённых из callback строках — это API-контракт. `strdup()` в tool callbacks обязателен, менять нельзя.

**Что делать:**
1. Tool callbacks (`cb_move`, `cb_turn`, `cb_read_imu`) — оставить `strdup()` как есть, добавить комментарий `// openrouter_client frees this`
2. `handle_chat()` — заменить `calloc` на stack-буферы:
   ```c
   char query[1024];
   char prompt[CHAT_PROMPT_MAX];
   ```
   Стек httpd задач достаточно большой (4096+ по умолчанию). Это убирает 2x calloc + free на каждый HTTP-запрос.
3. `handle_chat_result()` — `char response[CHAT_RESPONSE_MAX]` (строка 749) уже на стеке, 2KB — проверить что стек httpd-задачи достаточен (default 4096, может понадобиться увеличить `config.stack_size`).

## Фаза 7: Safety — emergency stop после ошибки AI [Medium]

**Что делать:**
1. В `chat_worker_task`, после `openrouter_call_with_tools` при `err != ESP_OK`:
   ```c
   xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
   rover_emergency_stop();
   xSemaphoreGive(s_i2c_mutex);
   xSemaphoreTake(s_state_mutex, portMAX_DELAY);
   set_motion(0, 0, 0, false);
   transition_to(STATE_IDLE);
   xSemaphoreGive(s_state_mutex);
   ```
2. При успешном завершении AI-вызова — тоже `transition_to(STATE_IDLE)`.

## Фаза 8: Syslog через очередь [Medium]

**Что делать:**
1. Создать `s_syslog_queue` (QueueHandle_t, depth=8, item=char[256])
2. `send_syslog()` — только `xQueueSend(..., 0)` (non-blocking, drop если полна)
3. Отдельная задача `syslog_task` на Core 1 (низкий приоритет) — `xQueueReceive` + `send()`
4. Это убирает блокирующий `send()` из main loop

---

## Порядок реализации

| Шаг | Фаза | Зависимости |
|-----|-------|-------------|
| 1 | Фаза 1: FSM enum + transition_to | — |
| 2 | Фаза 2: Синхронизация (мьютексы, i2c_mutex) | Фаза 1 |
| 3 | Фаза 3: Core pinning | Фаза 1 |
| 4 | Фаза 5: Watchdog (sdkconfig) | Фаза 3 |
| 5 | Фаза 4: Offline fallback | Фаза 1, 3 |
| 6 | Фаза 7: Emergency stop после AI-ошибки | Фаза 1, 2 |
| 7 | Фаза 6: Memory (strdup → static) | — |
| 8 | Фаза 8: Syslog queue | Фаза 3 |

## Верификация

1. `pio run` — компиляция без ошибок
2. `pio run --target upload` — прошивка на устройство
3. Проверить через serial monitor:
   - Логи переходов FSM при boot
   - Heartbeat с текущим состоянием
4. Web UI — кнопки движения, gripper, chat с AI
5. Нажать BtnB — emergency stop, проверить лог перехода
6. Отключить WiFi (выключить роутер) — ровер должен перейти в OFFLINE_FALLBACK, кнопки работают
7. Отправить невалидный chat — проверить что ровер переходит в IDLE, моторы остановлены
