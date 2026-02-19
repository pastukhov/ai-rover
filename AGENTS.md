# Repository Guidelines

## Project Structure & Module Organization
- `src/` — firmware source for the M5StickC Plus controller (`main.cpp`).
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
- Language: C++14 (PlatformIO + Arduino framework).
- Indentation: 2 spaces; keep line endings LF.
- Naming: `camelCase` for functions/variables, `PascalCase` for types, `kPrefix` for constants (for example `kHeartbeatMs`).
- Prefer concise, state-driven logic in `loop()` and avoid blocking delays for runtime control paths.
- Use only documented library APIs (especially `M5_RoverC` and `M5Unified`) and avoid direct low-level I2C register writes in app code.

## Testing Guidelines
- No formal automated test suite is configured.
- Minimum validation for changes:
  1. `pio run` passes.
  2. Flash succeeds (`pio run --target upload`).
  3. Manual smoke test on hardware (buttons, movement, display, battery text, emergency stop).
- If adding scripts/tools, include a small reproducible smoke-check command in `docs/`.

## Subagent Roles
- `planner` — converts `todo.md` into explicit implementation steps, acceptance criteria, and command checklist (`pio run`, upload, monitor).
- `doc-reader` — reads all relevant `CLAUDE.md` and library headers/examples to extract allowed APIs before code changes.
- `firmware-implementer` — edits `src/main.cpp` and `platformio.ini`, implements behavior, keeps loop non-blocking, and preserves emergency stop responsiveness.
- `build-runner` — runs build/flash/monitor workflow and reports exact failures with actionable fix direction.
- `spec-reviewer` — checks that runtime behavior matches task spec exactly (button mapping, timing sequence, display requirements).
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
- Never skip `doc-reader` when touching motor/servo/display/power logic.

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
