# Logging Conventions (UART + Syslog)

This project uses a single structured logger (`rover_log`) that emits the same JSON line to:

- UART (`esp_log_write`)
- syslog (via logger sink -> `send_syslog()`)

## Canonical JSON Shape

Every log line must be a single JSON object with this shape:

```json
{
  "event": "snake_case_event_name",
  "level": "debug|info|warn|error",
  "component": "ai-rover-idf",
  "t_ms": 12345,
  "fields": {
    "...": "typed values only"
  }
}
```

Rules:

- `message` field is forbidden.
- `fields` should always be present for normal app logs (empty object is acceptable if needed).
- Use JSON native types: string / integer / boolean.
- Application logs should be fully structured (domain fields, not human text).
- `fields.text` is reserved for exceptional/internal logger diagnostics only.

## Event Naming

Use `snake_case` and prefer domain prefixes.

Recommended prefixes:

- `boot_*` / `init_*`
- `wifi_*`
- `syslog_*`
- `vision_*`
- `ai_*`
- `web_*`
- `tool_*`
- `button_*`
- `fsm_*`
- `power_*`

Recommended suffix patterns:

- `_start`
- `_ok`
- `_failed`
- `_timeout`
- `_initialized`
- `_available`
- `_done`

## Field Naming

- Use `snake_case` keys.
- Prefer stable names over human phrasing (`max_retry`, not `max retries`).
- Include machine-usable outcome fields (`status`, `err`, `code`) instead of encoding status in `event` only when useful.
- Put verbose/raw payloads behind explicit keys (`resp`, `raw`, `text`).

## Examples

Wi-Fi reconnect attempt:

```json
{"event":"wifi_reconnect_attempt","level":"warn","component":"ai-rover-idf","t_ms":1234,"fields":{"retry":3,"max_retry":20}}
```

Vision ping bad response:

```json
{"event":"vision_ping","level":"debug","component":"ai-rover-idf","t_ms":1234,"fields":{"result":"bad_response","resp_len":42}}
```

Heartbeat:

```json
{"event":"heartbeat","level":"info","component":"ai-rover-idf","t_ms":1234,"fields":{"state":"IDLE","moving":0,"x":0,"y":0,"z":0,"gripper":"open","bat_pct":81}}
```

## Implementation Rule

Application code should log via typed records (`rover_log_record_t` + `rover_log_field_t`).

- Do not build JSON strings manually for logging.
- Do not call `send_syslog()` directly from business logic.
- `send_syslog()` is transport-only (internal sink path).
- Prefer domain events over generic `log` events.
