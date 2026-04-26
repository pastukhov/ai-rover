# ESP-Claw Migration Plan

## Goal

Move AI Rover from the current custom `ollama.cpp` tool loop toward ESP-Claw's agent runtime while preserving existing robot behavior, safety controls, and buildability on M5StickC Plus + RoverC Pro.

This is a migration plan, not an instruction to replace the firmware with `esp-claw/application/basic_demo`.

## Context Snapshot

Current firmware:
- Target: M5StickC Plus / ESP32-PICO-D4 with `2MB` flash.
- Build system: PlatformIO with ESP-IDF framework.
- Main application: `src/main_idf.cpp`.
- Current AI path: custom `ollama.cpp` client + `ollama_simple_function_t` tool callbacks.
- Existing tools: `move`, `turn`, `stop`, `gripper_open`, `gripper_close`, `read_imu`, `vision_scan`, `vision_capture`.
- Safety-critical behavior: BtnB emergency stop and gripper toggle in `main_loop_task`, independent of LLM success.
- Current partition table has no FATFS storage partition.

ESP-Claw provides:
- `claw_core`: LLM request loop, context assembly, tool-call iteration, cancellation.
- `claw_cap`: capability/tool registry, grouping, lifecycle, LLM-visible tool list.
- `claw_skill`: skill metadata and progressive context disclosure.
- `claw_memory`: session and long-term memory, requiring filesystem-backed runtime data.
- `claw_event_router`: deterministic event routing and automation.
- Many optional capabilities: Lua, files, scheduler, IM, MCP, web search, time, system info.

Key constraint:
- ESP-Claw is a full agent framework, not a servo/gripper library. For AI Rover, the right integration shape is a custom `cap_ai_rover` capability group around existing rover primitives.

## Progress Snapshot

Updated: 2026-04-26

- `P0` baseline build recorded: `pio run` passes, RAM `43,956 / 327,680`, flash `1,017,979 / 2,031,616`.
- `P0` external ESP-Claw component-dir spike failed under PlatformIO main-target discovery; separate ESP-IDF component targets generated, but PlatformIO did not proceed.
- `P0` fallback spike succeeded by compiling the minimal ESP-Claw sources through the main `src` component.
- `P1` initial `cap_ai_rover` group added and registered from `init_ai()` without switching chat execution to `claw_core`.
- `P1` `cap_ai_rover` was split out into `src/cap_ai_rover.{h,cpp}` with `main_idf.cpp` providing only the callback table.
- `P1` `cap_ai_rover` now exposes both legacy tool names (`move`, `turn`, `stop`) and explicit aliases (`rover_move`, `rover_turn`, `rover_stop`) for prompt compatibility.
- `P2` `AI_ROVER_USE_ESP_CLAW_CORE` compile-time guard wired through `chat_worker_task()`. Default remains `0`, so web chat still uses the existing Ollama path.
- `P2` compile-check with `AI_ROVER_USE_ESP_CLAW_CORE=1` passes: RAM `44,076 / 327,680`, flash `1,120,459 / 2,031,616`.
- `P2` **`AI_ROVER_USE_ESP_CLAW_CORE=1` enabled by default** via `build_flags` in `platformio.ini`. Readiness checks updated to use `s_esp_claw_core_ready` instead of `s_ai == NULL` in `chat_worker_task` and web chat HTTP handler. Build passes: RAM `44,076 / 327,680`, flash `1,120,463 / 2,031,616`. **Pending hardware smoke test** (flash when M5StickC Plus connected).
- Current build with ESP-Claw core active: RAM `44,076 / 327,680`, flash `1,120,463 / 2,031,616`.

## Migration Principles

1. Preserve the hardware control path first.
2. Keep BtnB emergency stop outside the agent runtime.
3. Avoid pulling in ESP-Claw demo application wholesale.
4. Start with the smallest useful ESP-Claw subset.
5. Measure firmware size and heap before adding memory, skills, Lua, IM, or MCP.
6. Keep every stage buildable and hardware-testable.
7. Treat motor, servo, display, power, Wi-Fi, and logging changes as safety-sensitive.

