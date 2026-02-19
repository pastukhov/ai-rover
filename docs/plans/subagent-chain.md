# RoverC Pro Firmware Subagent Chain

## Initial Chain (Before Reading All CLAUDE.md)

1. `planner`:
   - Parse `todo.md`.
   - Define required deliverables (firmware behavior, display, controls, build/flash steps).
2. `doc-reader`:
   - Find and read all `CLAUDE.md` files in root, `libraries/`, and `.pio/libdeps/`.
   - Extract only allowed APIs and constraints.
3. `firmware-implementer`:
   - Build PlatformIO project skeleton.
   - Implement sequence state machine, button controls, display status, battery display.
4. `spec-reviewer`:
   - Check behavior against `todo.md` step-by-step and timing-by-timing.
5. `quality-reviewer`:
   - Check safety behavior (immediate stop), non-blocking loop, and readability.
6. `build-runner`:
   - Run `pio run`, report/fix compile errors, then run upload/monitor commands.

## Reviewed Chain (After Reading All CLAUDE.md)

Based on the documented stack (`M5_RoverC` + `M5Unified` + `M5GFX`) and constraints:

1. `doc-reader` (must run first):
   - Confirm `M5_RoverC` API: `begin`, `setSpeed`, `setServoAngle`, `setPulse` variants.
   - Confirm `M5Unified` usage pattern: `M5.begin`, `M5.update`, `M5.Display`, `M5.Power`.
   - Confirm no direct I2C register access in user firmware.
2. `firmware-implementer`:
   - Use only library APIs, no raw I2C writes.
   - Implement non-blocking sequence progression using `millis()`.
   - Implement emergency stop with motor shutdown path reachable every loop iteration.
3. `spec-reviewer` (before quality review):
   - Validate exact demo order and durations:
     - forward 1s, stop 0.5s, backward 1s, stop 0.5s, gripper open 0.5s, gripper close 0.5s, idle.
   - Validate button mapping (A start, B emergency stop) and screen requirements.
4. `quality-reviewer`:
   - Validate loop responsiveness, state transitions, and serial observability.
   - Validate startup safety (motors stopped at boot).
5. `build-runner`:
   - Run `pio run`.
   - If compile passes and hardware is connected: `pio run --target upload`, then `pio device monitor`.

## Why Chain Was Updated

- The CLAUDE docs clarified that all robot control must pass through `M5_RoverC` instead of low-level I2C.
- The CLAUDE docs also reinforced the unified entry points (`M5.Display`, `M5.Power`, button updates via `M5.update`), so the chain now enforces API compliance before any code review.
- Spec compliance review is explicitly ordered before quality review.

