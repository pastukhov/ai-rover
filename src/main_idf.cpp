#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <strings.h>

#include "M5Unified.h"
#include "logger_json.h"
#include "secrets.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "openrouter.h"

static const char *TAG = "ai-rover-idf";

static size_t json_escape_copy(char *dst, size_t dst_size, const char *src);

static const char *kSyslogHost = "192.168.11.2";
static const int kSyslogPort = 514;
static const size_t kSyslogMsgMax = 512;
static const size_t kSyslogPayloadMax = 640;
static const TickType_t kHeartbeatPeriod = pdMS_TO_TICKS(1000);
static const TickType_t kVisionPingPeriod = pdMS_TO_TICKS(10000);
static const TickType_t kLoopPeriod = pdMS_TO_TICKS(20);
static const TickType_t kWifiConnectTimeout = pdMS_TO_TICKS(30000);
static const TickType_t kInactivitySleepTimeout = pdMS_TO_TICKS(120000);
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const int kWifiMaxRetry = 20;

static const gpio_num_t kI2cSdaPin = GPIO_NUM_0;
static const gpio_num_t kI2cSclPin = GPIO_NUM_26;
static const uint8_t kRoverAddr = 0x38;
static const uint32_t kI2cFreqHz = 100000;
static const int8_t kMoveSpeed = 80;
static const gpio_num_t kBtnAPin = GPIO_NUM_37;
static const gpio_num_t kBtnBPin = GPIO_NUM_39;
static const uint8_t kGripperServo = 1;
static const uint8_t kGripperOpenAngle = 35;
static const uint8_t kGripperCloseAngle = 150;
#define CHAT_PROMPT_MAX 384
#define CHAT_RESPONSE_MAX 2048

// ── Vision (UnitV-M12) ──
static const uart_port_t kVisionUart = UART_NUM_1;
static const gpio_num_t kVisionTxPin = GPIO_NUM_32;
static const gpio_num_t kVisionRxPin = GPIO_NUM_33;
static const int kVisionBaud = 115200;
static const int kVisionRxBuf = 2048;
static const int kVisionTimeoutMs = 7000;
static const int kVisionPingTimeoutMs = 500;
static const int kVisionCaptureTimeoutMs = 12000;
static const int kCaptureMaxJpegBytes = 40960;   // 40KB K210 limit
static const int kCaptureDefaultQuality = 75;
static const int kCaptureChunkSize = 2048;
#define VISION_RESP_MAX 512

// ── Rover FSM ──
typedef enum {
  STATE_IDLE,
  STATE_WEB_CONTROL,
  STATE_AI_THINKING,
  STATE_AI_EXECUTING,
  STATE_OFFLINE_FALLBACK,
} rover_state_t;

static const char *state_name(rover_state_t s) {
  switch (s) {
    case STATE_IDLE:             return "IDLE";
    case STATE_WEB_CONTROL:      return "WEB_CTRL";
    case STATE_AI_THINKING:      return "AI_THINK";
    case STATE_AI_EXECUTING:     return "AI_EXEC";
    case STATE_OFFLINE_FALLBACK: return "OFFLINE";
    default:                     return "???";
  }
}

static const char *wakeup_cause_name(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "cold_boot";
    case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP: return "ulp";
#if SOC_PM_SUPPORT_WIFI_WAKEUP
    case ESP_SLEEP_WAKEUP_WIFI: return "wifi";
#endif
#if SOC_PM_SUPPORT_BT_WAKEUP
    case ESP_SLEEP_WAKEUP_BT: return "bt";
#endif
    default: return "other";
  }
}

static rover_state_t s_rover_state = STATE_IDLE;

// Forward declaration — implemented after syslog helpers
static void transition_to(rover_state_t new_state);
static void send_syslog(const char *message);

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static int s_syslog_sock = -1;
static openrouter_handle_t s_ai = NULL;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_power_mutex;
static SemaphoreHandle_t s_ai_mutex;
static SemaphoreHandle_t s_chat_mutex;
static QueueHandle_t s_chat_queue;
static QueueHandle_t s_syslog_queue;
static uint32_t s_chat_id = 0;
static uint32_t s_chat_done_id = 0;
static bool s_chat_pending = false;
static esp_err_t s_chat_result_err = ESP_OK;
static char s_chat_response[CHAT_RESPONSE_MAX];
static bool s_wifi_connected = false;
static httpd_handle_t s_httpd = NULL;
static SemaphoreHandle_t s_vision_mutex;
static uint32_t s_vision_req_id = 0;
static bool s_vision_available = false;

static int8_t s_motion_x = 0;
static int8_t s_motion_y = 0;
static int8_t s_motion_z = 0;
static bool s_motion_active = false;
static bool s_gripper_open = false;
static TickType_t s_web_motion_deadline = 0;
static std::atomic<uint32_t> s_last_activity_tick{0};

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
      rover_log_field_t fields[] = {
        rover_log_field_int("retry", s_retry_num),
        rover_log_field_int("max_retry", kWifiMaxRetry),
      };
      rover_log_record_t rec = {
        .level = ESP_LOG_WARN,
        .component = TAG,
        .event = "wifi_reconnect_attempt",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&rec);
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

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, kWifiConnectTimeout);

  ESP_ERROR_CHECK(
      esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
  ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
  vEventGroupDelete(s_wifi_event_group);

  if (bits & WIFI_CONNECTED_BIT) {
    rover_log_field_t fields[] = { rover_log_field_str("ssid", WIFI_SSID) };
    rover_log_record_t rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "wifi_connected",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
    return ESP_OK;
  }
  if ((bits & WIFI_FAIL_BIT) == 0) {
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "wifi_connect_timeout",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    return ESP_ERR_TIMEOUT;
  }
  rover_log_field_t fields[] = { rover_log_field_int("max_retry", kWifiMaxRetry) };
  rover_log_record_t rec = {
    .level = ESP_LOG_ERROR,
    .component = TAG,
    .event = "wifi_connect_failed",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);
  return ESP_FAIL;
}

static void draw_boot_status(const char *status, const char *detail) {
  const uint32_t bg = 0x111827u;
  // Pick bar color by keyword
  uint32_t bar_color = 0x2563EBu; // blue default
  if (strstr(status, "OFFLINE") || strstr(status, "failed")) {
    bar_color = 0xDC2626u; // red
  } else if (strstr(status, "ready")) {
    bar_color = 0x2D8B2Du; // green
  } else if (strstr(status, "sleep")) {
    bar_color = 0x6B21A8u; // purple
  }

  M5.Display.startWrite();
  M5.Display.fillScreen(bg);

  // ── Top bar ──
  M5.Display.fillRoundRect(2, 2, 236, 24, 4, bar_color);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, bar_color);
  M5.Display.setCursor(8, 6);
  M5.Display.print("AI Rover");

  // ── Status text (large, centered) ──
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, bg);
  int sw = (int)strlen(status) * 12;
  M5.Display.setCursor((240 - sw) / 2, 44);
  M5.Display.print(status);

  // ── Detail line (small, centered, dimmed) ──
  if (detail != NULL && detail[0] != '\0') {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x9CA3AFu, bg);
    int dw = (int)strlen(detail) * 6;
    M5.Display.setCursor((240 - dw) / 2, 72);
    M5.Display.print(detail);
  }

  // ── Bottom accent line ──
  M5.Display.fillRoundRect(40, 100, 160, 4, 2, bar_color);

  M5.Display.endWrite();
}

static int open_syslog_socket(void) {
  struct sockaddr_in dest_addr = {};
  dest_addr.sin_addr.s_addr = inet_addr(kSyslogHost);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kSyslogPort);

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    rover_log_field_t fields[] = { rover_log_field_int("errno", errno) };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "syslog_socket_create_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
    rover_log_field_t fields[] = { rover_log_field_int("errno", errno) };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "syslog_socket_connect_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
    close(sock);
    return -1;
  }

  return sock;
}