## Priority Legend

- `P0`: Required before any real migration can safely proceed.
- `P1`: Minimal viable ESP-Claw integration.
- `P2`: Feature parity with current AI Rover behavior.
- `P3`: Optional ESP-Claw features after size and stability are proven.

## Phase 0 - Baseline and Feasibility (`P0`)

### Task 0.1 - Record Current Firmware Baseline

Priority: `P0`

Actions:
- Run `pio run`.
- Record binary size, RAM usage, and warnings.
- Record current `partitions.csv` constraints.
- Capture current tool list and prompt from `init_ai()`.
- Capture current runtime task layout and stack sizes.

Acceptance criteria:
- Build result is documented.
- Firmware size headroom is known.
- Current behavior baseline is available for regression checks.

Notes:
- This task should not modify firmware code.
- If the baseline build fails, fix or document the existing failure before migration.

### Task 0.2 - Define Minimal ESP-Claw Component Set

Priority: `P0`

Candidate minimal set:
- `claw_core`
- `claw_cap`

Candidate deferred set:
- `claw_skill`
- `claw_memory`
- `claw_event_router`
- `cap_lua`
- `cap_files`
- `cap_system`
- `cap_time`
- IM/MCP/search/scheduler capabilities

Actions:
- Inspect `esp-claw/components/claw_modules/claw_core/CMakeLists.txt`.
- Inspect `esp-claw/components/claw_modules/claw_cap/CMakeLists.txt`.
- List transitive ESP-IDF dependencies.
- Confirm whether `claw_core` can run without FATFS, skills, and memory callbacks.

Acceptance criteria:
- A concrete component include list exists.
- All deferred components are explicitly marked as out of scope for the first spike.
- Any hard dependency on FATFS or demo settings is identified.

### Task 0.3 - Build-System Spike

Priority: `P0`

Actions:
- Try adding ESP-Claw minimal components to the current PlatformIO/ESP-IDF build.
- Prefer local component inclusion rather than copying demo app code.
- Do not wire runtime behavior yet.
- Keep the change reversible.

Acceptance criteria:
- `pio run` either passes with linked minimal ESP-Claw components or fails with documented blocker.
- Firmware size delta is recorded.
- Dependency conflicts are documented.

Risks:
- ESP-Claw may assume ESP-IDF version or CMake layout closer to native `idf.py`.
- PlatformIO component discovery may require explicit `EXTRA_COMPONENT_DIRS` or CMake changes.
- ESP-Claw may pull more code than expected through transitive dependencies.

Decision gate:
- If minimal ESP-Claw components cannot fit or link cleanly, stop and choose between:
  - refactoring current `ollama.cpp` path only, or
  - moving to native ESP-IDF build, or
  - moving to hardware with more flash.

## Phase 1 - Isolate Rover Control API (`P1`)

### Task 1.1 - Extract Hardware Control Facade

Priority: `P1`

Target:
- Create a small internal API that exposes rover actions without exposing `main_idf.cpp` internals to ESP-Claw capabilities.

Proposed API surface:
- `rover_control_move(x, y, z, duration_ms)`
- `rover_control_turn(direction, angle_deg, speed_percent)`
- `rover_control_stop()`
- `rover_control_gripper_open()`
- `rover_control_gripper_close()`
- `rover_control_read_imu(...)`
- `rover_control_vision_scan(...)`
- `rover_control_vision_capture(...)`

Actions:
- Keep actual side effects on the existing Core 0 action queue path.
- Preserve existing timeout behavior.
- Preserve structured logging through `rover_log(...)`.
- Keep emergency stop direct and immediate.

Acceptance criteria:
- AI/tool code can request robot actions through a facade.
- No new direct motor/servo side effects are introduced outside the established execution path.
- `pio run` passes.

Risks:
- `main_idf.cpp` is currently large and tightly coupled; extraction should be incremental.
- Over-extraction can create churn. Keep the facade narrow.

