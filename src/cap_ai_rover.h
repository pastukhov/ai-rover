#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef char *(*cap_ai_rover_tool_callback_t)(const char *fn, const char *arguments, void *ud);

typedef struct {
  cap_ai_rover_tool_callback_t move;
  cap_ai_rover_tool_callback_t turn;
  cap_ai_rover_tool_callback_t stop;
  cap_ai_rover_tool_callback_t gripper_open;
  cap_ai_rover_tool_callback_t gripper_close;
  cap_ai_rover_tool_callback_t read_imu;
  cap_ai_rover_tool_callback_t vision_scan;
  cap_ai_rover_tool_callback_t vision_capture;
} cap_ai_rover_callbacks_t;

esp_err_t cap_ai_rover_register_group(const cap_ai_rover_callbacks_t *callbacks);

#ifdef __cplusplus
}
#endif
