# План: управление ровером через LLM tool calling

## Контекст

Чат с LLM уже работает в веб-интерфейсе. Нужно подключить управление моторами и захватом через function calling — чтобы пользователь мог писать в чат команды вроде «поезжай вперёд 2 секунды, потом поверни направо и остановись», и ровер выполнял их.

Библиотека `openrouter_client` поддерживает tool calling через `openrouter_call_with_tools()` с автоматическим выполнением зарегистрированных функций.

## Изменения в `src/main.cpp`

### 1. Модель

Сменить `google/gemma-3n-e2b-it:free` на модель с поддержкой tool calling. Кандидат: `qwen/qwen3-coder:free` (хорошо работает со структурированным выводом).

### 2. Конфиг AI handle

```cpp
ai_cfg.enable_tools = true;
ai_cfg.default_system_role = "You are the AI brain of a mecanum-wheel rover robot with a gripper. "
    "Use the provided tools to control the rover when the user asks. "
    "For movement commands with duration, call move() which blocks for the specified time then stops. "
    "You can chain multiple tool calls for sequences like 'forward then turn'. "
    "Respond naturally in the user's language.";
```

### 3. Регистрация 4 функций (после `openrouter_create`)

| Функция | Параметры | Действие |
|---------|-----------|----------|
| `move` | `x` (number), `y` (number), `z` (number, опц., default 0), `duration_ms` (number, опц., default 1000) | Двигаться duration_ms мс, потом стоп |
| `stop` | — | Стоп всех моторов |
| `gripper_open` | — | Открыть захват |
| `gripper_close` | — | Закрыть захват |

Используем `openrouter_register_simple_function()` — simplified API.

### 4. Callback-функции

Определяются в anonymous namespace (доступ к `roverc`, `setAction`):

- **`cb_move`**: парсит аргументы через `cJSON`, `constrain` значения, крутит моторы `duration_ms` мс (в цикле с `roverc.setSpeed` каждые 50мс), потом `roverc.setSpeed(0,0,0)`. Max duration: 5000ms.
- **`cb_stop`**: вызывает `commandStop("AI STOP")`.
- **`cb_gripper_open`**: вызывает `commandGripperOpen()`.
- **`cb_gripper_close`**: вызывает `commandGripperClose()`.

Все возвращают `strdup("{\"status\":\"ok\"}")`.

`cJSON.h` уже доступен через `#include "openrouter.h"` → `#include "cJSON.h"`.

### 5. `handleChat` — использовать `openrouter_call_with_tools`

Заменить `openrouter_call(ai_handle, ...)` на:
```cpp
openrouter_call_with_tools(ai_handle, msg.c_str(), response, sizeof(response), 5);
```

`max_tool_iterations = 5` — до 5 последовательных tool call → хватит для цепочек из нескольких команд.

### 6. Увеличить буфер ответа

С `char response[1024]` до `char response[2048]` — tool calling генерирует более длинные промежуточные ответы.

## Файлы

| Файл | Изменения |
|------|-----------|
| `src/main.cpp` | Модель, system role, enable_tools, 4 callback-функции, регистрация, handleChat → call_with_tools |

## Проверка

1. `pio run` — компиляция
2. `pio run --target upload` — прошивка
3. `curl "http://<IP>/chat?msg=drive+forward+1+second"` — ответ с tool call, ровер едет 1 сек
4. В браузере: «поезжай вперёд, потом поверни направо и остановись» — цепочка команд
5. «открой захват» → захват открывается