static size_t json_escape_copy(char *dst, size_t dst_size, const char *src) {
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

static const char *guess_syslog_event(const char *message) {
  if (message == NULL) return "log";
  if (strncmp(message, "FSM ", 4) == 0) return "fsm";
  if (strncmp(message, "TOOL ", 5) == 0) return "tool";
  if (strncmp(message, "VISION ", 7) == 0 || strncmp(message, "Vision ", 7) == 0) return "vision";
  if (strncmp(message, "WEB chat", 8) == 0) return "web_chat";
  if (strncmp(message, "BtnA", 4) == 0 || strncmp(message, "BtnB", 4) == 0) return "button";
  if (strncmp(message, "AI ", 3) == 0) return "ai";
  if (strncmp(message, "WiFi ", 5) == 0) return "wifi";
  if (strncmp(message, "Boot ", 5) == 0) return "boot";
  return "log";
}

static void send_syslog(const char *message) {
  if (s_syslog_queue == NULL) return;
  if (message == NULL || message[0] == '\0') return;

  char buf[kSyslogMsgMax];
  uint32_t ms = (uint32_t)(esp_log_timestamp() & 0xffffffffu);
  size_t msg_len = strlen(message);
  bool is_json_obj = (msg_len >= 2 && message[0] == '{' && message[msg_len - 1] == '}');

  if (is_json_obj) {
    int n;
    if (strstr(message, "\"t_ms\"") != NULL) {
      n = snprintf(buf, sizeof(buf), "%s", message);
    } else {
      n = snprintf(buf, sizeof(buf), "%.*s,\"t_ms\":%" PRIu32 "}",
                   (int)(msg_len - 1), message, ms);
    }
    if (n <= 0 || n >= (int)sizeof(buf)) {
      snprintf(buf, sizeof(buf),
               "{\"event\":\"log\",\"msg\":\"json message truncated\",\"t_ms\":%" PRIu32 "}", ms);
    }
  } else {
    char escaped[384];
    (void)json_escape_copy(escaped, sizeof(escaped), message);
    int n = snprintf(buf, sizeof(buf),
                     "{\"event\":\"%s\",\"msg\":\"%s\",\"t_ms\":%" PRIu32 "}",
                     guess_syslog_event(message), escaped, ms);
    if (n <= 0 || n >= (int)sizeof(buf)) {
      snprintf(buf, sizeof(buf),
               "{\"event\":\"log\",\"msg\":\"text message truncated\",\"t_ms\":%" PRIu32 "}", ms);
    }
  }
  // Non-blocking: drop if queue is full
  xQueueSend(s_syslog_queue, buf, 0);
}

static void rover_log_syslog_sink(const char *json_line, void *ctx) {
  (void)ctx;
  send_syslog(json_line);
}

static void read_power_metrics(int16_t *vbus_mv, int32_t *bat_pct) {
  if (s_power_mutex != NULL) xSemaphoreTake(s_power_mutex, portMAX_DELAY);
  if (vbus_mv != NULL) *vbus_mv = M5.Power.getVBUSVoltage();
  if (bat_pct != NULL) *bat_pct = M5.Power.getBatteryLevel();
  if (s_power_mutex != NULL) xSemaphoreGive(s_power_mutex);
}

// Must be called with s_state_mutex held
static void transition_to(rover_state_t new_state) {
  if (new_state == s_rover_state) return;
  const char *from = state_name(s_rover_state);
  const char *to = state_name(new_state);
  s_rover_state = new_state;
  rover_log_field_t fields[] = {
    rover_log_field_str("from", from),
    rover_log_field_str("to", to),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "fsm_transition",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);
}

static esp_err_t rover_write(uint8_t reg, const uint8_t *data, size_t len) {
  if (!M5.Ex_I2C.isEnabled()) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
  bool ok = M5.Ex_I2C.writeRegister(kRoverAddr, reg, data, len, kI2cFreqHz);
  xSemaphoreGive(s_i2c_mutex);
  return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t rover_set_speed(int8_t x, int8_t y, int8_t z) {
  // Negate z: hardware motor layout has opposite rotation convention
  int32_t zn = -z;
  int32_t x_adj = x;
  int32_t y_adj = y;
  if (zn != 0) {
    int32_t scale = 100 - (zn > 0 ? zn : -zn);
    x_adj = (x_adj * scale) / 100;
    y_adj = (y_adj * scale) / 100;
  }
  int8_t buffer[4];
  int32_t m0 = y_adj + x_adj - zn;
  int32_t m1 = y_adj - x_adj + zn;
  int32_t m2 = y_adj - x_adj - zn;
  int32_t m3 = y_adj + x_adj + zn;
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

// ── Vision UART (UnitV-M12 via Grove G32/G33) ──

static esp_err_t vision_uart_init(void) {
  uart_config_t uart_cfg = {};
  uart_cfg.baud_rate = kVisionBaud;
  uart_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.parity = UART_PARITY_DISABLE;
  uart_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

  esp_err_t err = uart_driver_install(kVisionUart, kVisionRxBuf * 2, kVisionRxBuf, 0, NULL, 0);
  if (err != ESP_OK) return err;
  err = uart_param_config(kVisionUart, &uart_cfg);
  if (err != ESP_OK) return err;
  return uart_set_pin(kVisionUart, kVisionTxPin, kVisionRxPin,
                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static esp_err_t vision_cmd_timeout(const char *cmd, const char *args_json,
                                    char *resp, size_t resp_size, int timeout_ms) {
  uint32_t rid = ++s_vision_req_id;
  char req[256];
  int n = snprintf(req, sizeof(req),
                   "{\"cmd\":\"%s\",\"req_id\":\"%" PRIu32 "\",\"args\":%s}\n",
                   cmd, rid, args_json ? args_json : "{}");
  if (n >= (int)sizeof(req)) return ESP_ERR_NO_MEM;

  uart_flush_input(kVisionUart);

  int sent = uart_write_bytes(kVisionUart, req, n);
  if (sent != n) return ESP_FAIL;

  // Read until \n or timeout
  int pos = 0;
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (pos < (int)resp_size - 1) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0) return ESP_ERR_TIMEOUT;
    TickType_t remaining = deadline - now;

    uint8_t byte;
    int rd = uart_read_bytes(kVisionUart, &byte, 1, remaining);
    if (rd <= 0) return ESP_ERR_TIMEOUT;
    if (byte == '\n') break;
    if (byte >= 0x20) resp[pos++] = (char)byte; // skip control chars
  }
  resp[pos] = '\0';
  if (pos == 0) return ESP_ERR_TIMEOUT;

  rover_log_field_t fields[] = {
    rover_log_field_str("cmd", cmd),
    rover_log_field_int("resp_bytes", pos),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "vision_uart_response",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);
  return ESP_OK;
}

static esp_err_t vision_cmd(const char *cmd, const char *args_json,
                            char *resp, size_t resp_size) {
  return vision_cmd_timeout(cmd, args_json, resp, resp_size, kVisionTimeoutMs);
}

static esp_err_t vision_capture(int quality, uint8_t **jpeg_out, size_t *jpeg_size_out) {
  *jpeg_out = NULL;
  *jpeg_size_out = 0;

  uint32_t rid = ++s_vision_req_id;
  char req[128];
  int n = snprintf(req, sizeof(req),
                   "{\"cmd\":\"CAPTURE\",\"req_id\":\"%" PRIu32 "\",\"args\":{\"quality\":%d}}\n",
                   rid, quality);
  if (n >= (int)sizeof(req)) return ESP_ERR_NO_MEM;

  uart_flush_input(kVisionUart);
  int sent = uart_write_bytes(kVisionUart, req, n);
  if (sent != n) return ESP_FAIL;

  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kVisionCaptureTimeoutMs);

  // Phase 1: read JSON header until \n
  char hdr[256];
  int pos = 0;
  while (pos < (int)sizeof(hdr) - 1) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0) return ESP_ERR_TIMEOUT;
    uint8_t byte;
    int rd = uart_read_bytes(kVisionUart, &byte, 1, deadline - now);
    if (rd <= 0) return ESP_ERR_TIMEOUT;
    if (byte == '\n') break;
    if (byte >= 0x20) hdr[pos++] = (char)byte;
  }
  hdr[pos] = '\0';
  if (pos == 0) return ESP_ERR_TIMEOUT;

  // Parse JSON header
  cJSON *root = cJSON_Parse(hdr);
  if (!root) return ESP_ERR_INVALID_RESPONSE;

  cJSON *ok_field = cJSON_GetObjectItem(root, "ok");
  if (!cJSON_IsTrue(ok_field)) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  cJSON *result = cJSON_GetObjectItem(root, "result");
  cJSON *size_field = result ? cJSON_GetObjectItem(result, "size") : NULL;
  if (!size_field || !cJSON_IsNumber(size_field)) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }
  int jpeg_size = size_field->valueint;
  cJSON_Delete(root);

  if (jpeg_size <= 0 || jpeg_size > kCaptureMaxJpegBytes) return ESP_ERR_INVALID_RESPONSE;

  // Phase 2: read binary JPEG data
  uint8_t *buf = (uint8_t *)malloc(jpeg_size);
  if (!buf) return ESP_ERR_NO_MEM;

  int total = 0;
  while (total < jpeg_size) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0) { free(buf); return ESP_ERR_TIMEOUT; }
    int want = jpeg_size - total;
    if (want > kCaptureChunkSize) want = kCaptureChunkSize;
    int rd = uart_read_bytes(kVisionUart, buf + total, want, deadline - now);
    if (rd <= 0) { free(buf); return ESP_ERR_TIMEOUT; }
    total += rd;
  }

  rover_log_field_t fields[] = {
    rover_log_field_str("cmd", "CAPTURE"),
    rover_log_field_int("jpeg_bytes", jpeg_size),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "vision_capture_ok",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);

  *jpeg_out = buf;
  *jpeg_size_out = (size_t)jpeg_size;
  return ESP_OK;
}

static esp_err_t rover_init_i2c(void) {
  if (!M5.Ex_I2C.begin(I2C_NUM_0, kI2cSdaPin, kI2cSclPin)) {
    return ESP_FAIL;
  }

  uint8_t zero[4] = {0, 0, 0, 0};
  return rover_write(0x00, zero, sizeof(zero));
}

static void set_motion(int8_t x, int8_t y, int8_t z, bool active) {
  s_motion_x = x;
  s_motion_y = y;
  s_motion_z = z;
  s_motion_active = active;
}

