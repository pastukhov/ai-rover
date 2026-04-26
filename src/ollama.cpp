#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ollama.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char *TAG = "ollama";

#define MAX_TOOLS 16
#define MAX_PROMPT_LEN 2048
#define MAX_RESPONSE_LEN 4096

typedef struct {
  char model[64];
  char system_role[512];
  float temperature;
  int max_tokens;
  int timeout_ms;
  bool enable_tools;
  ollama_tool_callback_t tool_callback;
  void *tool_user_data;
  ollama_simple_function_t tools[MAX_TOOLS];
  size_t tool_count;
} ollama_impl_t;

static esp_err_t build_tools_json(ollama_impl_t *impl, cJSON **out) {
  cJSON *tools = cJSON_CreateArray();
  if (!tools) return ESP_ERR_NO_MEM;

  for (size_t i = 0; i < impl->tool_count && i < MAX_TOOLS; i++) {
    ollama_simple_function_t *tool = &impl->tools[i];
    cJSON *t = cJSON_CreateObject();
    if (!t) {
      cJSON_Delete(tools);
      return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(t, "type", "function");
    cJSON *func = cJSON_CreateObject();
    if (!func) {
      cJSON_Delete(t);
      cJSON_Delete(tools);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(func, "name", tool->name);
    cJSON_AddStringToObject(func, "description", tool->description);

    if (tool->parameters) {
      cJSON *props = cJSON_CreateObject();
      cJSON *required = cJSON_CreateArray();
      for (int j = 0; tool->parameters[j].name != NULL; j++) {
        cJSON *prop = cJSON_CreateObject();
        cJSON_AddStringToObject(prop, "type", tool->parameters[j].type);
        if (tool->parameters[j].description) {
          cJSON_AddStringToObject(prop, "description", tool->parameters[j].description);
        }
        char key[64];
        snprintf(key, sizeof(key), "%s", tool->parameters[j].name);
        cJSON_AddItemToObject(props, key, prop);
        if (tool->parameters[j].required) {
          cJSON_AddItemToArray(required, cJSON_CreateString(tool->parameters[j].name));
        }
      }
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "type", "object");
      cJSON_AddItemToObject(params, "properties", props);
      if (cJSON_GetArraySize(required) > 0) {
        cJSON_AddItemToObject(params, "required", required);
      } else {
        cJSON_Delete(required);
      }
      cJSON_AddItemToObject(func, "parameters", params);
    }

    cJSON_AddItemToObject(t, "function", func);
    cJSON_AddItemToArray(tools, t);
  }

  *out = tools;
  return ESP_OK;
}

static esp_err_t parse_tool_calls(cJSON *response_obj, char ***calls_out, size_t *count_out) {
  *calls_out = NULL;
  *count_out = 0;

  cJSON *message = cJSON_GetObjectItem(response_obj, "message");
  if (!message) return ESP_OK;

  cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
  if (!tool_calls || !cJSON_IsArray(tool_calls)) return ESP_OK;

  size_t count = cJSON_GetArraySize(tool_calls);
  if (count == 0) return ESP_OK;

  char **calls = (char **)calloc(count, sizeof(char *));
  if (!calls) return ESP_ERR_NO_MEM;

  for (size_t i = 0; i < count; i++) {
    cJSON *call = cJSON_GetArrayItem(tool_calls, i);
    cJSON *func = cJSON_GetObjectItem(call, "function");
    if (!func) continue;

    cJSON *name = cJSON_GetObjectItem(func, "name");
    cJSON *args = cJSON_GetObjectItem(func, "arguments");

    cJSON *wrapper = cJSON_CreateObject();
    if (name) cJSON_AddStringToObject(wrapper, "name", name->valuestring);
    if (args) cJSON_AddItemToObject(wrapper, "arguments", cJSON_Duplicate(args, true));

    calls[i] = cJSON_PrintUnformatted(wrapper);
    cJSON_Delete(wrapper);
  }

  *calls_out = calls;
  *count_out = count;
  return ESP_OK;
}

ollama_handle_t ollama_create(const ollama_config_t *config) {
  if (!config || !config->model) return NULL;

  ollama_impl_t *impl = (ollama_impl_t *)calloc(1, sizeof(ollama_impl_t));
  if (!impl) return NULL;

  strncpy(impl->model, config->model, sizeof(impl->model) - 1);
  if (config->system_role) {
    strncpy(impl->system_role, config->system_role, sizeof(impl->system_role) - 1);
  }
  impl->temperature = config->temperature > 0 ? config->temperature : 0.7f;
  impl->max_tokens = config->max_tokens > 0 ? config->max_tokens : 256;
  impl->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 120000;
  impl->enable_tools = config->enable_tools;
  impl->tool_callback = config->tool_callback;
  impl->tool_user_data = config->tool_user_data;

  return (ollama_handle_t)impl;
}

void ollama_destroy(ollama_handle_t handle) {
  if (handle) free(handle);
}

esp_err_t ollama_set_model(ollama_handle_t handle, const char *model) {
  if (!handle || !model) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  strncpy(impl->model, model, sizeof(impl->model) - 1);
  return ESP_OK;
}

esp_err_t ollama_set_system_role(ollama_handle_t handle, const char *role) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  if (role) {
    strncpy(impl->system_role, role, sizeof(impl->system_role) - 1);
  } else {
    impl->system_role[0] = '\0';
  }
  return ESP_OK;
}

