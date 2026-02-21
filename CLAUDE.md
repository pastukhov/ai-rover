# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Embedded robotics project for an M5Stack RoverC Pro (K036-B) mecanum-wheel robot controlled by an M5StickC Plus (ESP32-PICO-D4). The firmware provides button-triggered demo sequences, emergency stop, on-screen status, and Wi-Fi web control.

## Build & Flash Commands

```bash
# Prerequisites: create Wi-Fi credentials from template
cp include/secrets.h.example include/secrets.h   # then edit with your SSID/password

# Build firmware
pio run

# Flash to device
pio run --target upload

# Serial monitor (115200 baud)
pio device monitor --baud 115200

# Clean build artifacts
pio run -t clean
```

No automated tests or linters. Validation is: `pio run` compiles → flash → manual smoke test on hardware.

## Repository Structure

```
src/main.cpp                    # All firmware logic (single-file)
platformio.ini                  # PlatformIO config (m5stick-c, M5Unified + M5-RoverC deps)
include/secrets.h               # Wi-Fi credentials (git-ignored, create from .example)
docs/                           # Hardware specs in Russian (RoverC Pro, M5StickC Plus, UnitV)
  plans/                        # Firmware design plans
libraries/                      # Local reference copies of M5 libraries (git-ignored, read-only)
AGENTS.md                       # Subagent roles and execution order for multi-agent workflows
todo.md                         # Current firmware task spec
```

Each library in `libraries/` has its own `CLAUDE.md` with detailed architecture — read those before modifying library code.

## Firmware Architecture (`src/main.cpp`)

Single-file, state-machine-driven firmware. Everything runs in a non-blocking `loop()` — no `delay()` in runtime paths.

**Core state machines:**
- **Demo sequence** — 7-step timed sequence (forward/stop/backward/stop/gripper open/close/idle), triggered by BtnA. Durations in `kStepDurationMs[]`, steps applied by `applyStep()`.
- **Motor diagnostic** — per-motor test cycle (run/stop each motor sequentially), triggered via web command.
- **Motion refresh** — active motion commands are re-sent every 50ms (`kMotionRefreshMs`) to keep RoverC alive.

**Controls:**
- BtnA → start demo sequence
- BtnB → emergency stop (motors off immediately, all state machines halted)
- Wi-Fi web UI → movement, gripper, demo, emergency (HTTP on port 80, `GET /cmd?act=...`)

**Display:** dirty-flag rendering — only redraws when action, battery, or Wi-Fi status changes.

**Safety invariant:** `emergencyStop()` is reachable every `loop()` iteration. BtnB check is first in `loop()`.

## Hardware Target

**Controller:** M5StickC Plus — ESP32, 1.14" TFT (135x240), MPU6886 IMU, buttons A/B + power
**Robot base:** RoverC Pro — STM32F030, 4x N20 mecanum motors, 2 servo ports, gripper
**Communication:** I2C at address `0x38`, pins SDA=0/SCL=26

### I2C Register Map (0x38)

| Register | Size | Function | Range |
|----------|------|----------|-------|
| `0x00-0x03` | 4 × int8 | Motor 1-4 speed | -127 to +127 |
| `0x10-0x11` | 2 × uint8 | Servo 1-2 angle | 0-180° |
| `0x20-0x23` | 2 × uint16 (big-endian) | Servo 1-2 pulse width | 500-2500 µs |

### Mecanum Kinematics (setSpeed)

`setSpeed(x, y, z)` where x=strafe, y=forward/back, z=rotation. When z≠0, x and y are scaled by `(100-|z|)/100`. Motor mixing: `[y+x-z, y-x+z, y-x-z, y+x+z]`, clamped to [-100, 100].

## Library Stack

```
User firmware (src/main.cpp)
  → M5_RoverC        (I2C motor/servo commands)
  → M5Unified        (hardware abstraction: display, buttons, IMU, power, speaker)
    → M5GFX           (graphics engine, board auto-detection)
      → LovyanGFX      (embedded rendering core, FreeBSD license)
  → ESP32 Arduino / ESP-IDF
```

## Key Patterns & Conventions

- **Prefer `M5Unified.h`** over device-specific headers (`M5StickC.h`, `M5StickCPlus.h`) for new code
- **Singleton `M5`** object — access peripherals via `M5.Display`, `M5.Speaker`, `M5.Imu`, `M5.Power`
- **All motor/servo I2C** goes through `M5_RoverC` class — never write to I2C registers directly in firmware
- **Board-specific behavior** uses `switch` on `board_t` enum, not `#ifdef` per device
- **Chip-specific compilation** uses `#if defined(CONFIG_IDF_TARGET_ESP32)`, not device-level ifdefs
- **Config pattern:** nested `config_t` structs for initialization
- **Non-blocking loop:** use `millis()` timers, never `delay()` in runtime paths
- **C++ standard:** C++14 minimum
- **Naming:** classes `PascalCase`, methods `camelCase`, constants `kPascalCase`, enums `snake_case_t`
- **Docs are in Russian** — hardware descriptions in `docs/` use Russian language