static void mark_activity(void) {
  s_last_activity_tick.store((uint32_t)xTaskGetTickCount(), std::memory_order_relaxed);
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
  mark_activity();
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

static void enter_deep_sleep(void) {
  rover_emergency_stop();
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "power_deep_sleep_enter",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);
  draw_boot_status("sleeping...", "press A/B wake");
  vTaskDelay(pdMS_TO_TICKS(200));

  // Shutdown mDNS, WiFi and display before sleep
  mdns_free();
  esp_wifi_disconnect();
  esp_wifi_stop();
  M5.Display.setBrightness(0);
  M5.Display.sleep();

  // Reset previous wake sources and reconfigure button pins for RTC wake.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  (void)rtc_gpio_deinit(kBtnAPin);
  (void)rtc_gpio_deinit(kBtnBPin);
  (void)rtc_gpio_init(kBtnAPin);
  (void)rtc_gpio_init(kBtnBPin);
  (void)rtc_gpio_set_direction(kBtnAPin, RTC_GPIO_MODE_INPUT_ONLY);
  (void)rtc_gpio_set_direction(kBtnBPin, RTC_GPIO_MODE_INPUT_ONLY);
  (void)rtc_gpio_pulldown_dis(kBtnAPin);
  (void)rtc_gpio_pulldown_dis(kBtnBPin);

  // BtnA (G37) — ext0, wakeup on LOW (active-low button)
  // Note: GPIO37/GPIO39 are input-only on ESP32; internal pull-ups may be unavailable,
  // so wake reliability depends on the board's external button circuitry.
  (void)rtc_gpio_pullup_en(kBtnAPin);
  esp_err_t ext0_err = esp_sleep_enable_ext0_wakeup(kBtnAPin, 0);
  if (ext0_err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("button", "A"),
      rover_log_field_str("err", esp_err_to_name(ext0_err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "wake_ext0_setup_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
  }

  // BtnB (G39) — ext1, wakeup on ALL_LOW (active-low button)
  (void)rtc_gpio_pullup_en(kBtnBPin);
  esp_err_t ext1_err = esp_sleep_enable_ext1_wakeup(1ULL << kBtnBPin, ESP_EXT1_WAKEUP_ALL_LOW);
  if (ext1_err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("button", "B"),
      rover_log_field_str("err", esp_err_to_name(ext1_err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "wake_ext1_setup_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
  }

  // Keep RTC peripherals powered in deep sleep so RTC GPIO wake logic and pull config
  // remain reliable on StickC Plus button lines (regression observed after Arduino->IDF port).
  esp_err_t rtc_pd_err = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  if (rtc_pd_err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("domain", "RTC_PERIPH"),
      rover_log_field_str("err", esp_err_to_name(rtc_pd_err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "power_domain_config_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
  }

  esp_deep_sleep_start();
}

// ── Tool callbacks for LLM function calling ──

static inline int clamp_int(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static char *make_tool_response(const char *status, const char *action) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"action\":\"%s\"}", status, action);
  return strdup(buf);
}

static char *cb_move(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)ud;
  int x = 0, y = 0, z = 0, duration_ms = 1500;
  cJSON *args = cJSON_Parse(arguments ? arguments : "{}");
  if (args) {
    cJSON *v;
    v = cJSON_GetObjectItem(args, "x"); if (v && cJSON_IsNumber(v)) x = v->valueint;
    v = cJSON_GetObjectItem(args, "y"); if (v && cJSON_IsNumber(v)) y = v->valueint;
    v = cJSON_GetObjectItem(args, "z"); if (v && cJSON_IsNumber(v)) z = v->valueint;
    v = cJSON_GetObjectItem(args, "duration_ms"); if (v && cJSON_IsNumber(v)) duration_ms = v->valueint;
    cJSON_Delete(args);
  }
  x = clamp_int(x, -100, 100);
  y = clamp_int(y, -100, 100);
  z = clamp_int(z, -100, 100);
  duration_ms = clamp_int(duration_ms, 100, 5000);

  mark_activity();
  rover_log_field_t fields[] = {
    rover_log_field_int("x", x),
    rover_log_field_int("y", y),
    rover_log_field_int("z", z),
    rover_log_field_int("duration_ms", duration_ms),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_move",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);

  TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
  while (xTaskGetTickCount() < end) {
    (void)rover_set_speed((int8_t)x, (int8_t)y, (int8_t)z);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  (void)rover_set_speed(0, 0, 0);
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  set_motion(0, 0, 0, false);
  xSemaphoreGive(s_state_mutex);

  return make_tool_response("ok", "move"); // openrouter_client frees this
}

static char *cb_turn(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)ud;
  const char *direction = "left";
  int angle_deg = 90, speed_pct = 50;
  char dir_buf[8] = "left";

  cJSON *args = cJSON_Parse(arguments ? arguments : "{}");
  if (args) {
    cJSON *v;
    v = cJSON_GetObjectItem(args, "direction");
    if (v && cJSON_IsString(v)) {
      strlcpy(dir_buf, v->valuestring, sizeof(dir_buf));
      direction = dir_buf;
    }
    v = cJSON_GetObjectItem(args, "angle_deg"); if (v && cJSON_IsNumber(v)) angle_deg = v->valueint;
    v = cJSON_GetObjectItem(args, "speed_percent"); if (v && cJSON_IsNumber(v)) speed_pct = v->valueint;
    cJSON_Delete(args);
  }

  if (!M5.Imu.isEnabled()) {
    return make_tool_response("imu_unavailable", "turn");
  }

  bool turn_left = (strcmp(direction, "right") != 0);
  float target = (float)clamp_int(angle_deg, 5, 360);
  int8_t spd = (int8_t)clamp_int(speed_pct, 20, 100);
  int8_t turn_z = turn_left ? (int8_t)-spd : spd;
  uint32_t timeout_ms = (uint32_t)clamp_int((int)(target * 100.0f), 2000, 12000);

  mark_activity();
  rover_log_field_t fields[] = {
    rover_log_field_str("direction", turn_left ? "left" : "right"),
    rover_log_field_int("angle_deg", (int)target),
    rover_log_field_int("speed_pct", spd),
    rover_log_field_int("timeout_ms", timeout_ms),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_turn",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);

  float turned = 0.0f;
  TickType_t start_tick = xTaskGetTickCount();
  uint32_t prev_ms = (uint32_t)(esp_log_timestamp());
  while (turned < target &&
         (xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(timeout_ms)) {
    float gx = 0, gy = 0, gz = 0;
    M5.Imu.getGyro(&gx, &gy, &gz);
    uint32_t now_ms = (uint32_t)(esp_log_timestamp());
    float dt_s = (float)(now_ms - prev_ms) / 1000.0f;
    prev_ms = now_ms;

    (void)rover_set_speed(0, 0, turn_z);
    float rate = fabsf(gx);
    if (fabsf(gy) > rate) rate = fabsf(gy);
    if (fabsf(gz) > rate) rate = fabsf(gz);
    if (rate > 3.0f) {
      turned += rate * dt_s;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  (void)rover_set_speed(0, 0, 0);
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  set_motion(0, 0, 0, false);
  xSemaphoreGive(s_state_mutex);

  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"status\":\"%s\",\"action\":\"turn\",\"target_deg\":%.1f,\"measured_deg\":%.1f}",
           turned >= target ? "ok" : "timeout", (double)target, (double)turned);
  return strdup(payload); // openrouter_client frees this
}

static char *cb_stop(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)arguments; (void)ud;
  mark_activity();
  rover_emergency_stop();
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  set_motion(0, 0, 0, false);
  s_web_motion_deadline = 0;
  xSemaphoreGive(s_state_mutex);
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_stop",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);
  return make_tool_response("ok", "stop"); // openrouter_client frees this
}

static char *cb_gripper_open(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)arguments; (void)ud;
  mark_activity();
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_gripper_open = true;
  xSemaphoreGive(s_state_mutex);
  (void)rover_set_servo_angle(kGripperServo, kGripperOpenAngle);
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_gripper_open",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);
  return make_tool_response("ok", "gripper_open"); // openrouter_client frees this
}

static char *cb_gripper_close(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)arguments; (void)ud;
  mark_activity();
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  s_gripper_open = false;
  xSemaphoreGive(s_state_mutex);
  (void)rover_set_servo_angle(kGripperServo, kGripperCloseAngle);
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_gripper_close",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);
  return make_tool_response("ok", "gripper_close"); // openrouter_client frees this
}

static char *cb_read_imu(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)arguments; (void)ud;
  if (!M5.Imu.isEnabled()) {
    return make_tool_response("imu_unavailable", "read_imu");
  }
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  bool ok = M5.Imu.getAccel(&ax, &ay, &az) && M5.Imu.getGyro(&gx, &gy, &gz);
  if (!ok) {
    return make_tool_response("imu_read_failed", "read_imu");
  }
  char buf[224];
  snprintf(buf, sizeof(buf),
           "{\"status\":\"ok\",\"accel\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
           "\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}",
           (double)ax, (double)ay, (double)az,
           (double)gx, (double)gy, (double)gz);
  return strdup(buf);
}

static char *cb_vision_scan(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)ud;
  const char *mode = "RELIABLE";
  cJSON *args = cJSON_Parse(arguments ? arguments : "{}");
  if (args) {
    cJSON *m = cJSON_GetObjectItem(args, "mode");
    if (m && cJSON_IsString(m) && strcasecmp(m->valuestring, "fast") == 0) {
      mode = "FAST";
    }
    cJSON_Delete(args);
  }

  char cmd_args[64];
  snprintf(cmd_args, sizeof(cmd_args), "{\"mode\":\"%s\",\"frames\":1}", mode);

  mark_activity();
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_vision_scan",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);

  char resp[VISION_RESP_MAX];
  xSemaphoreTake(s_vision_mutex, portMAX_DELAY);
  esp_err_t err = vision_cmd("SCAN", cmd_args, resp, sizeof(resp));
  xSemaphoreGive(s_vision_mutex);

  if (err != ESP_OK) {
    return make_tool_response("camera_timeout", "vision_scan");
  }

  // Parse and extract result for clean AI response
  cJSON *json = cJSON_Parse(resp);
  if (!json) return strdup(resp);

  cJSON *ok_field = cJSON_GetObjectItem(json, "ok");
  cJSON *result = cJSON_GetObjectItem(json, "result");
  if (ok_field && cJSON_IsTrue(ok_field) && result) {
    if (!s_vision_available) {
      s_vision_available = true;
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
        .event = "vision_available_via_ai",
        .fields = NULL,
        .field_count = 0,
      };
      rover_log(&rec);
    }
    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(json);
    return result_str ? result_str : make_tool_response("memory_error", "vision_scan");
  }

  // Return raw error from camera
  char *raw = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  return raw ? raw : make_tool_response("error", "vision_scan");
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
    rover_log_record_t start_rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "web_chat_start",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&start_rec);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    transition_to(STATE_AI_THINKING);
    xSemaphoreGive(s_state_mutex);

    if (s_ai == NULL) {
      err = ESP_ERR_INVALID_STATE;
      strlcpy(response, "AI unavailable", sizeof(response));
    } else {
      xSemaphoreTake(s_ai_mutex, portMAX_DELAY);
      err = openrouter_call_with_tools(s_ai, job.prompt, response, sizeof(response), 5);
      xSemaphoreGive(s_ai_mutex);
    }

    // Safety: on AI error, stop motors
    if (err != ESP_OK) {
      rover_emergency_stop();
      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      set_motion(0, 0, 0, false);
      transition_to(STATE_IDLE);
      xSemaphoreGive(s_state_mutex);
    } else {
      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      transition_to(STATE_IDLE);
      xSemaphoreGive(s_state_mutex);
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

    rover_log_field_t fields[] = {
      rover_log_field_str("status", err == ESP_OK ? "ok" : "failed"),
    };
    rover_log_record_t done_rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "web_chat_done",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&done_rec);
  }
}