esp_err_t ollama_set_temperature(ollama_handle_t handle, float temperature) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  impl->temperature = temperature;
  return ESP_OK;
}

esp_err_t ollama_set_max_tokens(ollama_handle_t handle, int max_tokens) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  impl->max_tokens = max_tokens;
  return ESP_OK;
}

esp_err_t ollama_set_tools_enabled(ollama_handle_t handle, bool enable_tools) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  impl->enable_tools = enable_tools;
  return ESP_OK;
}

esp_err_t ollama_set_tool_callback(ollama_handle_t handle, ollama_tool_callback_t callback, void *user_data) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  impl->tool_callback = callback;
  impl->tool_user_data = user_data;
  return ESP_OK;
}

esp_err_t ollama_register_simple_function(ollama_handle_t handle, const ollama_simple_function_t *function) {
  if (!handle || !function || !function->name) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;
  if (impl->tool_count >= MAX_TOOLS) return ESP_ERR_NO_MEM;
  impl->tools[impl->tool_count++] = *function;
  return ESP_OK;
}

typedef struct {
  char *response;
  size_t response_size;
  size_t response_pos;
  bool done;
  char error[128];
} ollama_http_context_t;

static esp_err_t ollama_http_event_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    ollama_http_context_t *ctx = (ollama_http_context_t *)evt->user_data;
    if (!ctx || ctx->done) return ESP_OK;

    char *data = (char *)evt->data;
    int len = evt->data_len;

    cJSON *line = cJSON_Parse(data);
    if (line) {
      cJSON *done = cJSON_GetObjectItem(line, "done");
      if (done && cJSON_IsTrue(done)) {
        ctx->done = true;
        cJSON_Delete(line);
        return ESP_OK;
      }

      cJSON *message = cJSON_GetObjectItem(line, "message");
      if (message) {
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring) {
          size_t slen = strlen(content->valuestring);
          if (ctx->response_pos + slen < ctx->response_size - 1) {
            memcpy(ctx->response + ctx->response_pos, content->valuestring, slen);
            ctx->response_pos += slen;
            ctx->response[ctx->response_pos] = '\0';
          }
        }
      }
      cJSON_Delete(line);
    }
  }
  return ESP_OK;
}

