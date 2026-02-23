#include "logger_json.h"

#include <stdio.h>
#include <string.h>

static rover_log_sink_fn s_sink = NULL;
static void *s_sink_ctx = NULL;

void rover_log_set_sink(rover_log_sink_fn sink, void *ctx) {
  s_sink = sink;
  s_sink_ctx = ctx;
}

static size_t json_escape_copy_local(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) return 0;
  size_t j = 0;
  for (size_t i = 0; src && src[i] != '\0'; ++i) {
    const char *esc = NULL;
    char esc_buf[7];
    switch (src[i]) {
      case '\\': esc = "\\\\"; break;
      case '"': esc = "\\\""; break;
      case '\n': esc = "\\n"; break;
      case '\r': esc = "\\r"; break;
      case '\t': esc = "\\t"; break;
      default:
        if ((unsigned char)src[i] < 0x20) {
          snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", (unsigned char)src[i]);
          esc = esc_buf;
        }
        break;
    }
    if (esc != NULL) {
      size_t n = strlen(esc);
      if (j + n >= dst_size) break;
      memcpy(&dst[j], esc, n);
      j += n;
    } else {
      if (j + 1 >= dst_size) break;
      dst[j++] = src[i];
    }
  }
  dst[j] = '\0';
  return j;
}

static const char *log_level_name(esp_log_level_t level) {
  switch (level) {
    case ESP_LOG_ERROR: return "error";
    case ESP_LOG_WARN: return "warn";
    case ESP_LOG_INFO: return "info";
    case ESP_LOG_DEBUG: return "debug";
    case ESP_LOG_VERBOSE: return "verbose";
    default: return "unknown";
  }
}

static void emit_json_line(esp_log_level_t level, const char *component, const char *json_line) {
  esp_log_write(level, component ? component : "", "%s", json_line ? json_line : "{}");
  if (s_sink != NULL && json_line != NULL) {
    s_sink(json_line, s_sink_ctx);
  }
}

void rover_log(const rover_log_record_t *record) {
  if (record == NULL) return;

  const char *component = record->component ? record->component : "";
  const char *event = record->event ? record->event : "log";

  char comp_escaped[96];
  char event_escaped[96];
  (void)json_escape_copy_local(comp_escaped, sizeof(comp_escaped), component);
  (void)json_escape_copy_local(event_escaped, sizeof(event_escaped), event);

  uint32_t t_ms = (uint32_t)(esp_log_timestamp() & 0xffffffffu);

  char buf[896];
  int n = snprintf(buf, sizeof(buf),
                   "{\"event\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"t_ms\":%lu",
                   event_escaped, log_level_name(record->level), comp_escaped, (unsigned long)t_ms);
  if (n <= 0 || n >= (int)sizeof(buf)) {
    emit_json_line(ESP_LOG_ERROR, component,
                   "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                   "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
    return;
  }

  size_t used = (size_t)n;
  if (record->fields != NULL && record->field_count > 0) {
    int f0 = snprintf(buf + used, sizeof(buf) - used, ",\"fields\":{");
    if (f0 <= 0 || (size_t)f0 >= sizeof(buf) - used) {
      emit_json_line(ESP_LOG_ERROR, component,
                     "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                     "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
      return;
    }
    used += (size_t)f0;

    for (size_t i = 0; i < record->field_count; ++i) {
      const rover_log_field_t *f = &record->fields[i];
      if (f->key == NULL || f->key[0] == '\0') continue;

      char key_escaped[64];
      (void)json_escape_copy_local(key_escaped, sizeof(key_escaped), f->key);

      if (used + 1 >= sizeof(buf)) {
        emit_json_line(ESP_LOG_ERROR, component,
                       "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                       "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
        return;
      }
      if (i != 0) buf[used++] = ',';

      int fk = snprintf(buf + used, sizeof(buf) - used, "\"%s\":", key_escaped);
      if (fk <= 0 || (size_t)fk >= sizeof(buf) - used) {
        emit_json_line(ESP_LOG_ERROR, component,
                       "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                       "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
        return;
      }
      used += (size_t)fk;

      if (f->type == ROVER_LOG_FIELD_STRING) {
        char val_escaped[320];
        (void)json_escape_copy_local(val_escaped, sizeof(val_escaped),
                                     f->value.s ? f->value.s : "");
        int fv = snprintf(buf + used, sizeof(buf) - used, "\"%s\"", val_escaped);
        if (fv <= 0 || (size_t)fv >= sizeof(buf) - used) {
          emit_json_line(ESP_LOG_ERROR, component,
                         "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                         "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
          return;
        }
        used += (size_t)fv;
      } else if (f->type == ROVER_LOG_FIELD_INT) {
        int fv = snprintf(buf + used, sizeof(buf) - used, "%lld", (long long)f->value.i);
        if (fv <= 0 || (size_t)fv >= sizeof(buf) - used) {
          emit_json_line(ESP_LOG_ERROR, component,
                         "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                         "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
          return;
        }
        used += (size_t)fv;
      } else if (f->type == ROVER_LOG_FIELD_BOOL) {
        const char *bv = f->value.b ? "true" : "false";
        int fv = snprintf(buf + used, sizeof(buf) - used, "%s", bv);
        if (fv <= 0 || (size_t)fv >= sizeof(buf) - used) {
          emit_json_line(ESP_LOG_ERROR, component,
                         "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                         "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
          return;
        }
        used += (size_t)fv;
      }
    }

    if (used + 1 >= sizeof(buf)) {
      emit_json_line(ESP_LOG_ERROR, component,
                     "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                     "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
      return;
    }
    buf[used++] = '}';
  }
  if (used + 2 >= sizeof(buf)) {
    emit_json_line(ESP_LOG_ERROR, component,
                   "{\"event\":\"logger_error\",\"level\":\"error\",\"component\":\"logger_json\","
                   "\"fields\":{\"code\":\"json_wrap_truncated\"}}");
    return;
  }
  buf[used++] = '}';
  buf[used] = '\0';

  emit_json_line(record->level, component, buf);
}