static esp_err_t handle_root(httpd_req_t *req) {
  static const char *html =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
      "<title>AI Rover</title>"
      "<style>"
      "*{box-sizing:border-box}"
      "body{font-family:system-ui,-apple-system,sans-serif;background:#0b1220;color:#e5e7eb;"
      "margin:0;padding:12px;touch-action:manipulation}"
      "h1{font-size:18px;margin:0 0 10px}h2{font-size:15px;margin:14px 0 6px}"
      ".card{background:#111827;border:1px solid #1f2937;border-radius:10px;padding:12px;margin-bottom:10px}"
      ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
      "button{background:#1f2937;color:#e5e7eb;border:1px solid #374151;border-radius:8px;padding:10px 14px;"
      "font-size:14px;cursor:pointer;flex:1;min-width:60px}button:active{background:#374151}"
      ".danger{background:#7f1d1d;border-color:#991b1b}"
      ".pill{display:inline-block;padding:3px 10px;border-radius:12px;font-size:12px;font-weight:600}"
      "textarea{width:100%;background:#0f172a;color:#e5e7eb;border:1px solid #334155;"
      "border-radius:8px;padding:10px;min-height:80px;resize:vertical}"
      "pre{white-space:pre-wrap;word-break:break-word;background:#0f172a;border:1px solid #334155;"
      "border-radius:8px;padding:10px;font-size:13px}"
      ".muted{opacity:.7;font-size:12px}"
      "#joyWrap{position:relative;margin:0 auto}"
      "canvas{display:block;margin:0 auto;border-radius:50%;background:#0f172a}"
      ".spd-row{display:flex;align-items:center;gap:8px;margin-top:8px}"
      ".spd-row input{flex:1;accent-color:#2563eb}"
      "</style></head><body>"
      "<h1>AI Rover</h1>"
      /* Status */
      "<div class='card'>"
      "<div class='row' style='justify-content:space-between'>"
      "<span class='pill' id='stPill' style='background:#2d8b2d'>IDLE</span>"
      "<span class='muted' id='stMotion'>--</span>"
      "<span class='muted' id='stGrip'>--</span>"
      "</div></div>"
      /* Joystick + rotation + speed */
      "<div class='card'><h2>Drive</h2>"
      "<div id='joyWrap'><canvas id='joy' width='180' height='180'></canvas></div>"
      "<div class='spd-row'><span class='muted'>Speed</span>"
      "<input type='range' id='spdSlider' min='10' max='100' value='80'>"
      "<span id='spdVal' class='muted'>80%</span></div>"
      "<div class='row' style='margin-top:8px'>"
      "<button onmousedown=\"holdStart('rotate_left')\" onmouseup='holdStop()' ontouchstart=\"holdStart('rotate_left')\" ontouchend='holdStop()'>&#8634; Left</button>"
      "<button class='danger' onclick=\"send('stop')\">STOP</button>"
      "<button onmousedown=\"holdStart('rotate_right')\" onmouseup='holdStop()' ontouchstart=\"holdStart('rotate_right')\" ontouchend='holdStop()'>Right &#8635;</button>"
      "</div>"
      "<div class='row' style='margin-top:8px'>"
      "<button onclick=\"send('open')\">Grip Open</button>"
      "<button onclick=\"send('close')\">Grip Close</button>"
      "</div></div>"
      /* Vision */
      "<div class='card'><h2>Vision</h2>"
      "<div class='row'>"
      "<button onclick=\"vscan('SCAN')\">Scan</button>"
      "<button onclick=\"vscan('OBJECTS')\">Objects</button>"
      "<button onclick=\"vscan('WHO')\">Who</button>"
      "<button onclick=\"vscan('PING')\">Ping</button>"
      "<button onclick=\"vcapture()\">Capture</button>"
      "</div>"
      "<img id='camImg' style='display:none;max-width:100%;margin-top:8px;"
      "border-radius:8px;border:1px solid #334155' />"
      "<pre id='visionOut' style='margin-top:8px;max-height:200px;overflow:auto'>--</pre>"
      "</div>"
      /* Chat */
      "<div class='card'><h2>Chat</h2>"
      "<textarea id='msg' placeholder='Message for rover AI...'></textarea>"
      "<div class='row' style='margin-top:8px'>"
      "<button onclick='ask()'>Send</button>"
      "<button onclick='poll()'>Poll</button>"
      "</div>"
      "<div class='muted' id='chatInfo' style='margin-top:6px'>idle</div>"
      "<pre id='chatOut'></pre>"
      "</div>"
      /* Script */
      "<script>"
      "const C=document.getElementById('joy'),ctx=C.getContext('2d');"
      "const R=90,DR=30;"
      "let jx=0,jy=0,jDown=false,jTimer=0;"
      "let holdAct='',holdT=0,lastId=0;"
      "const spd=()=>parseInt(document.getElementById('spdSlider').value);"
      "document.getElementById('spdSlider').oninput=function(){document.getElementById('spdVal').textContent=this.value+'%'};"
      /* draw joystick */
      "function drawJ(){"
      "ctx.clearRect(0,0,180,180);"
      "ctx.beginPath();ctx.arc(R,R,R-2,0,Math.PI*2);ctx.fillStyle='#1f2937';ctx.fill();ctx.strokeStyle='#374151';ctx.lineWidth=2;ctx.stroke();"
      "ctx.beginPath();ctx.moveTo(R,15);ctx.lineTo(R,R*2-15);ctx.moveTo(15,R);ctx.lineTo(R*2-15,R);"
      "ctx.strokeStyle='#374151';ctx.lineWidth=1;ctx.stroke();"
      "let dx=jx*(R-DR)/100,dy=-jy*(R-DR)/100;"
      "ctx.beginPath();ctx.arc(R+dx,R+dy,DR,0,Math.PI*2);ctx.fillStyle=jDown?'#2563eb':'#4b5563';ctx.fill();"
      "ctx.strokeStyle='#60a5fa';ctx.lineWidth=2;ctx.stroke();"
      "}"
      /* joystick events */
      "function jPos(e){"
      "const r=C.getBoundingClientRect();"
      "let t=e.touches?e.touches[0]:e;"
      "let px=t.clientX-r.left-R,py=t.clientY-r.top-R;"
      "let d=Math.sqrt(px*px+py*py),mx=R-DR;"
      "if(d>mx){px=px/d*mx;py=py/d*mx;}"
      "jx=Math.round(px/mx*100);jy=Math.round(-py/mx*100);"
      "drawJ();}"
      "function jStart(e){e.preventDefault();jDown=true;jPos(e);"
      "if(!jTimer)jTimer=setInterval(jSend,100);}"
      "function jMove(e){e.preventDefault();if(jDown)jPos(e);}"
      "function jEnd(e){e.preventDefault();jDown=false;jx=0;jy=0;drawJ();jSend();"
      "if(jTimer){clearInterval(jTimer);jTimer=0;}}"
      "C.addEventListener('mousedown',jStart);C.addEventListener('mousemove',jMove);"
      "C.addEventListener('mouseup',jEnd);C.addEventListener('mouseleave',jEnd);"
      "C.addEventListener('touchstart',jStart,{passive:false});"
      "C.addEventListener('touchmove',jMove,{passive:false});"
      "C.addEventListener('touchend',jEnd,{passive:false});"
      /* send joystick */
      "function jSend(){"
      "let s=spd()/100;"
      "let sy=Math.round(jy*s),sz=Math.round(jx*s);"
      "fetch('/cmd?act=move&x=0&y='+sy+'&z='+sz).catch(()=>{});}"
      /* simple command */
      "async function send(a){try{await fetch('/cmd?act='+encodeURIComponent(a));}catch(e){}refresh();}"
      /* hold buttons for rotation */
      "function holdStart(a){holdAct=a;send(a);if(holdT)clearInterval(holdT);holdT=setInterval(()=>send(holdAct),300);}"
      "function holdStop(){if(holdT){clearInterval(holdT);holdT=0;}if(holdAct){send('stop');holdAct='';}}"
      /* status refresh */
      "const stColors={IDLE:'#2d8b2d',WEB_CTRL:'#2563eb',AI_THINK:'#d97706',AI_EXEC:'#7c3aed',OFFLINE:'#dc2626'};"
      "async function refresh(){try{const r=await fetch('/status');const j=await r.json();"
      "const p=document.getElementById('stPill');p.textContent=j.state||'?';p.style.background=stColors[j.state]||'#374151';"
      "document.getElementById('stMotion').textContent=j.motion?'Moving x:'+j.x+' y:'+j.y+' z:'+j.z:'Stopped';"
      "document.getElementById('stGrip').textContent='Grip: '+j.gripper;"
      "}catch(e){document.getElementById('stPill').textContent='ERR';}}"
      /* chat */
      "async function ask(){const m=document.getElementById('msg').value.trim();if(!m)return;"
      "document.getElementById('chatInfo').textContent='sending...';"
      "const r=await fetch('/chat',{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:m});"
      "const t=await r.text();"
      "document.getElementById('chatInfo').textContent=t;"
      "try{const j=JSON.parse(t);if(j.id){lastId=j.id;setTimeout(poll,600);}}catch(_){}}"
      "async function poll(){if(!lastId){document.getElementById('chatInfo').textContent='no chat id';return;}"
      "const r=await fetch('/chat_result?id='+lastId);const t=await r.text();"
      "if(r.status===202||t==='pending'){document.getElementById('chatInfo').textContent='pending id='+lastId;"
      "setTimeout(poll,900);return;}"
      "document.getElementById('chatInfo').textContent='done id='+lastId;"
      "document.getElementById('chatOut').textContent=t;}"
      /* vision */
      "async function vscan(c){"
      "document.getElementById('visionOut').textContent='scanning...';"
      "try{const r=await fetch('/vision?cmd='+c);"
      "const t=await r.text();"
      "try{document.getElementById('visionOut').textContent=JSON.stringify(JSON.parse(t),null,2);}"
      "catch(_){document.getElementById('visionOut').textContent=t;}}"
      "catch(e){document.getElementById('visionOut').textContent='error: '+e;}}"
      "async function vcapture(){"
      "const vo=document.getElementById('visionOut'),img=document.getElementById('camImg');"
      "vo.textContent='capturing...';"
      "try{const r=await fetch('/vision?cmd=CAPTURE&quality=75');"
      "if(!r.ok){vo.textContent='capture failed: '+r.status;return;}"
      "const b=await r.blob();"
      "const u=URL.createObjectURL(b);"
      "img.onload=function(){URL.revokeObjectURL(u);};"
      "img.src=u;img.style.display='block';"
      "vo.textContent='captured '+b.size+' bytes';}"
      "catch(e){vo.textContent='error: '+e;}}"
      "drawJ();setInterval(refresh,1500);refresh();"
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t *req) {
  char body[256];
  int16_t vbus_mv = 0;
  int32_t bat_pct = -1;
  read_power_metrics(&vbus_mv, &bat_pct);
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  int n = snprintf(body,
                   sizeof(body),
                   "{\"state\":\"%s\",\"motion\":%d,\"x\":%d,\"y\":%d,\"z\":%d,"
                   "\"gripper\":\"%s\",\"vision\":\"%s\","
                   "\"bat_pct\":%d,\"vbus_mv\":%d}",
                   state_name(s_rover_state),
                   s_motion_active ? 1 : 0,
                   s_motion_x,
                   s_motion_y,
                   s_motion_z,
                   s_gripper_open ? "open" : "close",
                   s_vision_available ? "ok" : "offline",
                   (int)bat_pct,
                   (int)vbus_mv);
  xSemaphoreGive(s_state_mutex);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, n);
}