static esp_err_t ollama_chat_common(ollama_handle_t handle, cJSON *messages, bool with_images, cJSON *images_array, char *response, size_t response_size) {
  if (!handle || !messages) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;

  cJSON *req = cJSON_CreateObject();
  if (!req) return ESP_ERR_NO_MEM;

  cJSON_AddStringToObject(req, "model", impl->model);
  cJSON_AddNumberToObject(req, "stream", 0);
  cJSON_AddNumberToObject(req, "options/temperature", impl->temperature);
  cJSON_AddNumberToObject(req, "options.num_predict", impl->max_tokens);
  cJSON_AddItemToObject(req, "messages", messages);

  if (impl->enable_tools && impl->tool_count > 0) {
    cJSON *tools = NULL;
    esp_err_t err = build_tools_json(impl, &tools);
    if (err == ESP_OK && tools) {
      cJSON_AddItemToObject(req, "tools", tools);
    }
  }

  if (with_images && images_array && cJSON_GetArraySize(images_array) > 0) {
    cJSON_AddItemToObject(req, "images", cJSON_Duplicate(images_array, true));
  }

  char *req_str = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!req_str) return ESP_ERR_NO_MEM;

  char url[256];
  snprintf(url, sizeof(url), "%s/api/chat", OLLAMA_HOST);

  esp_http_client_config_t http_config = {};
  http_config.url = url;
  http_config.method = HTTP_METHOD_POST;
  http_config.timeout_ms = impl->timeout_ms;
  http_config.event_handler = ollama_http_event_handler;
  http_config.disable_auto_redirect = true;

  ollama_http_context_t ctx = {};
  ctx.response = response;
  ctx.response_size = response_size;
  ctx.response_pos = 0;
  ctx.done = false;
  response[0] = '\0';
  http_config.user_data = &ctx;

  esp_http_client_handle_t client = esp_http_client_init(&http_config);
  if (!client) {
    free(req_str);
    return ESP_ERR_INVALID_STATE;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");

  esp_err_t err = esp_http_client_open(client, strlen(req_str));
  if (err == ESP_OK) {
    int written = esp_http_client_write(client, req_str, strlen(req_str));
    if (written >= 0) {
      int status = esp_http_client_fetch_headers(client);
      if (status >= 200 && status < 300) {
        char buf[4096];
        int read_len;
        while ((read_len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
          buf[read_len] = '\0';

          cJSON *chunk = cJSON_Parse(buf);
          if (chunk) {
            cJSON *done = cJSON_GetObjectItem(chunk, "done");
            if (done && cJSON_IsTrue(done)) {
              cJSON_Delete(chunk);
              break;
            }

            cJSON *message = cJSON_GetObjectItem(chunk, "message");
            if (message) {
              cJSON *content = cJSON_GetObjectItem(message, "content");
              if (content && cJSON_IsString(content) && content->valuestring) {
                size_t slen = strlen(content->valuestring);
                if (ctx.response_pos + slen < ctx.response_size - 1) {
                  memcpy(ctx.response + ctx.response_pos, content->valuestring, slen);
                  ctx.response_pos += slen;
                  ctx.response[ctx.response_pos] = '\0';
                }
              }
            }

            if (impl->enable_tools && impl->tool_callback) {
              cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
              if (tool_calls && cJSON_IsArray(tool_calls)) {
                size_t call_count = cJSON_GetArraySize(tool_calls);
                for (size_t i = 0; i < call_count; i++) {
                  cJSON *call = cJSON_GetArrayItem(tool_calls, i);
                  cJSON *func = cJSON_GetObjectItem(call, "function");
                  if (func) {
                    cJSON *name = cJSON_GetObjectItem(func, "name");
                    cJSON *args = cJSON_GetObjectItem(func, "arguments");

                    if (name && args) {
                      char *result = impl->tool_callback("", name->valuestring, cJSON_PrintUnformatted(args), impl->tool_user_data);
                      if (result) {
                        free(result);
                      }
                    }
                  }
                }
              }
            }

            cJSON_Delete(chunk);
          }
        }
        err = ESP_OK;
      } else {
        err = ESP_FAIL;
      }
    } else {
      err = ESP_FAIL;
    }
  } else {
    err = ESP_ERR_TIMEOUT;
  }

  esp_http_client_cleanup(client);
  free(req_str);

  return err;
}

esp_err_t ollama_call_with_tools(ollama_handle_t handle, const char *prompt, char *response, size_t response_size, int max_tool_iterations) {
  if (!handle || !prompt || !response) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;

  cJSON *messages = cJSON_CreateArray();
  if (!messages) return ESP_ERR_NO_MEM;

  if (impl->system_role[0]) {
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", impl->system_role);
    cJSON_AddItemToArray(messages, sys_msg);
  }

  cJSON *user_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(user_msg, "role", "user");
  cJSON_AddStringToObject(user_msg, "content", prompt);
  cJSON_AddItemToArray(messages, user_msg);

  esp_err_t err = ollama_chat_common(handle, messages, false, NULL, response, response_size);

  if (err == ESP_OK && impl->enable_tools && impl->tool_callback) {
    cJSON *response_json = cJSON_Parse(response);
    if (response_json) {
      char **tool_calls = NULL;
      size_t call_count = 0;

      err = parse_tool_calls(response_json, &tool_calls, &call_count);

      if (err == ESP_OK && call_count > 0) {
        for (size_t t = 0; t < call_count && t < (size_t)max_tool_iterations; t++) {
          cJSON *call_info = cJSON_Parse(tool_calls[t]);
          if (!call_info) continue;

          cJSON *name = cJSON_GetObjectItem(call_info, "name");
          cJSON *args = cJSON_GetObjectItem(call_info, "arguments");

          if (name && args) {
            char *result = impl->tool_callback("", name->valuestring, cJSON_PrintUnformatted(args), impl->tool_user_data);

            if (result) {
              cJSON *tool_msg = cJSON_CreateObject();
              cJSON_AddStringToObject(tool_msg, "role", "tool");
              cJSON_AddStringToObject(tool_msg, "content", result);
              cJSON_AddItemToArray(messages, tool_msg);
              free(result);

              err = ollama_chat_common(handle, messages, false, NULL, response, response_size);
              if (err != ESP_OK) break;

              cJSON_Delete(response_json);
              response_json = cJSON_Parse(response);
              if (!response_json) break;

              free(tool_calls);
              err = parse_tool_calls(response_json, &tool_calls, &call_count);
              if (err != ESP_OK || call_count == 0) {
                call_count = 0;
                break;
              }
              t = (size_t)-1;
            }
          }
          cJSON_Delete(call_info);
        }
      }

      if (tool_calls) {
        for (size_t i = 0; i < call_count; i++) {
          free(tool_calls[i]);
        }
        free(tool_calls);
      }
      cJSON_Delete(response_json);
    }
  }

  cJSON_Delete(messages);
  return err;
}

esp_err_t ollama_call_with_image_data(ollama_handle_t handle, const char *prompt, const unsigned char *image_data, size_t image_size, char *response, size_t response_size) {
  if (!handle || !prompt || !image_data || !response) return ESP_ERR_INVALID_ARG;
  ollama_impl_t *impl = (ollama_impl_t *)handle;

  size_t b64_size = (image_size * 4 / 3) + 10;
  char *b64 = (char *)malloc(b64_size);
  if (!b64) return ESP_ERR_NO_MEM;

  size_t b64_len = 0;
  esp_err_t err = mbedtls_base64_encode((unsigned char *)b64, b64_size - 1, &b64_len, image_data, image_size);
  b64[b64_len] = '\0';

  if (err != 0) {
    free(b64);
    return ESP_ERR_NO_MEM;
  }

  cJSON *images_array = cJSON_CreateArray();
  if (!images_array) {
    free(b64);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToArray(images_array, cJSON_CreateString(b64));
  free(b64);

  cJSON *messages = cJSON_CreateArray();
  if (!messages) {
    cJSON_Delete(images_array);
    return ESP_ERR_NO_MEM;
  }

  if (impl->system_role[0]) {
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", impl->system_role);
    cJSON_AddItemToArray(messages, sys_msg);
  }

  cJSON *user_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(user_msg, "role", "user");
  cJSON_AddStringToObject(user_msg, "content", prompt);
  cJSON_AddItemToObject(user_msg, "images", images_array);
  cJSON_AddItemToArray(messages, user_msg);

  err = ollama_chat_common(handle, messages, true, images_array, response, response_size);

  cJSON_Delete(messages);
  return err;
}
