# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Embedded robotics project for an M5Stack RoverC Pro (K036-B) mecanum-wheel robot controlled by an M5StickC Plus (ESP32-PICO-D4). The repo contains hardware documentation and four Arduino/PlatformIO C++ libraries.

## Build & Flash Commands

There is no root-level build system. Each library is an independent Arduino library. Firmware projects use PlatformIO:

```bash
# Build firmware
pio run

# Flash to device
pio run --target upload

# Serial monitor
pio device monitor

# Arduino CLI alternative (for library examples)
arduino-cli compile --fqbn esp32:esp32:m5stick-c-plus <path-to-ino>
arduino-cli upload --fqbn esp32:esp32:m5stick-c-plus -p /dev/ttyUSB0 <path-to-ino>

# Desktop SDL2 simulation (M5GFX/M5Unified — no hardware needed)
cd libraries/M5GFX/examples/PlatformIO_SDL && pio run -e native
cd libraries/M5Unified/examples/PlatformIO_SDL && pio run -e native
```

No automated tests or linters are configured. Examples in `examples/` directories serve as functional validation.

## Repository Structure

```
docs/                           # Hardware specs (RoverC Pro, M5StickC Plus, UnitV camera)
  plans/                        # Firmware design plans
libraries/
  M5-RoverC/                    # I2C motor/servo control (2 source files + examples)
  M5Unified/                    # Device abstraction for 60+ M5Stack boards
  M5GFX/                        # Graphics/display library (built on LovyanGFX)
  M5-ProductExampleCodes/       # Reference examples (discontinued, read-only)
todo.md                         # Current firmware task spec
```

Each library has its own `CLAUDE.md` with detailed architecture — read those before modifying library code.

## Hardware Target

**Controller:** M5StickC Plus — ESP32, 1.14" TFT (ST7789v2, 135x240), MPU6886 IMU, 120 mAh battery, buttons A/B + power
**Robot base:** RoverC Pro — STM32F030, 4x N20 mecanum motors (L9110S driver), 2 servo ports, gripper, 16340 battery (700 mAh)
**Communication:** I2C at address `0x38`, pins SDA=0/SCL=26, 100-400 kHz

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
User firmware
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
- **C++ standard:** C++14 minimum
- **Naming:** classes `PascalCase` (with prefixes like `Panel_`, `Bus_`), methods `camelCase`, enums `snake_case_t`, namespaces `m5gfx`/`lgfx`
- **Docs are in Russian** — hardware descriptions in `docs/` use Russian language
