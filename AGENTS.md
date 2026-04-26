# Repository Guidelines for AI Rover

## Project Overview

Embedded robotics project for an M5Stack RoverC Pro (K036-B) mecanum-wheel robot controlled by an M5StickC Plus (ESP32-PICO-D4). Firmware provides AI-powered navigation, button-triggered sequences, emergency stop, on-screen status, and Wi-Fi web control.

---

## Build, Test, and Development Commands

### Prerequisites
```bash
cp include/secrets.h.example include/secrets.h   # Edit with your SSID/password
```

### Build Commands
```bash
pio run                      # Build firmware and validate compilation
pio run --target upload      # Flash firmware to connected device
pio device monitor --baud 115200   # Open serial monitor
pio run -t clean             # Remove current build outputs
```

### Single File/Target Build
```bash
pio run --environment m5stick-c-plus   # Explicit environment
pio run --target size                 # Analyze binary size
pio run --target compiledb            # Generate compile_commands.json
```

### Code Search
```bash
rg "<pattern>" -n           # Fast code/text search across repository
```

### Validation Checklist
1. `pio run` passes (no compilation errors)
2. Flash succeeds (`pio run --target upload`)
3. Serial boot log smoke test (JSON log lines visible after reset)
4. Manual hardware smoke test for any enabled peripherals

---

## Code Style Guidelines

### Language & Standards
- **Language:** C/C++ with PlatformIO + ESP-IDF framework
- **C++ Standard:** C++14 minimum
- **File:** `src/main_idf.cpp` (application code), `src/logger_json.{h,cpp}` (logging)

### Formatting
- **Indentation:** 2 spaces; no tabs
- **Line Endings:** LF (Unix-style)
- **Braces:** K&R style (opening brace on same line)
- **Maximum Line Length:** ~120 characters preferred, up to 240 for embedded strings

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Functions/Variables | `snake_case` | `rover_set_speed()` |
| Compile-time constants | `kPascalCase` | `kMoveSpeed` |
| Macros | `UPPER_SNAKE_CASE` | `CHAT_PROMPT_MAX` |
| Enums | `snake_case_t` | `rover_state_t` |
| Enum values | `UPPER_SNAKE_CASE` | `STATE_IDLE` |
| Classes/Types | `PascalCase` | `openrouter_handle_t` |
| Member variables | `snake_case_` (trailing underscore) | `s_rover_state` |
| Static globals | `s_name` | `s_wifi_connected` |

### File Structure & Imports

**Standard include order (most specific to most general):**
```cpp
#include <atomic>                  // C++ stdlib
#include <stdint.h>               // C stdlib
#include <stdio.h>
#include <string.h>

#include "M5Unified.h"            // Project-local (quotes)
#include "logger_json.h"
#include "secrets.h"

#include "esp_log.h"              // ESP-IDF (angle brackets)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
```

**Key API Abstractions:**
- Prefer `M5Unified.h` over device-specific headers (`M5StickC.h`, `M5StickCPlus.h`)
- Singleton `M5` object: access peripherals via `M5.Display`, `M5.Speaker`, `M5.Imu`, `M5.Power`
- All motor/servo I2C goes through `M5_RoverC` class — never write I2C registers directly

### Types & Declarations

**Use explicit fixed-width types for hardware/register interfaces:**
```cpp
static const uint8_t kRoverAddr = 0x38;      // I2C addresses
static const gpio_num_t kBtnAPin = GPIO_NUM_37;  // ESP-IDF types
static int8_t s_motion_x;                    // Signed ranges for motor control
static uint32_t s_chat_id;
```

**Atomic types for cross-core communication:**
```cpp
static std::atomic<bool> s_wifi_connected{false};   // Lock-free reads
static std::atomic<uint32_t> s_last_activity_tick{0};
```

**Struct definitions:**
```cpp
typedef struct {
  uint32_t req_id;
  ai_action_kind_t kind;
  int8_t x, y, z;
  uint16_t duration_ms;
} ai_action_req_t;
```

### Error Handling

**ESP-IDF error codes with ESP_ERROR_CHECK:**
```cpp
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_wifi_init(&cfg));
```

**Custom error returns:**
```cpp
static esp_err_t rover_write(uint8_t reg, const uint8_t *data, size_t len) {
  if (!M5.Ex_I2C.isEnabled()) {
    return ESP_ERR_INVALID_STATE;
  }
  bool ok = M5.Ex_I2C.writeRegister(kRoverAddr, reg, data, len, kI2cFreqHz);
  return ok ? ESP_OK : ESP_FAIL;
}
```

**Return early for error paths — avoid deep nesting:**
```cpp
esp_err_t err = vision_capture(quality, &jpeg, &jpeg_size);
if (err != ESP_OK) {
  s_vision_available.store(false, std::memory_order_relaxed);
  return make_tool_response("capture_failed", "vision_capture");
}
```

### Concurrency & Thread Safety

**Mutex pattern (always paired):**
```cpp
xSemaphoreTake(s_state_mutex, portMAX_DELAY);
// ... protected operations ...
xSemaphoreGive(s_state_mutex);
```

