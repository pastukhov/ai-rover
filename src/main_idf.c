#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

#include "secrets.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "openrouter.h"

static const char *TAG = "ai-rover-idf";

static const char *kSyslogHost = "192.168.11.2";
static const int kSyslogPort = 514;
static const TickType_t kHeartbeatPeriod = pdMS_TO_TICKS(1000);
static const TickType_t kLoopPeriod = pdMS_TO_TICKS(20);
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const int kWifiMaxRetry = 20;

static const gpio_num_t kBtnAPin = GPIO_NUM_37;
static const gpio_num_t kBtnBPin = GPIO_NUM_39;
static const i2c_port_t kI2cPort = I2C_NUM_0;
static const gpio_num_t kI2cSdaPin = GPIO_NUM_0;
static const gpio_num_t kI2cSclPin = GPIO_NUM_26;
static const uint8_t kRoverAddr = 0x38;
static const uint32_t kI2cFreqHz = 100000;
static const int8_t kMoveSpeed = 80;
static const uint8_t kGripperServo = 1;
static const uint8_t kGripperOpenAngle = 150;
static const uint8_t kGripperCloseAngle = 35;
#define CHAT_PROMPT_MAX 384
#define CHAT_RESPONSE_MAX 2048

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static int s_syslog_sock = -1;
static openrouter_handle_t s_ai = NULL;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_ai_mutex;
static SemaphoreHandle_t s_chat_mutex;
static QueueHandle_t s_chat_queue;
static uint32_t s_chat_id = 0;
static uint32_t s_chat_done_id = 0;
static bool s_chat_pending = false;
static esp_err_t s_chat_result_err = ESP_OK;
static char s_chat_response[CHAT_RESPONSE_MAX];

static int8_t s_motion_x = 0;
static int8_t s_motion_y = 0;
static int8_t s_motion_z = 0;
static bool s_motion_active = false;
static bool s_gripper_open = false;
static TickType_t s_web_motion_deadline = 0;

typedef struct {
  uint32_t id;
  char prompt[CHAT_PROMPT_MAX];
} chat_job_t;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data) {
  (void)arg;
  (void)event_data;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < kWifiMaxRetry) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGW(TAG, "WiFi reconnect attempt %d/%d", s_retry_num, kWifiMaxRetry);
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static esp_err_t wifi_connect_blocking(void) {
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

  ESP_ERROR_CHECK(
      esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
  ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
  vEventGroupDelete(s_wifi_event_group);

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "WiFi connected to %s", WIFI_SSID);
    return ESP_OK;
  }
  ESP_LOGE(TAG, "WiFi connect failed after %d attempts", kWifiMaxRetry);
  return ESP_FAIL;
}

static int open_syslog_socket(void) {
  struct sockaddr_in dest_addr = {0};
  dest_addr.sin_addr.s_addr = inet_addr(kSyslogHost);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kSyslogPort);

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create socket: errno=%d", errno);
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
    ESP_LOGE(TAG, "Failed to connect syslog socket: errno=%d", errno);
    close(sock);
    return -1;
  }

  return sock;
}

static void send_syslog(const char *message) {
  if (s_syslog_sock < 0) {
    return;
  }
  char payload[256];
  uint32_t ms = (uint32_t)(esp_log_timestamp() & 0xffffffffu);
  int n = snprintf(
      payload, sizeof(payload), "<134>1 - ai-rover firmware - - - %s t=%" PRIu32, message, ms);
  if (n <= 0) {
    return;
  }
  if (n >= (int)sizeof(payload)) {
    n = (int)sizeof(payload) - 1;
  }

  if (send(s_syslog_sock, payload, (size_t)n, 0) < 0) {
    ESP_LOGW(TAG, "Syslog send failed: errno=%d", errno);
  }
}

static esp_err_t rover_write(uint8_t reg, const uint8_t *data, size_t len) {
  uint8_t payload[8];
  if (len + 1 > sizeof(payload)) {
    return ESP_ERR_INVALID_SIZE;
  }
  payload[0] = reg;
  memcpy(&payload[1], data, len);
  return i2c_master_write_to_device(kI2cPort, kRoverAddr, payload, len + 1, pdMS_TO_TICKS(50));
}

static esp_err_t rover_set_speed(int8_t x, int8_t y, int8_t z) {
  int32_t x_adj = x;
  int32_t y_adj = y;
  if (z != 0) {
    int32_t scale = 100 - (z > 0 ? z : -z);
    x_adj = (x_adj * scale) / 100;
    y_adj = (y_adj * scale) / 100;
  }
  int8_t buffer[4];
  int32_t m0 = y_adj + x_adj - z;
  int32_t m1 = y_adj - x_adj + z;
  int32_t m2 = y_adj - x_adj - z;
  int32_t m3 = y_adj + x_adj + z;
  buffer[0] = (int8_t)((m0 > 100) ? 100 : (m0 < -100 ? -100 : m0));
  buffer[1] = (int8_t)((m1 > 100) ? 100 : (m1 < -100 ? -100 : m1));
  buffer[2] = (int8_t)((m2 > 100) ? 100 : (m2 < -100 ? -100 : m2));
  buffer[3] = (int8_t)((m3 > 100) ? 100 : (m3 < -100 ? -100 : m3));
  return rover_write(0x00, (const uint8_t *)buffer, sizeof(buffer));
}