static esp_err_t handle_vision(httpd_req_t *req) {
  char query[96] = {0};
  char cmd[16] = "SCAN";
  char mode[16] = "RELIABLE";
  char quality_str[8] = "";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "cmd", cmd, sizeof(cmd));
    (void)httpd_query_key_value(query, "mode", mode, sizeof(mode));
    (void)httpd_query_key_value(query, "quality", quality_str, sizeof(quality_str));
  }

  // Whitelist commands
  if (strcmp(cmd, "SCAN") != 0 && strcmp(cmd, "OBJECTS") != 0 &&
      strcmp(cmd, "WHO") != 0 && strcmp(cmd, "PING") != 0 &&
      strcmp(cmd, "INFO") != 0 && strcmp(cmd, "CAPTURE") != 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid cmd\"}", HTTPD_RESP_USE_STRLEN);
  }

  mark_activity();

  // CAPTURE: binary JPEG response
  if (strcmp(cmd, "CAPTURE") == 0) {
    int quality = kCaptureDefaultQuality;
    if (quality_str[0] != '\0') {
      int q = atoi(quality_str);
      if (q >= 10 && q <= 95) quality = q;
    }
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    xSemaphoreTake(s_vision_mutex, portMAX_DELAY);
    esp_err_t err = vision_capture(quality, &jpeg, &jpeg_size);
    xSemaphoreGive(s_vision_mutex);
    if (err != ESP_OK) {
      s_vision_available = false;
      httpd_resp_set_status(req, "504 Gateway Timeout");
      httpd_resp_set_type(req, "application/json");
      return httpd_resp_send(req, "{\"ok\":false,\"error\":\"capture failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    s_vision_available = true;
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t send_err = httpd_resp_send(req, (const char *)jpeg, jpeg_size);
    free(jpeg);
    return send_err;
  }

  // Text commands
  char args_json[64];
  if (strcmp(cmd, "PING") == 0 || strcmp(cmd, "INFO") == 0) {
    strlcpy(args_json, "{}", sizeof(args_json));
  } else {
    snprintf(args_json, sizeof(args_json), "{\"mode\":\"%s\",\"frames\":1}", mode);
  }

  char resp[VISION_RESP_MAX];
  xSemaphoreTake(s_vision_mutex, portMAX_DELAY);
  esp_err_t err = vision_cmd(cmd, args_json, resp, sizeof(resp));
  xSemaphoreGive(s_vision_mutex);

  httpd_resp_set_type(req, "application/json");
  if (err != ESP_OK) {
    s_vision_available = false;
    httpd_resp_set_status(req, "504 Gateway Timeout");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"camera timeout\"}", HTTPD_RESP_USE_STRLEN);
  }
  // Update availability on any successful response
  if (!s_vision_available && strstr(resp, "\"ok\":true") != NULL) {
    s_vision_available = true;
    rover_log_record_t rec1 = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "vision_status_online",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec1);
    rover_log_record_t rec2 = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "vision_available",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec2);
  }
  return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
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

  if (strcmp(action, "move") == 0) {
    // Joystick: /cmd?act=move&x=..&y=..&z=..
    char val[16];
    int x = 0, y = 0, z = 0;
    if (httpd_query_key_value(query, "x", val, sizeof(val)) == ESP_OK) x = atoi(val);
    if (httpd_query_key_value(query, "y", val, sizeof(val)) == ESP_OK) y = atoi(val);
    if (httpd_query_key_value(query, "z", val, sizeof(val)) == ESP_OK) z = atoi(val);
    x = clamp_int(x, -100, 100);
    y = clamp_int(y, -100, 100);
    z = clamp_int(z, -100, 100);
    mark_activity();
    bool active = (x != 0 || y != 0 || z != 0);
    set_motion((int8_t)x, (int8_t)y, (int8_t)z, active);
    s_web_motion_deadline = active ? xTaskGetTickCount() + pdMS_TO_TICKS(1500) : 0;
  } else {
    apply_action(action, true);
  }

  if (s_rover_state == STATE_IDLE || s_rover_state == STATE_WEB_CONTROL) {
    transition_to(s_motion_active ? STATE_WEB_CONTROL : STATE_IDLE);
  }
  apply_motion();
  xSemaphoreGive(s_state_mutex);

  httpd_resp_set_type(req, "application/json");
  char resp[96];
  int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"act\":\"%s\"}", action);
  return httpd_resp_send(req, resp, n);
}

