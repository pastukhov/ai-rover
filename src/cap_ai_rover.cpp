#include "cap_ai_rover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_cap.h"

static cap_ai_rover_callbacks_t s_callbacks = {};

static esp_err_t execute_tool(const char *tool_name,
                              cap_ai_rover_tool_callback_t callback,
                              const char *input_json,
                              char *output,
                              size_t output_size) {
  if (output == NULL || output_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  output[0] = '\0';
  if (callback == NULL) {
    snprintf(output, output_size, "Error: tool unavailable");
    return ESP_ERR_INVALID_STATE;
  }

  char *result = callback(tool_name, input_json ? input_json : "{}", NULL);
  if (result == NULL) {
    snprintf(output, output_size, "Error: no result");
    return ESP_FAIL;
  }

  strlcpy(output, result, output_size);
  free(result);
  return ESP_OK;
}

static esp_err_t move_execute(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char *output,
                              size_t output_size) {
  (void)ctx;
  return execute_tool("move", s_callbacks.move, input_json, output, output_size);
}

static esp_err_t turn_execute(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char *output,
                              size_t output_size) {
  (void)ctx;
  return execute_tool("turn", s_callbacks.turn, input_json, output, output_size);
}

static esp_err_t stop_execute(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char *output,
                              size_t output_size) {
  (void)ctx;
  return execute_tool("stop", s_callbacks.stop, input_json, output, output_size);
}

static esp_err_t gripper_open_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size) {
  (void)ctx;
  return execute_tool("gripper_open", s_callbacks.gripper_open, input_json, output, output_size);
}

static esp_err_t gripper_close_execute(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char *output,
                                       size_t output_size) {
  (void)ctx;
  return execute_tool("gripper_close", s_callbacks.gripper_close, input_json, output, output_size);
}

static esp_err_t read_imu_execute(const char *input_json,
                                  const claw_cap_call_context_t *ctx,
                                  char *output,
                                  size_t output_size) {
  (void)ctx;
  return execute_tool("read_imu", s_callbacks.read_imu, input_json, output, output_size);
}

static esp_err_t vision_scan_execute(const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     char *output,
                                     size_t output_size) {
  (void)ctx;
  return execute_tool("vision_scan", s_callbacks.vision_scan, input_json, output, output_size);
}

static esp_err_t vision_capture_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size) {
  (void)ctx;
  return execute_tool("vision_capture", s_callbacks.vision_capture, input_json, output, output_size);
}

esp_err_t cap_ai_rover_register_group(const cap_ai_rover_callbacks_t *callbacks) {
  if (callbacks == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  s_callbacks = *callbacks;

  static const claw_cap_descriptor_t descriptors[] = {
    {
      .id = "move",
      .name = "move",
      .family = "ai_rover",
      .description = "Move the mecanum rover briefly, then stop.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"duration_ms\":{\"type\":\"number\"}},\"required\":[\"x\",\"y\"]}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = move_execute,
    },
    {
      .id = "rover_move",
      .name = "rover_move",
      .family = "ai_rover",
      .description = "Move the mecanum rover briefly, then stop.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"duration_ms\":{\"type\":\"number\"}},\"required\":[\"x\",\"y\"]}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = move_execute,
    },
    {
      .id = "turn",
      .name = "turn",
      .family = "ai_rover",
      .description = "Rotate the rover using IMU feedback.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]},\"angle_deg\":{\"type\":\"number\"},\"speed_percent\":{\"type\":\"number\"}},\"required\":[\"direction\"]}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = turn_execute,
    },
    {
      .id = "rover_turn",
      .name = "rover_turn",
      .family = "ai_rover",
      .description = "Rotate the rover using IMU feedback.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]},\"angle_deg\":{\"type\":\"number\"},\"speed_percent\":{\"type\":\"number\"}},\"required\":[\"direction\"]}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = turn_execute,
    },
    {
      .id = "stop",
      .name = "stop",
      .family = "ai_rover",
      .description = "Stop all rover motion immediately.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = stop_execute,
    },
    {
      .id = "rover_stop",
      .name = "rover_stop",
      .family = "ai_rover",
      .description = "Stop all rover motion immediately.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = stop_execute,
    },
    {
      .id = "gripper_open",
      .name = "gripper_open",
      .family = "ai_rover",
      .description = "Open the rover gripper.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = gripper_open_execute,
    },
    {
      .id = "gripper_close",
      .name = "gripper_close",
      .family = "ai_rover",
      .description = "Close the rover gripper.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = gripper_close_execute,
    },
    {
      .id = "read_imu",
      .name = "read_imu",
      .family = "ai_rover",
      .description = "Read accelerometer and gyroscope values.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = read_imu_execute,
    },
    {
      .id = "vision_scan",
      .name = "vision_scan",
      .family = "ai_rover",
      .description = "Scan the camera scene for faces and objects.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"reliable\",\"fast\"]}}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = vision_scan_execute,
    },
    {
      .id = "vision_capture",
      .name = "vision_capture",
      .family = "ai_rover",
      .description = "Capture a camera image and answer a visual question.",
      .kind = CLAW_CAP_KIND_CALLABLE,
      .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
      .input_schema_json = "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"},\"quality\":{\"type\":\"number\"}}}",
      .init = NULL,
      .start = NULL,
      .stop = NULL,
      .execute = vision_capture_execute,
    },
  };
  static const claw_cap_group_t group = {
    .group_id = "cap_ai_rover",
    .plugin_name = "ai-rover",
    .version = "0.1.0",
    .descriptors = descriptors,
    .descriptor_count = sizeof(descriptors) / sizeof(descriptors[0]),
    .plugin_ctx = NULL,
    .group_init = NULL,
    .group_start = NULL,
    .group_stop = NULL,
  };

  if (claw_cap_group_exists(group.group_id)) {
    return ESP_OK;
  }
  return claw_cap_register_group(&group);
}
