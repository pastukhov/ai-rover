# Repository Guidelines

## Project Structure & Module Organization
- `src/` — firmware source for the M5StickC Plus controller (`main_idf.c`, `CMakeLists.txt`).
- `platformio.ini` — PlatformIO environment and dependency configuration.
- `docs/` — hardware notes and planning artifacts (for example `docs/plans/`).
- `libraries/` — local reference copies of upstream M5 libraries and examples. Treat these primarily as documentation/reference unless a task explicitly requires editing them.
- `todo.md` — current task specification used for implementation scope.
- Build artifacts (`.pio/`, `.pio-core/`) are generated and should not be treated as source.

## Build, Test, and Development Commands
- `pio run` — build firmware and validate compilation.
- `pio run --target upload` — flash firmware to connected device.
- `pio device monitor --baud 115200` — open serial monitor.
- `pio run -t clean` — remove current build outputs.
- `rg "<pattern>" -n` — fast code/text search across the repository.

## Coding Style & Naming Conventions
- Language: C (PlatformIO + ESP-IDF framework).
- Indentation: 2 spaces; keep line endings LF.
- Naming: `snake_case` for functions/variables, `UPPER_SNAKE_CASE` for compile-time constants.
- Prefer non-blocking FreeRTOS task loops; keep watchdog-safe delays (`vTaskDelay`) explicit.
- Use documented ESP-IDF APIs and avoid raw register access in app code unless task explicitly requires it.

## Testing Guidelines
- No formal automated test suite is configured.
- Minimum validation for changes:
  1. `pio run` passes.
  2. Flash succeeds (`pio run --target upload`).
  3. Serial boot log smoke test (`ESP_LOG*` output visible after reset).
  4. Manual hardware smoke test for any enabled peripherals touched by the change.
- If adding scripts/tools, include a small reproducible smoke-check command in `docs/`.

## Observability & Logs
- Primary runtime logs are `ESP_LOG*` messages from ESP-IDF over UART.
- Loki/syslog integration is optional and should be explicitly re-enabled only after ESP-IDF porting of logging stack.

## Subagent Roles
- `planner` — converts `todo.md` into explicit implementation steps, acceptance criteria, and command checklist (`pio run`, upload, monitor).
- `doc-reader` — reads relevant `CLAUDE.md` and ESP-IDF docs/examples to extract allowed APIs before code changes.
- `firmware-implementer` — edits `src/main_idf.c`, `src/CMakeLists.txt`, `platformio.ini`, and sdkconfig defaults; keeps runtime loops non-blocking.
- `build-runner` — runs build/flash/monitor workflow and reports exact failures with actionable fix direction.
- `spec-reviewer` — checks that runtime behavior matches task spec exactly for current ESP-IDF implementation scope.
- `hardware-diagnostics` — focuses on on-device diagnostics (per-motor checks, serial telemetry, power/connectivity assumptions) when behavior differs from expected.

### Recommended Execution Order
1. `planner`
2. `doc-reader`
3. `firmware-implementer`
4. `build-runner`
5. `spec-reviewer`
6. `hardware-diagnostics` (only when needed after functional mismatch on real hardware)

### Handoff Rules
- Every implementer handoff must include changed files, why changes were made, and what API constraints were respected.
- Every build handoff must include command used, pass/fail, and key error/output lines.
- Never skip `doc-reader` when touching motor/servo/display/power/network logic.

## Commit & Pull Request Guidelines
- Commit messages: imperative mood with optional scope, e.g. `firmware: reduce display flicker`.
- Keep commits focused and reversible; avoid unrelated refactors.
- PRs should include:
  - purpose and summary,
  - hardware/firmware verification steps,
  - risk notes (behavioral changes, safety impact),
  - linked issue/task when available.

## Security & Configuration Tips
- Never commit secrets or machine-specific credentials.
- Treat connected hardware operations as safety-sensitive: ensure `BtnB` emergency stop remains immediate after control logic changes.