static esp_err_t rover_set_servo_angle(uint8_t pos, uint8_t angle) {
  uint8_t reg = (uint8_t)(0x10 + pos);
  uint8_t value = angle;
  return rover_write(reg, &value, 1);
}

static void rover_emergency_stop(void) {
  (void)rover_set_speed(0, 0, 0);
}

static esp_err_t rover_init_i2c(void) {
  i2c_config_t conf = {0};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = kI2cSdaPin;
  conf.scl_io_num = kI2cSclPin;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = kI2cFreqHz;
  ESP_ERROR_CHECK(i2c_param_config(kI2cPort, &conf));
  ESP_ERROR_CHECK(i2c_driver_install(kI2cPort, conf.mode, 0, 0, 0));

  uint8_t zero[4] = {0, 0, 0, 0};
  return rover_write(0x00, zero, sizeof(zero));
}

static void buttons_init(void) {
  gpio_config_t io_conf = {0};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << kBtnAPin) | (1ULL << kBtnBPin);
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io_conf));
}

static bool button_pressed(gpio_num_t pin) {
  return gpio_get_level(pin) == 0;
}

static void set_motion(int8_t x, int8_t y, int8_t z, bool active) {
  s_motion_x = x;
  s_motion_y = y;
  s_motion_z = z;
  s_motion_active = active;
}

static void apply_motion(void) {
  static int8_t last_x = 127;
  static int8_t last_y = 127;
  static int8_t last_z = 127;
  int8_t x = s_motion_active ? s_motion_x : 0;
  int8_t y = s_motion_active ? s_motion_y : 0;
  int8_t z = s_motion_active ? s_motion_z : 0;
  if (x == last_x && y == last_y && z == last_z) {
    return;
  }
  (void)rover_set_speed(x, y, z);
  last_x = x;
  last_y = y;
  last_z = z;
}

static void apply_action(const char *action, bool from_web) {
  TickType_t now = xTaskGetTickCount();
  if (strcmp(action, "forward") == 0) {
    set_motion(0, kMoveSpeed, 0, true);
  } else if (strcmp(action, "back") == 0 || strcmp(action, "backward") == 0) {
    set_motion(0, -kMoveSpeed, 0, true);
  } else if (strcmp(action, "left") == 0) {
    set_motion(-kMoveSpeed, 0, 0, true);
  } else if (strcmp(action, "right") == 0) {
    set_motion(kMoveSpeed, 0, 0, true);
  } else if (strcmp(action, "rotate_left") == 0) {
    set_motion(0, 0, -60, true);
  } else if (strcmp(action, "rotate_right") == 0) {
    set_motion(0, 0, 60, true);
  } else if (strcmp(action, "open") == 0) {
    s_gripper_open = true;
    (void)rover_set_servo_angle(kGripperServo, kGripperOpenAngle);
  } else if (strcmp(action, "close") == 0) {
    s_gripper_open = false;
    (void)rover_set_servo_angle(kGripperServo, kGripperCloseAngle);
  } else {
    set_motion(0, 0, 0, false);
  }

  if (from_web) {
    if (s_motion_active) {
      s_web_motion_deadline = now + pdMS_TO_TICKS(1500);
    } else {
      s_web_motion_deadline = 0;
    }
  }
}