### Task 1.2 - Preserve Current Tool Responses

Priority: `P1`

Actions:
- Define a stable result format for action facade calls.
- Keep existing status strings where practical: `ok`, `busy`, `timeout`, `failed`, `unavailable`, `cancelled`.
- Document mapping from old callbacks to new capability outputs.

Acceptance criteria:
- The LLM-facing behavior remains close to current `make_tool_response(...)` output.
- Web and button behavior are unaffected.

## Phase 2 - Implement `cap_ai_rover` (`P1`)

### Task 2.1 - Scaffold Custom Capability Group

Priority: `P1`

Proposed location:
- `src/cap_ai_rover.h`
- `src/cap_ai_rover.cpp` or `src/cap_ai_rover.c`

Capability group:
- `group_id`: `cap_ai_rover`
- `plugin_name`: `ai-rover`
- `version`: project version or static `0.1.0`

Capabilities:
- `rover_move`
- `rover_turn`
- `rover_stop`
- `gripper_open`
- `gripper_close`
- `read_imu`
- `vision_scan`
- `vision_capture`

Actions:
- Register descriptors with `CLAW_CAP_FLAG_CALLABLE_BY_LLM`.
- Provide compact JSON schemas.
- Keep descriptions short and operational.
- Parse input JSON with `cJSON`.
- Return output text/JSON via ESP-Claw's caller-provided output buffer.

Acceptance criteria:
- `cap_ai_rover_register_group()` can be called after `claw_cap_init()`.
- Capability descriptors are visible through `claw_cap_list()`.
- `pio run` passes.

### Task 2.2 - Map Existing Tool Parameters

Priority: `P1`

Mapping:
- old `move` -> new `rover_move`
- old `turn` -> new `rover_turn`
- old `stop` -> new `rover_stop`
- old `gripper_open` -> same name
- old `gripper_close` -> same name
- old `read_imu` -> same name
- old `vision_scan` -> same name
- old `vision_capture` -> same name

Parameter compatibility:
- `rover_move`: `x`, `y`, `z`, `duration_ms`
- `rover_turn`: `direction`, `angle_deg`, `speed_percent`
- `vision_capture`: `question`, `quality`

Acceptance criteria:
- Existing prompt can be adapted without changing task semantics.
- The LLM still has explicit movement, turn, stop, gripper, IMU, and vision tools.

### Task 2.3 - Capability-Level Safety Rules

Priority: `P1`

Actions:
- Clamp numeric inputs in capability layer before submitting actions.
- Reject invalid JSON with `ESP_ERR_INVALID_ARG`.
- Return explicit error text on invalid parameters.
- Ensure `rover_stop` has shortest possible path through the queue.
- Do not allow capability calls to hold `s_state_mutex` while waiting for completion.

Acceptance criteria:
- Invalid tool args cannot cause unsafe motor commands.
- Stop remains responsive.
- Tool calls have bounded wait time.

## Phase 3 - Wire Minimal ESP-Claw Runtime (`P1`)

### Task 3.1 - Add Runtime Initialization

Priority: `P1`

Target:
- Replace `init_ai()` internals with a minimal ESP-Claw runtime path, behind a compile-time or runtime guard if needed.

Actions:
- Call `claw_cap_init()`.
- Register `cap_ai_rover`.
- Set LLM-visible groups to only `cap_ai_rover` initially.
- Configure `claw_core_config_t` with:
  - OpenAI-compatible backend if using current Ollama/OpenRouter-style endpoint.
  - Existing system prompt adapted for capability names.
  - `call_cap = claw_cap_call_from_core`.
  - bounded queue lengths and task stack size.
  - bounded timeout.
- Register `claw_cap_tools_provider` as context provider.
- Start `claw_core`.

Acceptance criteria:
- Firmware boots with ESP-Claw core enabled.
- A simple text prompt can trigger `rover_stop`.
- `pio run` passes.

Risks:
- Current secrets/config path may not map directly to ESP-Claw settings.
- TLS/HTTP stack size may increase.
- `claw_core` stack and heap usage may exceed current margins.