**Use RAII-style patterns for locks where possible:**
```cpp
// Prefer small critical sections
xSemaphoreTake(s_mutex, portMAX_DELAY);
state = s_rover_state;           // Read only what you need
xSemaphoreGive(s_mutex);
```

**Atomic operations for simple flags:**
```cpp
s_wifi_connected.store(true, std::memory_order_relaxed);
bool connected = s_wifi_connected.load(std::memory_order_relaxed);
```

### Memory & Performance

**FreeRTOS task stack sizes:**
- Main task: 6144 minimum (configured in sdkconfig)
- HTTP server: 8192
- Use `portSTACK_DEPTH` analysis when adding tasks

**Avoid allocations in hot paths:**
```cpp
// Bad: malloc in loop
uint8_t *buf = (uint8_t *)malloc(size);  // OK for one-time setup

// Good: use static buffers for repeated operations
static char req[256];
snprintf(req, sizeof(req), "...");
```

### Logging (Required)

**All application logs must use structured logger API:**
```cpp
rover_log_field_t fields[] = {
  rover_log_field_int("x", x),
  rover_log_field_str("action", "move"),
};
rover_log_record_t rec = {
  .level = ESP_LOG_INFO,
  .component = TAG,
  .event = "tool_move",
  .fields = fields,
  .field_count = sizeof(fields) / sizeof(fields[0]),
};
rover_log(&rec);
```

**Log event naming:** `snake_case` with domain prefixes:
- `boot_*`, `init_*` — startup events
- `wifi_*`, `syslog_*` — connectivity
- `vision_*`, `ai_*`, `tool_*` — core features
- `web_*`, `button_*` — user interaction
- `fsm_*`, `power_*` — state/power management

**Log field naming:** `snake_case`, stable machine-readable keys.

**Never hand-build JSON strings for logs. Never call `send_syslog()` directly.**

Full schema in `docs/logging-conventions.md`.

### Safety-Critical Patterns

**Emergency stop must be reachable every loop iteration:**
```cpp
while (1) {
  M5.update();
  ai_action_poll_and_execute();
  bool btn_b = M5.BtnB.isPressed();
  
  // BtnB check is FIRST
  if (btn_b) {
    rover_emergency_stop();
    // ...
  }
  vTaskDelay(kLoopPeriod);
}
```

**Use `pdMS_TO_TICKS()` for timing:**
```cpp
vTaskDelay(pdMS_TO_TICKS(50));        // Correct
vTaskDelay(50 / portTICK_PERIOD_MS);  // Also valid but less clear
```

**Never use `delay()` in runtime paths — only `vTaskDelay()`**

---

## Project Structure

```
src/
  main_idf.cpp          # All firmware logic (single-file)
  logger_json.h         # Structured logging API
  logger_json.cpp       # Logger implementation
  CMakeLists.txt        # Build configuration

platformio.ini          # PlatformIO config (m5stick-c, ESP-IDF)
sdkconfig.idf.defaults  # ESP-IDF defaults (2MB flash, optimizations)
include/
  secrets.h             # Wi-Fi credentials (git-ignored, create from .example)
docs/
  logging-conventions.md # Log schema and event naming rules
  plans/                # Firmware design plans
libraries/              # Local reference copies of M5 libraries (read-only)
todo.md                 # Current firmware task spec
```

---

## Subagent Roles

| Role | Responsibility |
|------|----------------|
| `planner` | Converts `todo.md` into explicit steps, acceptance criteria, command checklist |
| `doc-reader` | Reads relevant ESP-IDF docs/examples to extract allowed APIs before changes |
| `firmware-implementer` | Edits `main_idf.cpp`, `logger_json.{h,cpp}`, `platformio.ini`; keeps loops non-blocking |
| `build-runner` | Runs build/flash/monitor workflow; reports failures with actionable fix direction |
| `spec-reviewer` | Checks runtime behavior matches task spec exactly |
| `hardware-diagnostics` | On-device diagnostics when behavior differs from expected |

**Recommended Execution Order:**
1. `planner` → 2. `doc-reader` → 3. `firmware-implementer` → 4. `build-runner` → 5. `spec-reviewer` → 6. `hardware-diagnostics`

---

## Handoff Rules

- **Every implementer handoff:** Include changed files, why changes were made, what API constraints were respected.
- **Every build handoff:** Include command used, pass/fail, key error/output lines.
- **Never skip `doc-reader`** when touching motor/servo/display/power/network logic.
- **When touching logging:** Also read `docs/logging-conventions.md` before editing.

---

## Commit & PR Guidelines

**Commit messages:** Imperative mood with optional scope.
```
firmware: reduce display flicker
wifi: add reconnect timeout handling
vision: add retry logic for capture failures
```

**PRs should include:**
- Purpose and summary
- Hardware/firmware verification steps
- Risk notes (behavioral changes, safety impact)
- Linked issue/task when available

---

## Security Tips

- Never commit secrets or machine-specific credentials
- Treat connected hardware operations as safety-sensitive
- Ensure `BtnB` emergency stop remains immediate after control logic changes
- Validate all user input in HTTP handlers before use