static esp_err_t handle_chat(httpd_req_t *req) {
  char query[1024];
  char prompt[CHAT_PROMPT_MAX];
  query[0] = '\0';
  prompt[0] = '\0';

  if (req->method == HTTP_POST && req->content_len > 0) {
    int to_read = req->content_len;
    if (to_read >= CHAT_PROMPT_MAX) {
      to_read = CHAT_PROMPT_MAX - 1;
    }
    int read_total = 0;
    while (read_total < to_read) {
      int r = httpd_req_recv(req, prompt + read_total, to_read - read_total);
      if (r <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad request body\"}",
                              HTTPD_RESP_USE_STRLEN);
      }
      read_total += r;
    }
    prompt[read_total] = '\0';
  } else if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "msg", prompt, CHAT_PROMPT_MAX);
  }
  if (prompt[0] == '\0') {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing msg\"}", HTTPD_RESP_USE_STRLEN);
  }
  if (s_ai == NULL || s_chat_queue == NULL) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"ai unavailable\"}", HTTPD_RESP_USE_STRLEN);
  }

  chat_job_t job = {};
  xSemaphoreTake(s_chat_mutex, portMAX_DELAY);
  if (s_chat_pending) {
    xSemaphoreGive(s_chat_mutex);
    httpd_resp_set_status(req, "429 Too Many Requests");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"chat busy\"}", HTTPD_RESP_USE_STRLEN);
  }
  s_chat_id++;
  job.id = s_chat_id;
  strlcpy(job.prompt, prompt, sizeof(job.prompt));
  s_chat_pending = true;
  xSemaphoreGive(s_chat_mutex);

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

static void start_mdns(void) {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set("ai-rover"));
  ESP_ERROR_CHECK(mdns_instance_name_set("AI Rover Web Interface"));

  mdns_txt_item_t txt[] = {
      {(char *)"path", (char *)"/"},
      {(char *)"api_cmd", (char *)"/cmd"},
      {(char *)"api_status", (char *)"/status"},
      {(char *)"api_vision", (char *)"/vision"},
      {(char *)"api_chat", (char *)"/chat"},
      {(char *)"api_chat_result", (char *)"/chat_result"},
  };
  ESP_ERROR_CHECK(mdns_service_add("AI Rover", "_http", "_tcp", 80, txt,
                                   sizeof(txt) / sizeof(txt[0])));
  rover_log_field_t mdns_fields[] = {
    rover_log_field_str("host", "ai-rover.local"),
  };
  rover_log_record_t mdns_rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "mdns_started",
    .fields = mdns_fields,
    .field_count = sizeof(mdns_fields) / sizeof(mdns_fields[0]),
  };
  rover_log(&mdns_rec);
}

static void start_web_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));
  httpd_handle_t server = s_httpd;

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL};
  httpd_uri_t cmd = {.uri = "/cmd", .method = HTTP_GET, .handler = handle_cmd, .user_ctx = NULL};
  httpd_uri_t chat = {.uri = "/chat", .method = HTTP_GET, .handler = handle_chat, .user_ctx = NULL};
  httpd_uri_t chat_post = {
      .uri = "/chat", .method = HTTP_POST, .handler = handle_chat, .user_ctx = NULL};
  httpd_uri_t chat_result = {
      .uri = "/chat_result", .method = HTTP_GET, .handler = handle_chat_result, .user_ctx = NULL};
  httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL};
  httpd_uri_t vision = {.uri = "/vision", .method = HTTP_GET, .handler = handle_vision, .user_ctx = NULL};

  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cmd));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat_post));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat_result));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &vision));
}

static void init_ai(void) {
  static const char *kTurnDirEnum[] = {"left", "right", NULL};
  static const openrouter_param_t kMoveParams[] = {
      {"x", "number", "Lateral speed -100..100 (left negative)", true, NULL},
      {"y", "number", "Forward speed -100..100 (back negative)", true, NULL},
      {"z", "number", "Rotation speed -100..100", false, NULL},
      {"duration_ms", "number", "Move duration ms (100-5000, default 1500)", false, NULL},
      {NULL, NULL, NULL, false, NULL},
  };
  static const openrouter_param_t kTurnParams[] = {
      {"direction", "string", "Turn direction", true, kTurnDirEnum},
      {"angle_deg", "number", "Target angle in degrees (5-360)", false, NULL},
      {"speed_percent", "number", "Rotation speed percent (20-100)", false, NULL},
      {NULL, NULL, NULL, false, NULL},
  };
  static const openrouter_simple_function_t kTools[] = {
      {"move", "Move the rover for duration_ms, then stop.", kMoveParams, cb_move, NULL},
      {"turn", "Rotate the rover in place by angle_deg using IMU gyroscope feedback.", kTurnParams, cb_turn, NULL},
      {"stop", "Stop all rover motion immediately.", NULL, cb_stop, NULL},
      {"gripper_open", "Open the rover gripper.", NULL, cb_gripper_open, NULL},
      {"gripper_close", "Close the rover gripper.", NULL, cb_gripper_close, NULL},
      {"read_imu", "Read current accelerometer and gyroscope values.", NULL, cb_read_imu, NULL},
      {"vision_scan", "Look at the scene using the camera. Returns detected faces and objects.", NULL, cb_vision_scan, NULL},
  };

  openrouter_config_t cfg = {};
  cfg.api_key = OPENROUTER_API_KEY;
  cfg.enable_streaming = false;
  cfg.enable_tools = true;
  cfg.max_tokens = 256;
  cfg.default_model = "openai/gpt-4o-mini";
  cfg.default_system_role =
      "You are the AI brain of a mecanum-wheel rover robot with a gripper and camera. "
      "Use the provided tools to control the rover when the user asks. "
      "For movement commands with duration, call move() which blocks for the specified time then stops. "
      "For angle-based rotations, use turn(direction, angle_deg) which uses IMU feedback. "
      "You can inspect sensors with read_imu(). "
      "Use vision_scan() to look at the scene — it returns detected faces (person field) and objects. "
      "You can chain multiple tool calls for sequences like 'look around then move forward'. "
      "Respond naturally in the user's language. Be brief.";
  s_ai = openrouter_create(&cfg);
  if (s_ai == NULL) {
    rover_log_record_t rec1 = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "ai_openrouter_init_failed",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec1);
    rover_log_record_t rec2 = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "ai_init_failed",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec2);
    return;
  }

  esp_err_t reg_err = ESP_OK;
  for (size_t i = 0; i < sizeof(kTools) / sizeof(kTools[0]) && reg_err == ESP_OK; i++) {
    reg_err = openrouter_register_simple_function(s_ai, &kTools[i]);
  }
  if (reg_err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("err", esp_err_to_name(reg_err)),
    };
    rover_log_record_t rec1 = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "ai_tool_registration_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec1);
    rover_log_record_t rec2 = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "ai_tools_failed",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec2);
  } else {
    rover_log_record_t rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "ai_init_ok",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
  }
}

static uint32_t state_color(rover_state_t s) {
  switch (s) {
    case STATE_IDLE:             return 0x2D8B2Du; // green
    case STATE_WEB_CONTROL:      return 0x2563EBu; // blue
    case STATE_AI_THINKING:      return 0xD97706u; // amber
    case STATE_AI_EXECUTING:     return 0x7C3AEDu; // purple
    case STATE_OFFLINE_FALLBACK: return 0xDC2626u; // red
    default:                     return 0x374151u; // gray
  }
}

static const char *motion_label(int8_t x, int8_t y, int8_t z) {
  if (z < 0)       return "ROTATE L";
  if (z > 0)       return "ROTATE R";
  if (y > 0 && x == 0)  return "FORWARD";
  if (y < 0 && x == 0)  return "BACK";
  if (x < 0 && y == 0)  return "LEFT";
  if (x > 0 && y == 0)  return "RIGHT";
  if (x != 0 || y != 0) return "MOVE";
  return "STOP";
}

static void get_ip_str(char *buf, size_t len) {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info;
  if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
  } else {
    strlcpy(buf, "---.---.---.---", len);
  }
}