static void chat_worker_task(void *arg) {
  (void)arg;
  chat_job_t job;
  char response[CHAT_RESPONSE_MAX];
  while (1) {
    if (xQueueReceive(s_chat_queue, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    esp_err_t err = ESP_FAIL;
    response[0] = '\0';
    send_syslog("WEB chat: start");
    if (s_ai != NULL) {
      xSemaphoreTake(s_ai_mutex, portMAX_DELAY);
      err = openrouter_call(s_ai, job.prompt, response, sizeof(response));
      xSemaphoreGive(s_ai_mutex);
    }

    xSemaphoreTake(s_chat_mutex, portMAX_DELAY);
    if (job.id >= s_chat_done_id) {
      s_chat_done_id = job.id;
      s_chat_result_err = err;
      s_chat_pending = false;
      if (err == ESP_OK) {
        strlcpy(s_chat_response, response, sizeof(s_chat_response));
      } else {
        s_chat_response[0] = '\0';
      }
    }
    xSemaphoreGive(s_chat_mutex);

    send_syslog(err == ESP_OK ? "WEB chat: ok" : "WEB chat: failed");
  }
}

static esp_err_t handle_root(httpd_req_t *req) {
  static const char *html =
      "<html><body><h1>AI Rover ESP-IDF</h1>"
      "<p>/cmd?act=forward|back|left|right|rotate_left|rotate_right|stop|open|close</p>"
      "<p>/chat?msg=...</p><p>/chat_result?id=N</p><p>/status</p></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t *req) {
  char body[192];
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  int n = snprintf(body,
                   sizeof(body),
                   "{\"motion\":%d,\"x\":%d,\"y\":%d,\"z\":%d,\"gripper\":\"%s\"}",
                   s_motion_active ? 1 : 0,
                   s_motion_x,
                   s_motion_y,
                   s_motion_z,
                   s_gripper_open ? "open" : "close");
  xSemaphoreGive(s_state_mutex);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, n);
}

static esp_err_t handle_cmd(httpd_req_t *req) {
  char query[160] = {0};
  char action[48] = "";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "act", action, sizeof(action));
  }
  if (action[0] == '\0') {
    strlcpy(action, "stop", sizeof(action));
  }

  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  apply_action(action, true);
  xSemaphoreGive(s_state_mutex);
  apply_motion();

  char msg[128];
  snprintf(msg, sizeof(msg), "WEB cmd: %s", action);
  send_syslog(msg);

  httpd_resp_set_type(req, "application/json");
  char resp[96];
  int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"act\":\"%s\"}", action);
  return httpd_resp_send(req, resp, n);
}

static esp_err_t handle_chat(httpd_req_t *req) {
  char *query = calloc(1, 512);
  char *prompt = calloc(1, CHAT_PROMPT_MAX);
  if (query == NULL || prompt == NULL) {
    free(query);
    free(prompt);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "alloc failed", HTTPD_RESP_USE_STRLEN);
  }

  if (httpd_req_get_url_query_str(req, query, 512) == ESP_OK) {
    (void)httpd_query_key_value(query, "msg", prompt, CHAT_PROMPT_MAX);
  }
  if (prompt[0] == '\0') {
    free(query);
    free(prompt);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing msg\"}", HTTPD_RESP_USE_STRLEN);
  }
  if (s_ai == NULL || s_chat_queue == NULL) {
    free(query);
    free(prompt);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"ai unavailable\"}", HTTPD_RESP_USE_STRLEN);
  }

  chat_job_t job = {0};
  xSemaphoreTake(s_chat_mutex, portMAX_DELAY);
  if (s_chat_pending) {
    xSemaphoreGive(s_chat_mutex);
    free(query);
    free(prompt);
    httpd_resp_set_status(req, "429 Too Many Requests");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"chat busy\"}", HTTPD_RESP_USE_STRLEN);
  }
  s_chat_id++;
  job.id = s_chat_id;
  strlcpy(job.prompt, prompt, sizeof(job.prompt));
  s_chat_pending = true;
  xSemaphoreGive(s_chat_mutex);

  free(query);
  free(prompt);

  if (xQueueSend(s_chat_queue, &job, 0) != pdTRUE) {
    xSemaphoreTake(s_chat_mutex, portMAX_DELAY);
    s_chat_pending = false;
    xSemaphoreGive(s_chat_mutex);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"chat queue full\"}", HTTPD_RESP_USE_STRLEN);
  }

  char resp[96];
  int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":%" PRIu32 ",\"status\":\"pending\"}", job.id);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, resp, n);
}

static esp_err_t handle_chat_result(httpd_req_t *req) {
  char query[64] = {0};
  char id_str[24] = {0};
  uint32_t id = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "id", id_str, sizeof(id_str));
  }
  if (id_str[0] != '\0') {
    id = (uint32_t)strtoul(id_str, NULL, 10);
  }

  xSemaphoreTake(s_chat_mutex, portMAX_DELAY);
  uint32_t current_id = s_chat_id;
  uint32_t done_id = s_chat_done_id;
  bool pending = s_chat_pending;
  esp_err_t err = s_chat_result_err;
  char response[CHAT_RESPONSE_MAX];
  strlcpy(response, s_chat_response, sizeof(response));
  xSemaphoreGive(s_chat_mutex);

  if (id == 0) {
    id = current_id;
  }
  if (id == 0 || id > current_id) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "no such chat id", HTTPD_RESP_USE_STRLEN);
  }
  if (pending && id == current_id) {
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "pending", HTTPD_RESP_USE_STRLEN);
  }
  if (id > done_id) {
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "pending", HTTPD_RESP_USE_STRLEN);
  }
  if (err != ESP_OK) {
    char body[64];
    int n = snprintf(body, sizeof(body), "ai error: 0x%x", (unsigned)err);
    httpd_resp_set_status(req, "502 Bad Gateway");
    return httpd_resp_send(req, body, n);
  }

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static void start_web_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  httpd_handle_t server = NULL;
  ESP_ERROR_CHECK(httpd_start(&server, &config));

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL};
  httpd_uri_t cmd = {.uri = "/cmd", .method = HTTP_GET, .handler = handle_cmd, .user_ctx = NULL};
  httpd_uri_t chat = {.uri = "/chat", .method = HTTP_GET, .handler = handle_chat, .user_ctx = NULL};
  httpd_uri_t chat_result = {
      .uri = "/chat_result", .method = HTTP_GET, .handler = handle_chat_result, .user_ctx = NULL};
  httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL};

  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cmd));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat_result));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
}

