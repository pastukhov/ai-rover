# TODO / Next Steps (Post-logging Refactor)

## Current Status
- `task_wdt` issue from periodic vision `PING` in `main_loop` fixed:
  - short ping timeout
  - `vision_ping_task` moved to separate FreeRTOS task
- Unified structured logger implemented:
  - single app-facing API: `rover_log(const rover_log_record_t *record)`
  - same JSON line goes to UART and syslog
  - business logic no longer calls `send_syslog()` directly
- `src/main_idf.cpp` logs migrated to structured events (`event + typed fields`)

## Next Work Items
1. Validate on hardware that `main_loop` watchdog no longer fires during camera offline periods.
2. Review all event names in `src/main_idf.cpp` against `docs/logging-conventions.md` and remove any remaining ambiguous names.
3. Add lightweight log schema checks (host-side script or grep/jq smoke check) to verify:
   - valid JSON per line
   - required top-level keys exist
   - no `message` field
4. Add serial monitor smoke-test instructions for logger verification in docs:
   - UART JSON shape
   - syslog mirror presence
   - expected `vision_status` transitions
5. Consider rate-limiting or dedup for noisy debug events (`vision_ping`, reconnect loops) if logs become too chatty on unstable links.
6. Optionally add a small helper macro set for typed field arrays (ergonomics only, no format changes).

## Vision Capture — AI Tool Integration
7. Add `vision_capture` AI tool: capture JPEG via `vision_capture()` and send to LLM via `openrouter_call_with_image_data()`.
   - Challenge: tool callback runs inside `openrouter_call_with_tools` which already holds `s_ai_mutex`.
   - Need to decide: release mutex before capture, or use separate capture path outside AI tool chain.
   - Also consider heap pressure: 40KB JPEG + LLM request buffer concurrently.

## Hardware Validation Checklist
1. `pio run`
2. `pio run --target upload`
3. `pio device monitor --baud 115200`
4. Confirm no `task_wdt` for `main_loop` while camera is disconnected/unresponsive for >30s.
5. Confirm logs are JSON on UART and syslog with fields:
   - `event`
   - `level`
   - `component`
   - `t_ms`
   - `fields`

## Constraints (Keep)
- App code logs only through `rover_log(...)`.
- No manual JSON construction in business logic.
- No direct `send_syslog()` calls outside logging transport/sink code.