static void update_local_display(bool btn_a, bool btn_b, bool chat_active) {
  (void)chat_active; // FSM state in the bar already shows AI status

  static bool initialized = false;
  static int8_t prev_motion_x = 0;
  static int8_t prev_motion_y = 0;
  static int8_t prev_motion_z = 0;
  static bool prev_motion_active = false;
  static bool prev_gripper_open = false;
  static bool prev_btn_a = false;
  static bool prev_btn_b = false;
  static rover_state_t prev_state = STATE_IDLE;
  static int32_t prev_bat_pct = -1;

  int32_t bat_pct = -1;
  read_power_metrics(NULL, &bat_pct);

  if (initialized &&
      prev_state == s_rover_state &&
      prev_motion_x == s_motion_x &&
      prev_motion_y == s_motion_y &&
      prev_motion_z == s_motion_z &&
      prev_motion_active == s_motion_active &&
      prev_gripper_open == s_gripper_open &&
      prev_btn_a == btn_a &&
      prev_btn_b == btn_b &&
      prev_bat_pct == bat_pct) {
    return;
  }

  prev_state = s_rover_state;
  prev_motion_x = s_motion_x;
  prev_motion_y = s_motion_y;
  prev_motion_z = s_motion_z;
  prev_motion_active = s_motion_active;
  prev_gripper_open = s_gripper_open;
  prev_btn_a = btn_a;
  prev_btn_b = btn_b;
  prev_bat_pct = bat_pct;
  initialized = true;

  // Layout: 240x135, 5 rows packed tight
  // Row 0 (y=0..23):   FSM state bar (colored)
  // Row 1 (y=26..49):  IP address bar (dark)
  // Row 2 (y=52..71):  Motion info
  // Row 3 (y=74..93):  Three pills: gripper | wifi | battery
  // Row 4 (y=96..135):  Button hints + extra info

  const uint32_t bg = 0x111827u;

  M5.Display.startWrite();
  M5.Display.fillScreen(bg);

  // ── Row 0: FSM state ──
  uint32_t sc = state_color(s_rover_state);
  M5.Display.fillRoundRect(2, 2, 236, 22, 4, sc);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, sc);
  const char *sname = state_name(s_rover_state);
  int snw = (int)strlen(sname) * 12;
  M5.Display.setCursor((240 - snw) / 2, 5);
  M5.Display.print(sname);

  // ── Row 1: IP address ──
  char ip_str[20];
  get_ip_str(ip_str, sizeof(ip_str));
  M5.Display.fillRoundRect(2, 34, 236, 22, 4, 0x1F2937u);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(s_wifi_connected ? 0x60A5FAu : 0x6B7280u, 0x1F2937u);
  int ipw = (int)strlen(ip_str) * 12;
  M5.Display.setCursor((240 - ipw) / 2, 37);
  M5.Display.print(ip_str);

  // ── Row 2: Motion ──
  if (s_motion_active) {
    const char *ml = motion_label(s_motion_x, s_motion_y, s_motion_z);
    char motion_str[48];
    snprintf(motion_str, sizeof(motion_str), "%s  x:%d y:%d z:%d",
             ml, s_motion_x, s_motion_y, s_motion_z);
    int mlen = (int)strlen(motion_str);
    bool small = (mlen * 12 > 236);
    M5.Display.setTextSize(small ? 1 : 2);
    M5.Display.setTextColor(0x60A5FAu, bg);
    int msw = mlen * (small ? 6 : 12);
    M5.Display.setCursor((240 - msw) / 2, 64);
    M5.Display.print(motion_str);
  } else {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x4B5563u, bg);
    M5.Display.setCursor((240 - 7 * 6) / 2, 68);
    M5.Display.print("Stopped");
  }

  // ── Row 3: Three pills ──
  const int py = 93;
  M5.Display.setTextSize(1);

  uint32_t gc = s_gripper_open ? 0x10B981u : 0xEF4444u;
  M5.Display.fillRoundRect(4, py, 74, 18, 4, gc);
  M5.Display.setTextColor(TFT_WHITE, gc);
  const char *gl = s_gripper_open ? "GRIP OPEN" : "GRIP SHUT";
  int glw = (int)strlen(gl) * 6;
  M5.Display.setCursor(4 + (74 - glw) / 2, py + 5);
  M5.Display.print(gl);

  uint32_t wc = s_wifi_connected ? 0x1E40AFu : 0x7F1D1Du;
  M5.Display.fillRoundRect(82, py, 74, 18, 4, wc);
  M5.Display.setTextColor(TFT_WHITE, wc);
  const char *wl = s_wifi_connected ? "WiFi OK" : "OFFLINE";
  int wlw = (int)strlen(wl) * 6;
  M5.Display.setCursor(82 + (74 - wlw) / 2, py + 5);
  M5.Display.print(wl);

  uint32_t bc = bat_pct > 20 ? 0x1F2937u : 0x991B1Bu;
  M5.Display.fillRoundRect(160, py, 76, 18, 4, bc);
  M5.Display.setTextColor(TFT_WHITE, bc);
  char bat_label[16];
  snprintf(bat_label, sizeof(bat_label), "BAT %d%%", (int)bat_pct);
  int blw = (int)strlen(bat_label) * 6;
  M5.Display.setCursor(160 + (76 - blw) / 2, py + 5);
  M5.Display.print(bat_label);

  // ── Row 4: Button hints ──
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(0x6B7280u, bg);
  M5.Display.setCursor(4, 122);
  M5.Display.print("[A] Drive");
  if (btn_a) M5.Display.fillCircle(64, 126, 3, 0x10B981u);
  M5.Display.setCursor(140, 122);
  M5.Display.print("[B] E-Stop");
  if (btn_b) M5.Display.fillCircle(202, 126, 3, 0xEF4444u);

  M5.Display.endWrite();
}

// ── Syslog queue task ──

static void syslog_task(void *arg) {
  (void)arg;
  static char msg[kSyslogMsgMax];
  static char payload[kSyslogPayloadMax];
  while (1) {
    if (xQueueReceive(s_syslog_queue, msg, portMAX_DELAY) == pdTRUE) {
      if (s_syslog_sock >= 0) {
        int n = snprintf(payload, sizeof(payload),
                         "<134>1 - ai-rover firmware - - - %s", msg);
        if (n > 0) {
          if (n >= (int)sizeof(payload)) n = (int)sizeof(payload) - 1;
          (void)send(s_syslog_sock, payload, (size_t)n, 0);
        }
      }
    }
  }
}

// ── WiFi reconnect task (for offline fallback) ──

static void wifi_reconnect_task(void *arg) {
  (void)arg;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(15000));
    if (s_wifi_connected) continue;

    rover_log_record_t rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "wifi_reconnect_start",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    s_retry_num = 0;
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) continue;

    esp_event_handler_instance_t inst_any, inst_ip;
    esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any);
    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip);

    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, kWifiConnectTimeout);

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_any);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
      s_wifi_connected = true;
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

      s_syslog_sock = open_syslog_socket();

      if (s_ai == NULL) init_ai();
      start_mdns();
      if (s_httpd == NULL) start_web_server();

      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      transition_to(STATE_IDLE);
      xSemaphoreGive(s_state_mutex);
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
      .event = "wifi_reconnect_services_restored",
        .fields = NULL,
        .field_count = 0,
      };
      rover_log(&rec);
    }
  }
}

