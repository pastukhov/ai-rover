#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifndef OLLAMA_HOST
#define OLLAMA_HOST "http://localhost:11434"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ollama_handle *ollama_handle_t;

typedef char *(*ollama_function_callback_t)(const char *function_name, const char *arguments, void *user_data);

typedef struct {
  const char *name;
  const char *type;
  const char *description;
  bool required;
  const char **enum_values;
} ollama_param_t;

typedef struct {
  const char *name;
  const char *description;
  const ollama_param_t *parameters;
  ollama_function_callback_t callback;
  void *user_data;
} ollama_simple_function_t;

typedef char *(*ollama_tool_callback_t)(const char *tool_call_id, const char *function_name, const char *arguments, void *user_data);

typedef struct {
  const char *model;
  const char *system_role;
  float temperature;
  int max_tokens;
  int timeout_ms;
  bool enable_tools;
  ollama_tool_callback_t tool_callback;
  void *tool_user_data;
} ollama_config_t;

ollama_handle_t ollama_create(const ollama_config_t *config);
void ollama_destroy(ollama_handle_t handle);
esp_err_t ollama_set_model(ollama_handle_t handle, const char *model);
esp_err_t ollama_set_system_role(ollama_handle_t handle, const char *role);
esp_err_t ollama_set_temperature(ollama_handle_t handle, float temperature);
esp_err_t ollama_set_max_tokens(ollama_handle_t handle, int max_tokens);
esp_err_t ollama_set_tools_enabled(ollama_handle_t handle, bool enable_tools);
esp_err_t ollama_set_tool_callback(ollama_handle_t handle, ollama_tool_callback_t callback, void *user_data);
esp_err_t ollama_register_simple_function(ollama_handle_t handle, const ollama_simple_function_t *function);
esp_err_t ollama_call_with_tools(ollama_handle_t handle, const char *prompt, char *response, size_t response_size, int max_tool_iterations);
esp_err_t ollama_call_with_image_data(ollama_handle_t handle, const char *prompt, const unsigned char *image_data, size_t image_size, char *response, size_t response_size);

#ifdef __cplusplus
}
#endif