### Task 3.2 - Adapt Chat Request Path

Priority: `P1`

Actions:
- Replace direct `ollama_chat(...)` usage with `claw_core_submit(...)` and `claw_core_receive_for(...)`.
- Preserve existing web chat endpoint behavior if present.
- Preserve response rendering on display/web.
- Add cancellation path for emergency stop if an AI request is in flight.

Acceptance criteria:
- Existing user-facing chat flow still returns assistant text.
- Tool calls execute through `claw_cap`.
- Failed AI requests do not leave rover moving.

### Task 3.3 - Remove or Guard Old AI Client

Priority: `P1`

Actions:
- Keep old `ollama.cpp` path temporarily behind a build flag until ESP-Claw path passes hardware smoke.
- After parity is confirmed, remove old path in a separate cleanup task.

Acceptance criteria:
- There is a clear single active AI path in production build.
- Rollback remains possible during migration.

## Phase 4 - Feature Parity Validation (`P2`)

### Task 4.1 - Tool Parity Smoke Tests

Priority: `P2`

Manual tests:
- Ask rover to stop.
- Ask rover to move forward briefly.
- Ask rover to turn left 90 degrees.
- Ask rover to open gripper.
- Ask rover to close gripper.
- Ask rover to read IMU.
- Ask rover to scan scene.
- Ask rover to capture and inspect image.

Acceptance criteria:
- Each command produces expected physical behavior or clear error.
- BtnB still immediately stops motion.
- No watchdog resets.
- JSON logs remain structured.

### Task 4.2 - Regression Checks for Non-AI Control Paths

Priority: `P2`

Manual tests:
- BtnA behavior.
- BtnB emergency stop and gripper toggle.
- Web UI movement.
- Web UI gripper controls.
- Status endpoint.
- Display status updates.
- Wi-Fi connect/disconnect behavior.

Acceptance criteria:
- Non-AI controls behave as before migration.
- ESP-Claw task failures do not break local control.

### Task 4.3 - Resource Monitoring

Priority: `P2`

Actions:
- Record boot heap.
- Record heap before and after one AI request.
- Record stack high-water marks for core tasks if available.
- Record firmware binary size.

Acceptance criteria:
- Heap does not monotonically shrink after repeated requests.
- Stack headroom is acceptable.
- Firmware still fits the partition with margin.

## Phase 5 - Optional ESP-Claw Features (`P3`)

### Task 5.1 - Skills

Priority: `P3`

Purpose:
- Let the model progressively load detailed guidance instead of placing every rover rule in the base prompt.

Requirements:
- FATFS or another storage strategy for skills.
- `claw_skill` initialization.
- Skill manifest and markdown docs for AI Rover operation.

Recommended skill:
- `ai_rover_ops`: movement rules, search behavior, vision limitations, safety instructions.

Acceptance criteria:
- Base context is smaller.
- Activating the rover skill exposes useful guidance without bloating every prompt.

### Task 5.2 - Memory

Priority: `P3`

Purpose:
- Store session history and optional user preferences.

Requirements:
- Storage partition.
- `claw_memory` setup.
- Clear policy for what should and should not be remembered.

Risks:
- Likely too large for current `2MB` flash unless other components are removed.
- More writes to flash.

Acceptance criteria:
- Memory works without destabilizing boot or runtime.
- Memory use is explicitly user-beneficial.

### Task 5.3 - Event Router

Priority: `P3`

Purpose:
- Route sensor/button/time events into deterministic automations or agent runs.

Possible uses:
- Scheduled self-check.
- Vision status changes.
- Low battery events.
- Button-driven agent prompt.

Acceptance criteria:
- Event routing is deterministic.
- Safety-critical stop remains direct, not routed through agent.

### Task 5.4 - Lua Runtime

Priority: `P3`

Purpose:
- Allow scripted behaviors.

Risks:
- Large footprint.
- Safety risk if scripts can directly drive motors without guardrails.