static void vision_ping_task(void *arg) {
  (void)arg;

  TickType_t last_vision_ping = 0;
  while (1) {
    TickType_t now = xTaskGetTickCount();
    if ((now - last_vision_ping) >= kVisionPingPeriod) {
      last_vision_ping = now;
      if (xSemaphoreTake(s_vision_mutex, 0) == pdTRUE) {
        char ping_resp[128];
        esp_err_t ping_err =
            vision_cmd_timeout("PING", "{}", ping_resp, sizeof(ping_resp), kVisionPingTimeoutMs);
        xSemaphoreGive(s_vision_mutex);
        bool was = s_vision_available;
        s_vision_available = (ping_err == ESP_OK && strstr(ping_resp, "\"ok\":true") != NULL);
        if (ping_err != ESP_OK) {
          rover_log_field_t fields[] = {
            rover_log_field_str("result", "error"),
            rover_log_field_str("err", esp_err_to_name(ping_err)),
          };
          rover_log_record_t rec = {
            .level = ESP_LOG_DEBUG,
            .component = TAG,
            .event = "vision_ping",
            .fields = fields,
            .field_count = sizeof(fields) / sizeof(fields[0]),
          };
          rover_log(&rec);
        } else if (!s_vision_available) {
          rover_log_field_t fields[] = {
            rover_log_field_str("result", "bad_response"),
            rover_log_field_int("resp_len", (int64_t)strlen(ping_resp)),
          };
          rover_log_record_t rec = {
            .level = ESP_LOG_DEBUG,
            .component = TAG,
            .event = "vision_ping",
            .fields = fields,
            .field_count = sizeof(fields) / sizeof(fields[0]),
          };
          rover_log(&rec);
        }
        if (s_vision_available != was) {
          rover_log_field_t fields[] = {
            rover_log_field_str("status", s_vision_available ? "online" : "offline"),
          };
          rover_log_record_t rec = {
            .level = ESP_LOG_INFO,
            .component = TAG,
            .event = "vision_status",
            .fields = fields,
            .field_count = sizeof(fields) / sizeof(fields[0]),
          };
          rover_log(&rec);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ── Main loop task (Core 0) ──

static void main_loop_task(void *arg) {
  (void)arg;

  esp_task_wdt_add(NULL);

  bool prev_btn_a = false;
  bool prev_btn_b = false;
  TickType_t last_hb = 0;

  while (1) {
    esp_task_wdt_reset();

    M5.update();
    bool btn_a = M5.BtnA.isPressed();
    bool btn_b = M5.BtnB.isPressed();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (btn_b && !prev_btn_b) {
      mark_activity();
      rover_emergency_stop();
      set_motion(0, 0, 0, false);
      s_web_motion_deadline = 0;
      s_gripper_open = !s_gripper_open;
      (void)rover_set_servo_angle(kGripperServo, s_gripper_open ? kGripperOpenAngle : kGripperCloseAngle);
      transition_to(STATE_IDLE);
      rover_log_field_t fields[] = {
        rover_log_field_str("button", "B"),
        rover_log_field_str("action", "stop"),
        rover_log_field_str("gripper", s_gripper_open ? "open" : "close"),
      };
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
        .event = "button_action",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&rec);
    }

    if (btn_a && btn_b) {
      mark_activity();
      set_motion(0, 0, 60, true);
      s_web_motion_deadline = 0;
    } else if (btn_a) {
      mark_activity();
      set_motion(0, kMoveSpeed, 0, true);
      s_web_motion_deadline = 0;
    } else if (s_web_motion_deadline != 0 && xTaskGetTickCount() > s_web_motion_deadline) {
      set_motion(0, 0, 0, false);
      s_web_motion_deadline = 0;
      if (s_rover_state == STATE_WEB_CONTROL) {
        transition_to(STATE_IDLE);
      }
    }

    if (!btn_a && prev_btn_a && s_web_motion_deadline == 0) {
      mark_activity();
      set_motion(0, 0, 0, false);
      rover_log_field_t fields[] = {
        rover_log_field_str("button", "A"),
        rover_log_field_str("action", "stop"),
      };
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
        .event = "button_action",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&rec);
    }
    if (btn_a && !prev_btn_a) {
      mark_activity();
      rover_log_field_t fields[] = {
        rover_log_field_str("button", "A"),
        rover_log_field_str("action", "active"),
      };
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
        .event = "button_action",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&rec);
    }

    apply_motion();
    xSemaphoreGive(s_state_mutex);

    bool chat_pending = false;
    xSemaphoreTake(s_chat_mutex, portMAX_DELAY);
    chat_pending = s_chat_pending;
    xSemaphoreGive(s_chat_mutex);

    update_local_display(btn_a, btn_b, chat_pending);

    TickType_t now = xTaskGetTickCount();
    if ((now - last_hb) >= kHeartbeatPeriod) {
      int32_t bat_pct = -1;
      read_power_metrics(NULL, &bat_pct);
      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      const char *state = state_name(s_rover_state);
      int moving = s_motion_active ? 1 : 0;
      int x = s_motion_x;
      int y = s_motion_y;
      int z = s_motion_z;
      const char *gripper = s_gripper_open ? "open" : "close";
      xSemaphoreGive(s_state_mutex);
      rover_log_field_t fields[] = {
        rover_log_field_str("state", state),
        rover_log_field_int("moving", moving),
        rover_log_field_int("x", x),
        rover_log_field_int("y", y),
        rover_log_field_int("z", z),
        rover_log_field_str("gripper", gripper),
        rover_log_field_int("bat_pct", (int)bat_pct),
      };
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
        .event = "heartbeat",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&rec);
      last_hb = now;
    }

    prev_btn_a = btn_a;
    prev_btn_b = btn_b;

    bool should_sleep = false;
    uint32_t activity = s_last_activity_tick.load(std::memory_order_relaxed);
    TickType_t idle_for = xTaskGetTickCount() - (TickType_t)activity;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int16_t vbus_mv = 0;
    read_power_metrics(&vbus_mv, NULL);
    bool usb_power = vbus_mv > 4000;  // USB ~5V, RoverC pogo ~0.8V
    should_sleep = (!btn_a && !btn_b &&
                    !s_motion_active &&
                    !chat_pending &&
                    !usb_power &&
                    s_rover_state == STATE_IDLE &&
                    idle_for >= kInactivitySleepTimeout);
    xSemaphoreGive(s_state_mutex);

    if (should_sleep) {
      esp_task_wdt_delete(NULL);
      enter_deep_sleep();
    }

    vTaskDelay(kLoopPeriod);
  }
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  s_state_mutex = xSemaphoreCreateMutex();
  s_i2c_mutex = xSemaphoreCreateMutex();
  s_power_mutex = xSemaphoreCreateMutex();
  s_ai_mutex = xSemaphoreCreateMutex();
  s_chat_mutex = xSemaphoreCreateMutex();
  s_vision_mutex = xSemaphoreCreateMutex();
  s_chat_queue = xQueueCreate(1, sizeof(chat_job_t));
  s_syslog_queue = xQueueCreate(8, kSyslogMsgMax);
  if (s_state_mutex == NULL || s_i2c_mutex == NULL || s_power_mutex == NULL ||
      s_ai_mutex == NULL || s_chat_mutex == NULL ||
      s_vision_mutex == NULL ||
      s_chat_queue == NULL || s_syslog_queue == NULL) {
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "init_alloc_failed_mutex_or_queue",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    esp_restart();
  }

  // Unified logger: mirror JSON UART logs to syslog queue.
  rover_log_set_sink(rover_log_syslog_sink, NULL);

  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  M5.Display.setRotation(1);
  draw_boot_status("booting...", "");
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  rover_log_field_t wake_fields[] = {
    rover_log_field_str("cause", wakeup_cause_name(wake)),
    rover_log_field_int("cause_id", (int)wake),
  };
  rover_log_record_t wake_rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "wakeup_cause",
    .fields = wake_fields,
    .field_count = sizeof(wake_fields) / sizeof(wake_fields[0]),
  };
  rover_log(&wake_rec);

  ESP_ERROR_CHECK(rover_init_i2c());

  bool vision_uart_ready = false;

  // Init vision UART (UnitV-M12 on Grove G32/G33)
  if (vision_uart_init() == ESP_OK) {
    vision_uart_ready = true;
    rover_log_field_t fields[] = {
      rover_log_field_int("tx_pin", 33),
      rover_log_field_int("rx_pin", 32),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "vision_uart_initialized",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
    // Give camera time to be ready, then ping
    vTaskDelay(pdMS_TO_TICKS(500));
    char ping_resp[128];
    xSemaphoreTake(s_vision_mutex, portMAX_DELAY);
    esp_err_t ping_err =
        vision_cmd_timeout("PING", "{}", ping_resp, sizeof(ping_resp), kVisionPingTimeoutMs);
    xSemaphoreGive(s_vision_mutex);
    if (ping_err == ESP_OK && strstr(ping_resp, "\"ok\":true") != NULL) {
      s_vision_available = true;
      rover_log_field_t fields[] = {
        rover_log_field_str("resp", ping_resp),
      };
      rover_log_record_t rec = {
        .level = ESP_LOG_INFO,
        .component = TAG,
        .event = "vision_online_boot_ping",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&rec);
    } else {
      rover_log_record_t rec = {
        .level = ESP_LOG_WARN,
        .component = TAG,
        .event = "vision_not_responding_boot_ping",
        .fields = NULL,
        .field_count = 0,
      };
      rover_log(&rec);
    }
  } else {
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "vision_uart_init_failed",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
  }

  draw_boot_status("connecting WiFi...", WIFI_SSID);
  esp_err_t wifi_err = wifi_connect_blocking();

  if (wifi_err == ESP_OK) {
    s_wifi_connected = true;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    draw_boot_status("WiFi OK", "init rover...");

    s_syslog_sock = open_syslog_socket();
    if (s_syslog_sock < 0) {
      rover_log_record_t rec = {
        .level = ESP_LOG_WARN,
        .component = TAG,
        .event = "syslog_unavailable",
        .fields = NULL,
        .field_count = 0,
      };
      rover_log(&rec);
    }

    // Open gripper on boot
    s_gripper_open = true;
    (void)rover_set_servo_angle(kGripperServo, kGripperOpenAngle);

    init_ai();
    start_mdns();
    start_web_server();
    draw_boot_status("ready", s_vision_available ? "web + chat + cam" : "web + chat online");
  } else {
    // Offline fallback — no restart, buttons still work
    s_wifi_connected = false;
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "wifi_offline_fallback",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    draw_boot_status("OFFLINE", "buttons only");

    s_gripper_open = true;
    (void)rover_set_servo_angle(kGripperServo, kGripperOpenAngle);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    transition_to(STATE_OFFLINE_FALLBACK);
    xSemaphoreGive(s_state_mutex);
  }

  mark_activity();

  // Syslog queue task — Core 1, low priority
  xTaskCreatePinnedToCore(syslog_task, "syslog", 4096, NULL, 2, NULL, 1);

  // Chat worker — Core 1 (agent core, long HTTP calls)
  xTaskCreatePinnedToCore(chat_worker_task, "chat_worker", 16384, NULL, 4, NULL, 1);

  // WiFi reconnect task — Core 1, low priority
  xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_reconn", 4096, NULL, 2, NULL, 1);

  // Vision ping task — Core 1, low priority (keeps camera health checks off main_loop)
  if (vision_uart_ready) {
    xTaskCreatePinnedToCore(vision_ping_task, "vision_ping", 4096, NULL, 2, NULL, 1);
  }

  // Main loop — Core 0 (RT core, motors, buttons, display)
  xTaskCreatePinnedToCore(main_loop_task, "main_loop", 4096, NULL, 5, NULL, 0);

  rover_log_record_t rec1 = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "init_tasks_started",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec1);
  rover_log_record_t rec2 = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "boot_complete",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec2);
  // app_main returns — FreeRTOS scheduler continues
}