static void init_ai(void) {
  openrouter_config_t cfg = {
      .api_key = OPENROUTER_API_KEY,
      .enable_streaming = false,
      .default_model = "openai/gpt-4o-mini",
      .default_system_role = "You are AI Rover firmware assistant. Answer briefly.",
  };
  s_ai = openrouter_create(&cfg);
  if (s_ai == NULL) {
    ESP_LOGE(TAG, "OpenRouter init failed");
    send_syslog("AI init failed");
    return;
  }
  send_syslog("AI init OK");
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  s_state_mutex = xSemaphoreCreateMutex();
  s_ai_mutex = xSemaphoreCreateMutex();
  s_chat_mutex = xSemaphoreCreateMutex();
  s_chat_queue = xQueueCreate(1, sizeof(chat_job_t));
  if (s_state_mutex == NULL || s_ai_mutex == NULL || s_chat_mutex == NULL || s_chat_queue == NULL) {
    ESP_LOGE(TAG, "Mutex allocation failed");
    esp_restart();
  }
  (void)xTaskCreate(chat_worker_task, "chat_worker", 8192, NULL, 4, NULL);

  ESP_ERROR_CHECK(wifi_connect_blocking());
  ESP_ERROR_CHECK(rover_init_i2c());
  buttons_init();

  s_syslog_sock = open_syslog_socket();
  if (s_syslog_sock < 0) {
    ESP_LOGE(TAG, "Syslog unavailable, rebooting in 5s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
  }

  init_ai();
  start_web_server();

  bool prev_btn_a = false;
  bool prev_btn_b = false;

  ESP_LOGI(TAG, "ESP-IDF rover web control ready");
  send_syslog("Boot complete (idf-only)");

  while (1) {
    bool btn_a = button_pressed(kBtnAPin);
    bool btn_b = button_pressed(kBtnBPin);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (btn_b && !prev_btn_b) {
      rover_emergency_stop();
      set_motion(0, 0, 0, false);
      s_web_motion_deadline = 0;
      s_gripper_open = !s_gripper_open;
      (void)rover_set_servo_angle(kGripperServo, s_gripper_open ? kGripperOpenAngle : kGripperCloseAngle);
      send_syslog(s_gripper_open ? "BtnB: STOP + GRIPPER OPEN" : "BtnB: STOP + GRIPPER CLOSE");
    }

    if (btn_a && btn_b) {
      set_motion(0, 0, 60, true);
      s_web_motion_deadline = 0;
    } else if (btn_a) {
      set_motion(0, kMoveSpeed, 0, true);
      s_web_motion_deadline = 0;
    } else if (s_web_motion_deadline != 0 && xTaskGetTickCount() > s_web_motion_deadline) {
      set_motion(0, 0, 0, false);
      s_web_motion_deadline = 0;
    }

    if (!btn_a && prev_btn_a && s_web_motion_deadline == 0) {
      set_motion(0, 0, 0, false);
      send_syslog("BtnA: STOP");
    }
    if (btn_a && !prev_btn_a) {
      send_syslog("BtnA: ACTIVE");
    }

    xSemaphoreGive(s_state_mutex);

    apply_motion();

    static TickType_t last_hb = 0;
    TickType_t now = xTaskGetTickCount();
    if ((now - last_hb) >= kHeartbeatPeriod) {
      char hb[192];
      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      snprintf(hb,
               sizeof(hb),
               "HB (idf-only) moving=%d x=%d y=%d z=%d gripper=%s btnA=%d btnB=%d",
               s_motion_active ? 1 : 0,
               s_motion_x,
               s_motion_y,
               s_motion_z,
               s_gripper_open ? "open" : "close",
               btn_a ? 1 : 0,
               btn_b ? 1 : 0);
      xSemaphoreGive(s_state_mutex);
      send_syslog(hb);
      last_hb = now;
    }

    prev_btn_a = btn_a;
    prev_btn_b = btn_b;
    vTaskDelay(kLoopPeriod);
  }
}