Acceptance criteria:
- Lua can only call bounded, safe rover capabilities.
- Emergency stop remains external and immediate.

## Phase 6 - Cleanup and Documentation (`P2`)

### Task 6.1 - Remove Dead AI Path

Priority: `P2`

Actions:
- Remove old `ollama.cpp` only after ESP-Claw path passes parity validation.
- Remove unused structs, callbacks, and prompt wiring.
- Keep commit separate from initial integration.

Acceptance criteria:
- There is no duplicate AI tool implementation.
- Build passes.
- Diff is reviewable.

### Task 6.2 - Update Docs

Priority: `P2`

Files to update:
- `README.md`
- `CLAUDE.md`
- `AGENTS.md` if workflow expectations change
- `docs/logging-conventions.md` only if log events change

Required doc content:
- New AI architecture overview.
- Capability list.
- Build flags if both old and new paths coexist temporarily.
- Hardware smoke test checklist.
- Known limitations.

Acceptance criteria:
- New contributors can understand which agent runtime is active.
- Safety and hardware validation steps are documented.

## Suggested Execution Order

1. `P0` baseline and minimal build spike.
2. `P1` rover control facade.
3. `P1` `cap_ai_rover` implementation.
4. `P1` minimal `claw_core` wiring.
5. `P2` parity validation.
6. `P2` remove old AI path.
7. `P3` skills/memory/event router/Lua only after size and stability are proven.

## Command Checklist

Baseline:
```bash
pio run
pio run --target size
rg -n "init_ai|ollama_simple_function_t|cb_move|cb_turn|cb_stop|cb_gripper|vision_capture" src/main_idf.cpp src/ollama.*
```

ESP-Claw inspection:
```bash
rg -n "idf_component_register|REQUIRES|PRIV_REQUIRES" esp-claw/components/claw_modules/claw_core esp-claw/components/claw_modules/claw_cap
sed -n '1,220p' esp-claw/components/claw_modules/claw_core/include/claw_core.h
sed -n '1,220p' esp-claw/components/claw_modules/claw_cap/include/claw_cap.h
```

Hardware smoke:
```bash
pio run --target upload
pio device monitor --baud 115200
```

## Risk Register

| Risk | Priority | Mitigation |
| --- | --- | --- |
| Firmware no longer fits `2MB` flash | `P0` | Measure minimal component size before runtime migration |
| ESP-Claw assumes native ESP-IDF app layout | `P0` | Do a build-system spike before refactoring AI tools |
| FATFS requirements collide with current partition table | `P0` | Defer skills/memory/files until after minimal core works |
| Emergency stop becomes dependent on agent task | `P1` | Keep BtnB and direct stop path outside ESP-Claw |
| Tool callbacks reintroduce hardware side effects on wrong core | `P1` | Route capability execution through existing action queue/facade |
| LLM-visible tool names change behavior | `P2` | Preserve prompt semantics and validate every current tool |
| Heap/stack regressions cause WDT resets | `P2` | Track heap and stack after each migration stage |
| Optional Lua/scripts bypass safety limits | `P3` | Only expose bounded rover capabilities to scripts |

## Open Questions

- Can `claw_core + claw_cap` fit comfortably in the current `2MB` firmware partition?
- Does ESP-Claw's OpenAI-compatible backend support the exact endpoint currently used by AI Rover?
- Should migration keep PlatformIO or move to native `idf.py` if ESP-Claw integration becomes awkward?
- Is M5StickC Plus the final target, or is larger-flash ESP32-S3 hardware acceptable if full ESP-Claw features are desired?
- Do we want ESP-Claw skills/memory on-device, or is the minimal capability runtime enough?

## Definition of Done

Migration is complete when:
- ESP-Claw is the single active AI tool/runtime path.
- `cap_ai_rover` exposes all current rover tools.
- Existing button, web, display, motion, gripper, IMU, and vision behavior are preserved.
- BtnB emergency stop remains immediate and independent of LLM/network state.
- `pio run` passes.
- Hardware smoke test passes.
- Docs describe the new architecture and validation steps.
