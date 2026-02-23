#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rover_log_sink_fn)(const char *json_line, void *ctx);

typedef enum {
  ROVER_LOG_FIELD_STRING = 0,
  ROVER_LOG_FIELD_INT = 1,
  ROVER_LOG_FIELD_BOOL = 2,
} rover_log_field_type_t;

typedef union {
  const char *s;
  int64_t i;
  bool b;
} rover_log_field_value_t;

typedef struct {
  const char *key;
  rover_log_field_type_t type;
  rover_log_field_value_t value;
} rover_log_field_t;

typedef struct {
  esp_log_level_t level;
  const char *component;
  const char *event;  // optional; defaults to "log"
  const rover_log_field_t *fields;
  size_t field_count;
} rover_log_record_t;

static inline rover_log_field_t rover_log_field_str(const char *key, const char *value) {
  rover_log_field_t f;
  f.key = key;
  f.type = ROVER_LOG_FIELD_STRING;
  f.value.s = value;
  return f;
}

static inline rover_log_field_t rover_log_field_int(const char *key, int64_t value) {
  rover_log_field_t f;
  f.key = key;
  f.type = ROVER_LOG_FIELD_INT;
  f.value.i = value;
  return f;
}

static inline rover_log_field_t rover_log_field_bool(const char *key, bool value) {
  rover_log_field_t f;
  f.key = key;
  f.type = ROVER_LOG_FIELD_BOOL;
  f.value.b = value;
  return f;
}

void rover_log_set_sink(rover_log_sink_fn sink, void *ctx);
void rover_log(const rover_log_record_t *record);

#ifdef __cplusplus
}
#endif
