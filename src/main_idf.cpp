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
#include "cJSON.h"
#include "logger_json.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
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
static const TickType_t kLoopPeriod = pdMS_TO_TICKS(20);
static const TickType_t kAiActionQueueSendTimeout = pdMS_TO_TICKS(100);
static const TickType_t kAiActionResultTimeoutSlack = pdMS_TO_TICKS(1000);
static const TickType_t kAiStopActionTimeout = pdMS_TO_TICKS(7000);
static const int kAiActionQueueDepth = 4;
static const int kAiHttpTimeoutMs = 15000;
static const int kAiToolCallMaxIterations = 48;
static const TickType_t kWifiConnectTimeout = pdMS_TO_TICKS(30000);
static const TickType_t kInactivitySleepTimeout = pdMS_TO_TICKS(120000);
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const int kWifiMaxRetry = 20;
static const int kWifiApMaxClients = 2;
static const char *kSettingsNamespace = "rover_cfg";
static const char *kDefaultLlmModel = "openai/gpt-4o-mini";
static const char *kDefaultLlmEndpoint = "";

typedef struct {
  char wifi_ssid[33];
  char wifi_password[65];
  char llm_endpoint[256];
  char llm_api_key[257];
  char llm_model[128];
} rover_settings_t;

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
static SemaphoreHandle_t s_settings_mutex;
static SemaphoreHandle_t s_chat_mutex;
static SemaphoreHandle_t s_ai_action_queue_mutex;
static QueueHandle_t s_chat_queue;
static QueueHandle_t s_syslog_queue;
static QueueHandle_t s_ai_action_queue;
static QueueHandle_t s_ai_action_result_queue;
static esp_netif_t *s_wifi_sta_netif = NULL;
static esp_netif_t *s_wifi_ap_netif = NULL;
static uint32_t s_chat_id = 0;
static uint32_t s_chat_done_id = 0;
static bool s_chat_pending = false;
static esp_err_t s_chat_result_err = ESP_OK;
static char s_chat_response[CHAT_RESPONSE_MAX];
// Cross-core status flags: use atomics for lock-free reads in UI/tasks.
static std::atomic<bool> s_wifi_connected{false};
static std::atomic<bool> s_wifi_ap_active{false};
static char s_wifi_ap_ssid[33];
static rover_settings_t s_settings;
static httpd_handle_t s_httpd = NULL;
static bool s_wifi_stack_initialized = false;
static bool s_wifi_started = false;

static int8_t s_motion_x = 0;
static int8_t s_motion_y = 0;
static int8_t s_motion_z = 0;
static bool s_motion_active = false;
static bool s_gripper_open = false;
static TickType_t s_web_motion_deadline = 0;
static std::atomic<uint32_t> s_last_activity_tick{0};
static std::atomic<uint32_t> s_ai_action_req_seq{0};

typedef struct {
  uint32_t id;
  char prompt[CHAT_PROMPT_MAX];
} chat_job_t;

typedef enum {
  AI_ACTION_MOVE = 1,
  AI_ACTION_STOP = 2,
  AI_ACTION_TURN = 3,
  AI_ACTION_GRIPPER_OPEN = 4,
  AI_ACTION_GRIPPER_CLOSE = 5,
} ai_action_kind_t;

typedef struct {
  uint32_t req_id;
  ai_action_kind_t kind;
  int8_t x;
  int8_t y;
  int8_t z;
  uint16_t duration_ms;
  uint16_t turn_target_deg;
  uint16_t turn_timeout_ms;
} ai_action_req_t;

typedef struct {
  uint32_t req_id;
  esp_err_t err;
  float turn_measured_deg;
} ai_action_result_t;

static const char *wifi_authmode_name(wifi_auth_mode_t authmode) {
  switch (authmode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK: return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa_wpa2_psk";
#if CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
    case WIFI_AUTH_WPA3_PSK: return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2_wpa3_psk";
#endif
    default: return "unknown";
  }
}

static void settings_set_defaults(rover_settings_t *settings) {
  memset(settings, 0, sizeof(*settings));
  strlcpy(settings->llm_model, kDefaultLlmModel, sizeof(settings->llm_model));
  strlcpy(settings->llm_endpoint, kDefaultLlmEndpoint, sizeof(settings->llm_endpoint));
}

static void settings_copy_if_present(char *dst, size_t dst_size, const cJSON *item) {
  if (!cJSON_IsString(item) || item->valuestring == NULL) {
    return;
  }
  if (item->valuestring[0] == '\0') {
    return;
  }
  strlcpy(dst, item->valuestring, dst_size);
}

static esp_err_t settings_load_from_nvs(rover_settings_t *settings) {
  settings_set_defaults(settings);

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kSettingsNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return ESP_OK;
  }

  size_t len = sizeof(settings->wifi_ssid);
  err = nvs_get_str(handle, "wifi_ssid", settings->wifi_ssid, &len);
  if (err != ESP_OK) settings->wifi_ssid[0] = '\0';

  len = sizeof(settings->wifi_password);
  err = nvs_get_str(handle, "wifi_password", settings->wifi_password, &len);
  if (err != ESP_OK) settings->wifi_password[0] = '\0';

  len = sizeof(settings->llm_endpoint);
  err = nvs_get_str(handle, "llm_endpoint", settings->llm_endpoint, &len);
  if (err != ESP_OK) strlcpy(settings->llm_endpoint, kDefaultLlmEndpoint, sizeof(settings->llm_endpoint));

  len = sizeof(settings->llm_api_key);
  err = nvs_get_str(handle, "llm_api_key", settings->llm_api_key, &len);
  if (err != ESP_OK) settings->llm_api_key[0] = '\0';

  len = sizeof(settings->llm_model);
  err = nvs_get_str(handle, "llm_model", settings->llm_model, &len);
  if (err != ESP_OK) strlcpy(settings->llm_model, kDefaultLlmModel, sizeof(settings->llm_model));

  nvs_close(handle);
  return ESP_OK;
}

static esp_err_t settings_save_to_nvs(const rover_settings_t *settings) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_str(handle, "wifi_ssid", settings->wifi_ssid);
  if (err == ESP_OK) err = nvs_set_str(handle, "wifi_password", settings->wifi_password);
  if (err == ESP_OK) err = nvs_set_str(handle, "llm_endpoint", settings->llm_endpoint);
  if (err == ESP_OK) err = nvs_set_str(handle, "llm_api_key", settings->llm_api_key);
  if (err == ESP_OK) err = nvs_set_str(handle, "llm_model", settings->llm_model);
  if (err == ESP_OK) err = nvs_commit(handle);

  nvs_close(handle);
  return err;
}

static esp_err_t settings_erase_nvs(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_erase_all(handle);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static esp_err_t settings_init_from_nvs(void) {
  if (s_settings_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  rover_settings_t loaded = {};
  esp_err_t err = settings_load_from_nvs(&loaded);
  if (err != ESP_OK) {
    return err;
  }
  xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
  s_settings = loaded;
  xSemaphoreGive(s_settings_mutex);
  return ESP_OK;
}

static void settings_snapshot(rover_settings_t *out) {
  xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
  *out = s_settings;
  xSemaphoreGive(s_settings_mutex);
}

static void settings_apply_snapshot(const rover_settings_t *in) {
  xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
  s_settings = *in;
  xSemaphoreGive(s_settings_mutex);
}

static bool settings_wifi_configured(const rover_settings_t *settings) {
  return settings->wifi_ssid[0] != '\0';
}

static bool settings_llm_configured(const rover_settings_t *settings) {
  return settings->llm_api_key[0] != '\0';
}

static void destroy_ai_handles(void) {
  if (s_ai != NULL) {
    openrouter_destroy(s_ai);
    s_ai = NULL;
  }
}

static void reload_ai_from_settings(void);

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data) {
  (void)arg;
  (void)event_data;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    rover_settings_t settings = {};
    settings_snapshot(&settings);
    if (settings_wifi_configured(&settings)) {
      esp_wifi_connect();
    }
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_wifi_connected.store(false, std::memory_order_relaxed);
    if (s_state_mutex != NULL) {
      xSemaphoreTake(s_state_mutex, portMAX_DELAY);
      transition_to(STATE_OFFLINE_FALLBACK);
      xSemaphoreGive(s_state_mutex);
    }
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
      if (s_wifi_event_group != NULL) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_retry_num = 0;
    if (s_wifi_event_group != NULL) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
  }
}

static void format_wifi_ap_ssid(char *buf, size_t len) {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
    snprintf(buf, len, "AI-Rover-%02X%02X", mac[4], mac[5]);
  } else {
    strlcpy(buf, "AI-Rover-AP", len);
  }
}

static esp_err_t ensure_wifi_stack_initialized(void) {
  if (s_wifi_stack_initialized) {
    return ESP_OK;
  }

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  if (s_wifi_sta_netif == NULL) {
    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
    return err;
  }

  s_wifi_stack_initialized = true;
  return ESP_OK;
}

static esp_err_t ensure_wifi_started(void) {
  if (s_wifi_started) {
    return ESP_OK;
  }

  esp_err_t err = esp_wifi_start();
  if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
    s_wifi_started = true;
    return ESP_OK;
  }
  return err;
}

static esp_err_t start_wifi_ap_fallback(void) {
  if (s_wifi_ap_active.load(std::memory_order_relaxed)) {
    return ESP_OK;
  }

  esp_err_t err = ensure_wifi_stack_initialized();
  if (err != ESP_OK) return err;

  if (s_wifi_ap_netif == NULL) {
    s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_wifi_ap_netif == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  format_wifi_ap_ssid(s_wifi_ap_ssid, sizeof(s_wifi_ap_ssid));

  wifi_config_t ap_config = {};
  strlcpy((char *)ap_config.ap.ssid, s_wifi_ap_ssid, sizeof(ap_config.ap.ssid));
  ap_config.ap.ssid_len = strlen(s_wifi_ap_ssid);
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  ap_config.ap.max_connection = kWifiApMaxClients;
  ap_config.ap.channel = 6;

  wifi_mode_t mode = WIFI_MODE_APSTA;
  err = esp_wifi_set_mode(mode);
  if (err != ESP_OK) return err;
  err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  if (err != ESP_OK) return err;
  err = ensure_wifi_started();
  if (err != ESP_OK) return err;

  s_wifi_ap_active.store(true, std::memory_order_relaxed);
  rover_log_field_t fields[] = {
    rover_log_field_str("ssid", s_wifi_ap_ssid),
    rover_log_field_str("mode", "apsta"),
    rover_log_field_int("max_clients", kWifiApMaxClients),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_WARN,
    .component = TAG,
    .event = "wifi_ap_started",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);
  return ESP_OK;
}

static void stop_wifi_ap_fallback(void) {
  if (!s_wifi_ap_active.load(std::memory_order_relaxed)) {
    return;
  }

  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK) {
    s_wifi_ap_active.store(false, std::memory_order_relaxed);
    rover_log_record_t rec = {
      .level = ESP_LOG_INFO,
      .component = TAG,
      .event = "wifi_ap_stopped",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
  } else {
    rover_log_field_t fields[] = {
      rover_log_field_str("err", esp_err_to_name(err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "wifi_ap_stop_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
  }
}

static esp_err_t wifi_connect_blocking(void) {
  esp_err_t err = ensure_wifi_stack_initialized();
  if (err != ESP_OK) {
    return err;
  }

  rover_settings_t settings = {};
  settings_snapshot(&settings);
  if (!settings_wifi_configured(&settings)) {
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "wifi_credentials_missing",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    return ESP_ERR_NOT_FOUND;
  }

  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    return ESP_ERR_NO_MEM;
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, settings.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, settings.wifi_password, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(s_wifi_ap_active.load(std::memory_order_relaxed) ? WIFI_MODE_APSTA : WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(ensure_wifi_started());
  (void)esp_wifi_connect();

  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, kWifiConnectTimeout);

  ESP_ERROR_CHECK(
      esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
  ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
  vEventGroupDelete(s_wifi_event_group);

  if (bits & WIFI_CONNECTED_BIT) {
    rover_log_field_t fields[] = { rover_log_field_str("ssid", settings.wifi_ssid) };
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

static void init_ai(void);

static char *make_tool_response(const char *status, const char *action) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"action\":\"%s\"}", status, action);
  return strdup(buf);
}

static bool ai_action_wait_result_obj(uint32_t req_id, TickType_t timeout, ai_action_result_t *out) {
  if (s_ai_action_result_queue == NULL) {
    if (out) {
      *out = {};
      out->req_id = req_id;
      out->err = ESP_ERR_INVALID_STATE;
    }
    return false;
  }

  TickType_t deadline = xTaskGetTickCount() + timeout;
  while (1) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0) {
      if (out) {
        *out = {};
        out->req_id = req_id;
        out->err = ESP_ERR_TIMEOUT;
      }
      return false;
    }
    TickType_t remaining = deadline - now;

    ai_action_result_t result = {};
    if (xQueueReceive(s_ai_action_result_queue, &result, remaining) != pdTRUE) {
      if (out) {
        *out = {};
        out->req_id = req_id;
        out->err = ESP_ERR_TIMEOUT;
      }
      return false;
    }
    if (result.req_id == req_id) {
      if (out) *out = result;
      return true;
    }
    if (result.req_id > req_id) {
      // Should not happen with serialized tool callbacks, but preserve the future result if it does.
      (void)xQueueSendToFront(s_ai_action_result_queue, &result, 0);
      if (out) {
        *out = {};
        out->req_id = req_id;
        out->err = ESP_FAIL;
      }
      return false;
    }
    // result.req_id < req_id: stale result from a timed-out earlier callback, drop it and continue.
  }
}

static bool ai_action_wait_result(uint32_t req_id, TickType_t timeout, esp_err_t *err_out) {
  ai_action_result_t result = {};
  bool ok = ai_action_wait_result_obj(req_id, timeout, &result);
  if (err_out) *err_out = result.err;
  return ok;
}

static void ai_action_send_result_obj(const ai_action_result_t *src) {
  if (s_ai_action_result_queue == NULL || src == NULL) return;

  ai_action_result_t result = *src;
  if (xQueueSend(s_ai_action_result_queue, &result, 0) == pdTRUE) {
    return;
  }

  ai_action_result_t dropped = {};
  (void)xQueueReceive(s_ai_action_result_queue, &dropped, 0);
  (void)xQueueSend(s_ai_action_result_queue, &result, 0);
}

static void ai_action_send_result(uint32_t req_id, esp_err_t err) {
  ai_action_result_t result = {
    .req_id = req_id,
    .err = err,
    .turn_measured_deg = 0.0f,
  };
  ai_action_send_result_obj(&result);
}

static void ai_action_apply_stop_state(void) {
  rover_emergency_stop();
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  set_motion(0, 0, 0, false);
  s_web_motion_deadline = 0;
  xSemaphoreGive(s_state_mutex);
}

static bool ai_action_drain_and_process_stop(void) {
  if (s_ai_action_queue == NULL) return false;
  if (s_ai_action_queue_mutex == NULL) return false;

  ai_action_req_t pending[kAiActionQueueDepth];
  int pending_count = 0;
  bool stop_processed = false;

  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  ai_action_req_t queued = {};
  while (xQueueReceive(s_ai_action_queue, &queued, 0) == pdTRUE) {
    if (queued.kind == AI_ACTION_STOP) {
      ai_action_apply_stop_state();
      ai_action_send_result(queued.req_id, ESP_OK);
      stop_processed = true;
      continue;
    }
    if (pending_count < kAiActionQueueDepth) {
      pending[pending_count++] = queued;
    }
  }

  for (int i = 0; i < pending_count; i++) {
    (void)xQueueSend(s_ai_action_queue, &pending[i], 0);
  }
  xSemaphoreGive(s_ai_action_queue_mutex);
  return stop_processed;
}

static esp_err_t ai_action_execute_on_core0(const ai_action_req_t *req, ai_action_result_t *result_out) {
  if (req == NULL) return ESP_ERR_INVALID_ARG;
  if (result_out) {
    *result_out = {};
    result_out->req_id = req->req_id;
    result_out->err = ESP_OK;
  }

  rover_state_t prev_state = STATE_IDLE;
  bool restore_ai_state = false;
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  prev_state = s_rover_state;
  if (s_rover_state == STATE_AI_THINKING || s_rover_state == STATE_AI_EXECUTING) {
    transition_to(STATE_AI_EXECUTING);
    restore_ai_state = true;
  }
  xSemaphoreGive(s_state_mutex);

  esp_err_t action_err = ESP_OK;
  if (req->kind == AI_ACTION_MOVE) {
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(req->duration_ms);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    set_motion(req->x, req->y, req->z, (req->x != 0 || req->y != 0 || req->z != 0));
    xSemaphoreGive(s_state_mutex);

    while ((int32_t)(end - xTaskGetTickCount()) > 0) {
      esp_task_wdt_reset();
      M5.update();
      if (M5.BtnB.isPressed()) {
        action_err = ESP_ERR_INVALID_STATE;
        break;
      }
      if (ai_action_drain_and_process_stop()) {
        action_err = ESP_ERR_INVALID_STATE;
        break;
      }
      esp_err_t err = rover_set_speed(req->x, req->y, req->z);
      if (action_err == ESP_OK && err != ESP_OK) action_err = err;
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_err_t stop_err = rover_set_speed(0, 0, 0);
    if (action_err == ESP_OK && stop_err != ESP_OK) action_err = stop_err;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    set_motion(0, 0, 0, false);
    xSemaphoreGive(s_state_mutex);
  } else if (req->kind == AI_ACTION_TURN) {
    float turned = 0.0f;
    float target = (float)req->turn_target_deg;
    TickType_t start_tick = xTaskGetTickCount();
    uint32_t prev_ms = (uint32_t)(esp_log_timestamp());
    while (turned < target &&
           (xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(req->turn_timeout_ms)) {
      esp_task_wdt_reset();
      M5.update();
      if (M5.BtnB.isPressed()) {
        action_err = ESP_ERR_INVALID_STATE;
        break;
      }
      if (ai_action_drain_and_process_stop()) {
        action_err = ESP_ERR_INVALID_STATE;
        break;
      }

      float gx = 0, gy = 0, gz = 0;
      M5.Imu.getGyro(&gx, &gy, &gz);
      uint32_t now_ms = (uint32_t)(esp_log_timestamp());
      float dt_s = (float)(now_ms - prev_ms) / 1000.0f;
      prev_ms = now_ms;

      esp_err_t err = rover_set_speed(0, 0, req->z);
      if (action_err == ESP_OK && err != ESP_OK) action_err = err;

      float rate = fabsf(gx);
      if (fabsf(gy) > rate) rate = fabsf(gy);
      if (fabsf(gz) > rate) rate = fabsf(gz);
      if (rate > 3.0f) {
        turned += rate * dt_s;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_err_t stop_err = rover_set_speed(0, 0, 0);
    if (action_err == ESP_OK && stop_err != ESP_OK) action_err = stop_err;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    set_motion(0, 0, 0, false);
    xSemaphoreGive(s_state_mutex);

    if (action_err == ESP_OK && turned < target) {
      action_err = ESP_ERR_TIMEOUT;
    }
    if (result_out) {
      result_out->turn_measured_deg = turned;
    }
  } else if (req->kind == AI_ACTION_STOP) {
    ai_action_apply_stop_state();
  } else if (req->kind == AI_ACTION_GRIPPER_OPEN || req->kind == AI_ACTION_GRIPPER_CLOSE) {
    bool open = (req->kind == AI_ACTION_GRIPPER_OPEN);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_gripper_open = open;
    xSemaphoreGive(s_state_mutex);
    esp_err_t servo_err = rover_set_servo_angle(kGripperServo, open ? kGripperOpenAngle : kGripperCloseAngle);
    if (action_err == ESP_OK && servo_err != ESP_OK) action_err = servo_err;
  } else {
    action_err = ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  if (restore_ai_state) {
    transition_to(prev_state);
  }
  xSemaphoreGive(s_state_mutex);

  if (result_out) {
    result_out->err = action_err;
  }
  return action_err;
}

static void ai_action_poll_and_execute(void) {
  if (s_ai_action_queue == NULL) return;
  if (s_ai_action_queue_mutex == NULL) return;
  ai_action_req_t req = {};
  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  BaseType_t got = xQueueReceive(s_ai_action_queue, &req, 0);
  xSemaphoreGive(s_ai_action_queue_mutex);
  if (got != pdTRUE) {
    return;
  }
  ai_action_result_t result = {
    .req_id = req.req_id,
    .err = ESP_OK,
    .turn_measured_deg = 0.0f,
  };
  result.err = ai_action_execute_on_core0(&req, &result);
  ai_action_send_result_obj(&result);
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

  if (s_ai_action_queue == NULL) {
    return make_tool_response("unavailable", "move");
  }
  if (s_ai_action_queue_mutex == NULL) {
    return make_tool_response("unavailable", "move");
  }

  ai_action_req_t req = {
    .req_id = ++s_ai_action_req_seq,
    .kind = AI_ACTION_MOVE,
    .x = (int8_t)x,
    .y = (int8_t)y,
    .z = (int8_t)z,
    .duration_ms = (uint16_t)duration_ms,
    .turn_target_deg = 0,
    .turn_timeout_ms = 0,
  };
  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  BaseType_t sent = xQueueSend(s_ai_action_queue, &req, 0);
  xSemaphoreGive(s_ai_action_queue_mutex);
  if (sent != pdTRUE) {
    return make_tool_response("busy", "move");
  }
  esp_err_t action_err = ESP_OK;
  TickType_t wait_timeout = pdMS_TO_TICKS(duration_ms) + kAiActionResultTimeoutSlack;
  if (!ai_action_wait_result(req.req_id, wait_timeout, &action_err)) {
    return make_tool_response("timeout", "move");
  }
  if (action_err != ESP_OK) {
    return make_tool_response("failed", "move");
  }

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

  if (s_ai_action_queue == NULL) {
    return make_tool_response("unavailable", "turn");
  }
  if (s_ai_action_queue_mutex == NULL) {
    return make_tool_response("unavailable", "turn");
  }

  ai_action_req_t req = {
    .req_id = ++s_ai_action_req_seq,
    .kind = AI_ACTION_TURN,
    .x = 0,
    .y = 0,
    .z = turn_z,
    .duration_ms = 0,
    .turn_target_deg = (uint16_t)target,
    .turn_timeout_ms = (uint16_t)timeout_ms,
  };
  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  BaseType_t sent = xQueueSend(s_ai_action_queue, &req, 0);
  xSemaphoreGive(s_ai_action_queue_mutex);
  if (sent != pdTRUE) {
    return make_tool_response("busy", "turn");
  }

  ai_action_result_t result = {};
  TickType_t wait_timeout = pdMS_TO_TICKS(timeout_ms) + kAiActionResultTimeoutSlack;
  if (!ai_action_wait_result_obj(req.req_id, wait_timeout, &result)) {
    result.err = ESP_ERR_TIMEOUT;
  }

  const char *status = "failed";
  if (result.err == ESP_OK) {
    status = "ok";
  } else if (result.err == ESP_ERR_TIMEOUT) {
    status = "timeout";
  }

  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"status\":\"%s\",\"action\":\"turn\",\"target_deg\":%.1f,\"measured_deg\":%.1f}",
           status, (double)target, (double)result.turn_measured_deg);
  return strdup(payload); // openrouter_client frees this
}

static char *cb_stop(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)arguments; (void)ud;
  mark_activity();
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "tool_stop",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);

  if (s_ai_action_queue == NULL) {
    return make_tool_response("unavailable", "stop");
  }
  if (s_ai_action_queue_mutex == NULL) {
    return make_tool_response("unavailable", "stop");
  }

  ai_action_req_t req = {
    .req_id = ++s_ai_action_req_seq,
    .kind = AI_ACTION_STOP,
    .x = 0,
    .y = 0,
    .z = 0,
    .duration_ms = 0,
    .turn_target_deg = 0,
    .turn_timeout_ms = 0,
  };
  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  BaseType_t sent = xQueueSend(s_ai_action_queue, &req, 0);
  xSemaphoreGive(s_ai_action_queue_mutex);
  if (sent != pdTRUE) {
    return make_tool_response("busy", "stop");
  }
  esp_err_t action_err = ESP_OK;
  if (!ai_action_wait_result(req.req_id, kAiStopActionTimeout, &action_err)) {
    return make_tool_response("timeout", "stop");
  }
  if (action_err != ESP_OK) {
    return make_tool_response("failed", "stop");
  }

  return make_tool_response("ok", "stop"); // openrouter_client frees this
}

static char *cb_gripper_open(const char *fn, const char *arguments, void *ud) {
  (void)fn; (void)arguments; (void)ud;
  mark_activity();
  if (s_ai_action_queue == NULL) {
    return make_tool_response("unavailable", "gripper_open");
  }
  if (s_ai_action_queue_mutex == NULL) {
    return make_tool_response("unavailable", "gripper_open");
  }
  ai_action_req_t req = {
    .req_id = ++s_ai_action_req_seq,
    .kind = AI_ACTION_GRIPPER_OPEN,
    .x = 0,
    .y = 0,
    .z = 0,
    .duration_ms = 0,
    .turn_target_deg = 0,
    .turn_timeout_ms = 0,
  };
  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  BaseType_t sent = xQueueSend(s_ai_action_queue, &req, 0);
  xSemaphoreGive(s_ai_action_queue_mutex);
  if (sent != pdTRUE) {
    return make_tool_response("busy", "gripper_open");
  }
  esp_err_t action_err = ESP_OK;
  if (!ai_action_wait_result(req.req_id, kAiStopActionTimeout, &action_err)) {
    return make_tool_response("timeout", "gripper_open");
  }
  if (action_err != ESP_OK) {
    return make_tool_response("failed", "gripper_open");
  }
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
  if (s_ai_action_queue == NULL) {
    return make_tool_response("unavailable", "gripper_close");
  }
  if (s_ai_action_queue_mutex == NULL) {
    return make_tool_response("unavailable", "gripper_close");
  }
  ai_action_req_t req = {
    .req_id = ++s_ai_action_req_seq,
    .kind = AI_ACTION_GRIPPER_CLOSE,
    .x = 0,
    .y = 0,
    .z = 0,
    .duration_ms = 0,
    .turn_target_deg = 0,
    .turn_timeout_ms = 0,
  };
  xSemaphoreTake(s_ai_action_queue_mutex, portMAX_DELAY);
  BaseType_t sent = xQueueSend(s_ai_action_queue, &req, 0);
  xSemaphoreGive(s_ai_action_queue_mutex);
  if (sent != pdTRUE) {
    return make_tool_response("busy", "gripper_close");
  }
  esp_err_t action_err = ESP_OK;
  if (!ai_action_wait_result(req.req_id, kAiStopActionTimeout, &action_err)) {
    return make_tool_response("timeout", "gripper_close");
  }
  if (action_err != ESP_OK) {
    return make_tool_response("failed", "gripper_close");
  }
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
      err = openrouter_call_with_tools(
          s_ai, job.prompt, response, sizeof(response), kAiToolCallMaxIterations);
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
      "input,select{width:100%;background:#0f172a;color:#e5e7eb;border:1px solid #334155;"
      "border-radius:8px;padding:10px;font-size:14px}"
      "pre{white-space:pre-wrap;word-break:break-word;background:#0f172a;border:1px solid #334155;"
      "border-radius:8px;padding:10px;font-size:13px}"
      ".muted{opacity:.7;font-size:12px}"
      ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}"
      ".field{display:flex;flex-direction:column;gap:4px;min-width:0}"
      ".full{grid-column:1 / -1}"
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
      /* Chat */
      "<div class='card' id='chatCard' style='display:none'><h2>Chat</h2>"
      "<textarea id='msg' placeholder='Message for rover AI...'></textarea>"
      "<div class='row' style='margin-top:8px'>"
      "<button onclick='ask()'>Send</button>"
      "<button onclick='poll()'>Poll</button>"
      "</div>"
      "<div class='muted' id='chatInfo' style='margin-top:6px'>idle</div>"
      "<pre id='chatOut'></pre>"
      "</div>"
      /* Settings */
      "<div class='card'><h2>Settings</h2>"
      "<div class='grid'>"
      "<label class='field full'>Wi-Fi network<select id='wifiSsid'></select></label>"
      "<label class='field full'>Wi-Fi password<input type='password' id='wifiPassword' placeholder='Leave blank to keep current'></label>"
      "<label class='field full'>LLM endpoint<input type='text' id='llmEndpoint' placeholder='https://.../chat/completions'></label>"
      "<label class='field'>LLM API key<input type='password' id='llmApiKey' placeholder='Leave blank to keep current'></label>"
      "<label class='field'>LLM model<input type='text' id='llmModel' placeholder='openai/gpt-4o-mini'></label>"
      "</div>"
      "<div class='row' style='margin-top:8px'>"
      "<button onclick='scanNetworks()'>Rescan</button>"
      "<button onclick='saveSettings()'>Save</button>"
      "<button class='danger' onclick='resetSettings()'>Reset</button>"
      "</div>"
      "<div class='muted' id='settingsInfo' style='margin-top:6px'>loading...</div>"
      "</div>"
      /* Script */
      "<script>"
      "const C=document.getElementById('joy'),ctx=C.getContext('2d');"
      "const R=90,DR=30;"
      "let jx=0,jy=0,jDown=false,jTimer=0;"
      "let holdAct='',holdT=0,lastId=0,selectedWifi='';"
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
      "function setSettingsInfo(t){document.getElementById('settingsInfo').textContent=t;}"
      "function setChatVisible(v){document.getElementById('chatCard').style.display=v?'block':'none';}"
      "function addWifiOption(sel,ssid,label){const o=document.createElement('option');o.value=ssid;o.textContent=label||ssid;sel.appendChild(o);}"
      "function populateWifiList(networks,current){const sel=document.getElementById('wifiSsid');const prev=selectedWifi||current||sel.value||'';sel.innerHTML='';"
      "if(prev && !networks.some(n=>n.ssid===prev)){addWifiOption(sel,prev,prev+' (saved)');}"
      "if(networks.length===0){addWifiOption(sel,'','No networks found');}"
      "for(const n of networks){addWifiOption(sel,n.ssid,n.ssid+' ('+n.rssi+' dBm, '+n.auth+')');}"
      "if(prev)sel.value=prev;selectedWifi=sel.value||prev;}"
      "async function loadSettings(){try{const r=await fetch('/settings');const j=await r.json();"
      "if(!j.ok)throw new Error('bad');"
      "selectedWifi=j.wifi_ssid||selectedWifi||'';"
      "document.getElementById('llmEndpoint').value=j.llm_endpoint||'';"
      "document.getElementById('llmModel').value=j.llm_model||'';"
      "document.getElementById('wifiPassword').value='';"
      "document.getElementById('llmApiKey').value='';"
      "setChatVisible(!!j.llm_api_key_set);"
      "setSettingsInfo(j.wifi_connected?'Wi-Fi connected':(j.wifi_ap_active?'AP active: '+(j.ap_ssid||''):'Wi-Fi offline'));"
      "if(document.getElementById('wifiSsid').options.length>0){document.getElementById('wifiSsid').value=selectedWifi;}}catch(e){setSettingsInfo('settings load failed');}}"
      "async function scanNetworks(){setSettingsInfo('scanning Wi-Fi...');try{const r=await fetch('/wifi_scan');const t=await r.text();let j;try{j=JSON.parse(t);}catch(_){throw new Error(t.slice(0,80)||('HTTP '+r.status));}if(!j.ok)throw new Error(j.err||j.error||'bad');populateWifiList(j.networks||[],selectedWifi);setSettingsInfo('found '+(j.networks||[]).length+' networks');}catch(e){setSettingsInfo('scan failed: '+e.message);}}"
      "async function saveSettings(){const payload={wifi_ssid:document.getElementById('wifiSsid').value||'',wifi_password:document.getElementById('wifiPassword').value||'',llm_endpoint:document.getElementById('llmEndpoint').value||'',llm_api_key:document.getElementById('llmApiKey').value||'',llm_model:document.getElementById('llmModel').value||''};"
      "setSettingsInfo('saving...');"
      "const r=await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const t=await r.text();"
      "try{const j=JSON.parse(t);if(j.reboot){setSettingsInfo('saved, rebooting...');return;}setSettingsInfo('saved');await loadSettings();}catch(e){setSettingsInfo(t);}}"
      "async function resetSettings(){if(!confirm('Reset Wi-Fi and LLM settings?'))return;setSettingsInfo('resetting...');const r=await fetch('/settings/reset',{method:'POST'});const t=await r.text();try{const j=JSON.parse(t);if(j.reboot){setSettingsInfo('reset, rebooting...');return;}}catch(e){}setSettingsInfo(t);}"
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
      "document.getElementById('wifiSsid').onchange=function(){selectedWifi=this.value;};"
      "Promise.all([refresh(),loadSettings(),scanNetworks()]).finally(()=>setInterval(refresh,1500));"
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
                   "\"gripper\":\"%s\","
                   "\"bat_pct\":%d,\"vbus_mv\":%d}",
                   state_name(s_rover_state),
                   s_motion_active ? 1 : 0,
                   s_motion_x,
                   s_motion_y,
                   s_motion_z,
                   s_gripper_open ? "open" : "close",
                   (int)bat_pct,
                   (int)vbus_mv);
  xSemaphoreGive(s_state_mutex);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, n);
}

static bool read_http_body(httpd_req_t *req, char *buf, size_t buf_size) {
  if (buf_size == 0 || req->content_len >= buf_size) {
    return false;
  }
  size_t total = 0;
  while (total < req->content_len) {
    int r = httpd_req_recv(req, buf + total, req->content_len - total);
    if (r <= 0) {
      return false;
    }
    total += (size_t)r;
  }
  buf[total] = '\0';
  return true;
}

static void settings_to_json(cJSON *root, const rover_settings_t *settings) {
  cJSON_AddStringToObject(root, "wifi_ssid", settings->wifi_ssid);
  cJSON_AddBoolToObject(root, "wifi_password_set", settings->wifi_password[0] != '\0');
  cJSON_AddStringToObject(root, "llm_endpoint", settings->llm_endpoint);
  cJSON_AddBoolToObject(root, "llm_api_key_set", settings->llm_api_key[0] != '\0');
  cJSON_AddStringToObject(root, "llm_model", settings->llm_model);
  cJSON_AddBoolToObject(root, "wifi_connected", s_wifi_connected.load(std::memory_order_relaxed));
  cJSON_AddBoolToObject(root, "wifi_ap_active", s_wifi_ap_active.load(std::memory_order_relaxed));
  cJSON_AddStringToObject(root, "ap_ssid", s_wifi_ap_ssid);
}

static esp_err_t handle_settings_get(httpd_req_t *req) {
  rover_settings_t settings = {};
  settings_snapshot(&settings);

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
  }
  cJSON_AddBoolToObject(root, "ok", true);
  settings_to_json(root, &settings);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_set_type(req, "application/json");
  esp_err_t send_err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  free(json);
  return send_err;
}

static esp_err_t handle_settings_post(httpd_req_t *req) {
  char body[1024];
  if (!read_http_body(req, body, sizeof(body))) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
  }

  cJSON *root = cJSON_Parse(body);
  if (root == NULL) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid json\"}", HTTPD_RESP_USE_STRLEN);
  }

  rover_settings_t current = {};
  settings_snapshot(&current);
  rover_settings_t updated = current;

  settings_copy_if_present(updated.wifi_ssid, sizeof(updated.wifi_ssid),
                           cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid"));
  settings_copy_if_present(updated.wifi_password, sizeof(updated.wifi_password),
                           cJSON_GetObjectItemCaseSensitive(root, "wifi_password"));
  settings_copy_if_present(updated.llm_endpoint, sizeof(updated.llm_endpoint),
                           cJSON_GetObjectItemCaseSensitive(root, "llm_endpoint"));
  settings_copy_if_present(updated.llm_api_key, sizeof(updated.llm_api_key),
                           cJSON_GetObjectItemCaseSensitive(root, "llm_api_key"));
  settings_copy_if_present(updated.llm_model, sizeof(updated.llm_model),
                           cJSON_GetObjectItemCaseSensitive(root, "llm_model"));
  cJSON_Delete(root);

  esp_err_t save_err = settings_save_to_nvs(&updated);
  if (save_err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("err", esp_err_to_name(save_err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "settings_save_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
  }

  settings_apply_snapshot(&updated);

  bool wifi_changed = strcmp(current.wifi_ssid, updated.wifi_ssid) != 0 ||
                      strcmp(current.wifi_password, updated.wifi_password) != 0;
  bool llm_changed = strcmp(current.llm_endpoint, updated.llm_endpoint) != 0 ||
                     strcmp(current.llm_api_key, updated.llm_api_key) != 0 ||
                     strcmp(current.llm_model, updated.llm_model) != 0;

  if (llm_changed && !wifi_changed) {
    reload_ai_from_settings();
  }

  rover_log_field_t fields[] = {
    rover_log_field_bool("wifi_changed", wifi_changed),
    rover_log_field_bool("llm_changed", llm_changed),
  };
  rover_log_record_t rec = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "settings_saved",
    .fields = fields,
    .field_count = sizeof(fields) / sizeof(fields[0]),
  };
  rover_log(&rec);

  httpd_resp_set_type(req, "application/json");
  if (wifi_changed) {
    esp_err_t send_err = httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return send_err;
  }

  return httpd_resp_send(req, "{\"ok\":true,\"reboot\":false}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_settings_reset(httpd_req_t *req) {
  rover_settings_t defaults = {};
  settings_set_defaults(&defaults);
  esp_err_t erase_err = settings_erase_nvs();
  if (erase_err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"reset failed\"}", HTTPD_RESP_USE_STRLEN);
  }

  settings_apply_snapshot(&defaults);
  reload_ai_from_settings();

  rover_log_record_t rec = {
    .level = ESP_LOG_WARN,
    .component = TAG,
    .event = "settings_reset",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec);

  httpd_resp_set_type(req, "application/json");
  esp_err_t send_err = httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
  return send_err;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req) {
  esp_err_t wifi_ready_err = ensure_wifi_stack_initialized();
  if (wifi_ready_err != ESP_OK) {
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"wifi not ready\",\"err\":\"%s\"}",
             esp_err_to_name(wifi_ready_err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  }

  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t mode_err = esp_wifi_get_mode(&mode);
  if (mode_err == ESP_OK && mode == WIFI_MODE_AP) {
    mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  }
  if (mode_err != ESP_OK) {
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"scan mode failed\",\"err\":\"%s\"}",
             esp_err_to_name(mode_err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  }

  esp_err_t start_err = ensure_wifi_started();
  if (start_err != ESP_OK) {
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"wifi start failed\",\"err\":\"%s\"}",
             esp_err_to_name(start_err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  }

  wifi_scan_config_t scan_cfg = {};
  scan_cfg.ssid = NULL;
  scan_cfg.bssid = NULL;
  scan_cfg.channel = 0;
  scan_cfg.show_hidden = true;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.scan_time.active.min = 40;
  scan_cfg.scan_time.active.max = 120;

  esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
  if (err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("err", esp_err_to_name(err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "wifi_scan_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"scan failed\",\"err\":\"%s\"}",
             esp_err_to_name(err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  }

  uint16_t ap_count = 0;
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
  wifi_ap_record_t *records = NULL;
  if (ap_count > 0) {
    records = (wifi_ap_record_t *)calloc(ap_count, sizeof(wifi_ap_record_t));
    if (records == NULL) {
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }
    uint16_t count = ap_count;
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
      free(records);
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_send(req, "{\"ok\":false,\"error\":\"scan read failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    ap_count = count;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *networks = cJSON_CreateArray();
  if (root == NULL || networks == NULL) {
    if (root) cJSON_Delete(root);
    free(records);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
  }

  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddItemToObject(root, "networks", networks);

  for (uint16_t i = 0; i < ap_count; i++) {
    wifi_ap_record_t *best = &records[i];
    if (best->ssid[0] == '\0') {
      continue;
    }

    for (uint16_t j = (uint16_t)(i + 1); j < ap_count; j++) {
      wifi_ap_record_t *candidate = &records[j];
      if (candidate->ssid[0] == '\0') {
        continue;
      }
      if (strcmp((const char *)best->ssid, (const char *)candidate->ssid) == 0) {
        if (candidate->rssi > best->rssi) {
          *best = *candidate;
        }
        candidate->ssid[0] = '\0';
      }
    }

    const wifi_ap_record_t *r = &records[i];
    if (r->ssid[0] == '\0') {
      continue;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) continue;
    cJSON_AddStringToObject(item, "ssid", (const char *)r->ssid);
    cJSON_AddNumberToObject(item, "rssi", r->rssi);
    cJSON_AddNumberToObject(item, "channel", r->primary);
    cJSON_AddStringToObject(item, "auth", wifi_authmode_name(r->authmode));
    cJSON_AddItemToArray(networks, item);
  }

  free(records);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_set_type(req, "application/json");
  esp_err_t send_err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  free(json);
  return send_err;
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
  if (s_ai == NULL && s_wifi_connected.load(std::memory_order_relaxed)) {
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "ai_lazy_reinit_attempt",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    if (s_ai_mutex != NULL) xSemaphoreTake(s_ai_mutex, portMAX_DELAY);
    if (s_ai == NULL) {
      init_ai();
    }
    if (s_ai_mutex != NULL) xSemaphoreGive(s_ai_mutex);
  }
  if (s_ai == NULL || s_chat_queue == NULL) {
    rover_log_record_t rec = {
      .level = ESP_LOG_ERROR,
      .component = TAG,
      .event = "chat_ai_unavailable",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
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
  config.max_uri_handlers = 12;
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
  httpd_uri_t settings_get = {
      .uri = "/settings", .method = HTTP_GET, .handler = handle_settings_get, .user_ctx = NULL};
  httpd_uri_t settings_post = {
      .uri = "/settings", .method = HTTP_POST, .handler = handle_settings_post, .user_ctx = NULL};
  httpd_uri_t settings_reset = {
      .uri = "/settings/reset", .method = HTTP_POST, .handler = handle_settings_reset, .user_ctx = NULL};
  httpd_uri_t wifi_scan = {.uri = "/wifi_scan", .method = HTTP_GET, .handler = handle_wifi_scan, .user_ctx = NULL};

  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cmd));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat_post));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &chat_result));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_get));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_post));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_reset));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan));
}

static void init_ai(void) {
  rover_settings_t settings = {};
  settings_snapshot(&settings);
  if (!settings_llm_configured(&settings)) {
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "ai_config_missing",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    return;
  }

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
  };

  openrouter_config_t cfg = {};
  cfg.api_key = settings.llm_api_key;
  cfg.api_base_url = settings.llm_endpoint[0] ? settings.llm_endpoint : NULL;
  cfg.enable_streaming = false;
  cfg.enable_tools = true;
  cfg.http_timeout_ms = kAiHttpTimeoutMs;
  cfg.max_tokens = 256;
  cfg.default_model = settings.llm_model[0] ? settings.llm_model : kDefaultLlmModel;
  cfg.default_system_role =
      "You control a mecanum rover with gripper. "
      "Use tools directly when user gives a command; do not ask permission first. "
      "If you say you will scan/look/check, call the tool in the same response. "
      "Use move() for timed movement, turn() for angle rotation (IMU), stop() for immediate stop. "
      "Respond briefly in the user's language.";
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

static void reload_ai_from_settings(void) {
  if (s_ai_mutex != NULL) xSemaphoreTake(s_ai_mutex, portMAX_DELAY);
  destroy_ai_handles();
  if (settings_llm_configured(&s_settings)) {
    init_ai();
  }
  if (s_ai_mutex != NULL) xSemaphoreGive(s_ai_mutex);
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
  esp_netif_t *netif = s_wifi_sta_netif != NULL ? s_wifi_sta_netif
                                                : esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info;
  if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
  } else if (s_wifi_ap_active.load(std::memory_order_relaxed) &&
             s_wifi_ap_netif != NULL &&
             esp_netif_get_ip_info(s_wifi_ap_netif, &ip_info) == ESP_OK &&
             ip_info.ip.addr != 0) {
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
  static bool prev_wifi_connected = false;
  static bool prev_wifi_ap_active = false;
  static rover_state_t prev_state = STATE_IDLE;
  static int32_t prev_bat_pct = -1;

  int32_t bat_pct = -1;
  read_power_metrics(NULL, &bat_pct);
  bool wifi_connected = s_wifi_connected.load(std::memory_order_relaxed);
  bool wifi_ap_active = s_wifi_ap_active.load(std::memory_order_relaxed);
  rover_state_t state = STATE_IDLE;
  int8_t motion_x = 0;
  int8_t motion_y = 0;
  int8_t motion_z = 0;
  bool motion_active = false;
  bool gripper_open = false;
  xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  state = s_rover_state;
  motion_x = s_motion_x;
  motion_y = s_motion_y;
  motion_z = s_motion_z;
  motion_active = s_motion_active;
  gripper_open = s_gripper_open;
  xSemaphoreGive(s_state_mutex);

  if (initialized &&
      prev_state == state &&
      prev_motion_x == motion_x &&
      prev_motion_y == motion_y &&
      prev_motion_z == motion_z &&
      prev_motion_active == motion_active &&
      prev_gripper_open == gripper_open &&
      prev_btn_a == btn_a &&
      prev_btn_b == btn_b &&
      prev_wifi_connected == wifi_connected &&
      prev_wifi_ap_active == wifi_ap_active &&
      prev_bat_pct == bat_pct) {
    return;
  }

  prev_state = state;
  prev_motion_x = motion_x;
  prev_motion_y = motion_y;
  prev_motion_z = motion_z;
  prev_motion_active = motion_active;
  prev_gripper_open = gripper_open;
  prev_btn_a = btn_a;
  prev_btn_b = btn_b;
  prev_wifi_connected = wifi_connected;
  prev_wifi_ap_active = wifi_ap_active;
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
  uint32_t sc = state_color(state);
  M5.Display.fillRoundRect(2, 2, 236, 22, 4, sc);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, sc);
  const char *sname = state_name(state);
  int snw = (int)strlen(sname) * 12;
  M5.Display.setCursor((240 - snw) / 2, 5);
  M5.Display.print(sname);

  // ── Row 1: IP address ──
  char ip_str[20];
  get_ip_str(ip_str, sizeof(ip_str));
  M5.Display.fillRoundRect(2, 34, 236, 22, 4, 0x1F2937u);
  const char *net_label = ip_str;
  int net_label_len = (int)strlen(net_label);
  bool net_label_small = net_label_len * 12 > 232;
  M5.Display.setTextSize(net_label_small ? 1 : 2);
  M5.Display.setTextColor(wifi_connected ? 0x60A5FAu : (wifi_ap_active ? 0xFBBF24u : 0x6B7280u),
                          0x1F2937u);
  int ipw = net_label_len * (net_label_small ? 6 : 12);
  M5.Display.setCursor((240 - ipw) / 2, 37);
  M5.Display.print(net_label);

  // ── Row 2: Motion ──
  if (motion_active) {
    const char *ml = motion_label(motion_x, motion_y, motion_z);
    char motion_str[48];
    snprintf(motion_str, sizeof(motion_str), "%s  x:%d y:%d z:%d",
             ml, motion_x, motion_y, motion_z);
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

  uint32_t gc = gripper_open ? 0x10B981u : 0xEF4444u;
  M5.Display.fillRoundRect(4, py, 74, 18, 4, gc);
  M5.Display.setTextColor(TFT_WHITE, gc);
  const char *gl = gripper_open ? "GRIP OPEN" : "GRIP SHUT";
  int glw = (int)strlen(gl) * 6;
  M5.Display.setCursor(4 + (74 - glw) / 2, py + 5);
  M5.Display.print(gl);

  uint32_t wc = wifi_connected ? 0x1E40AFu : (wifi_ap_active ? 0xB45309u : 0x7F1D1Du);
  M5.Display.fillRoundRect(82, py, 74, 18, 4, wc);
  M5.Display.setTextColor(TFT_WHITE, wc);
  const char *wl = wifi_connected ? "WiFi OK" : (wifi_ap_active ? "AP MODE" : "OFFLINE");
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
    if (s_wifi_connected.load(std::memory_order_relaxed)) continue;
    rover_settings_t settings = {};
    settings_snapshot(&settings);
    if (!settings_wifi_configured(&settings)) continue;

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
      s_wifi_connected.store(true, std::memory_order_relaxed);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      stop_wifi_ap_fallback();

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
    ai_action_poll_and_execute();
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
  s_settings_mutex = xSemaphoreCreateMutex();
  s_chat_mutex = xSemaphoreCreateMutex();
  s_ai_action_queue_mutex = xSemaphoreCreateMutex();
  s_chat_queue = xQueueCreate(1, sizeof(chat_job_t));
  s_syslog_queue = xQueueCreate(8, kSyslogMsgMax);
  s_ai_action_queue = xQueueCreate(kAiActionQueueDepth, sizeof(ai_action_req_t));
  s_ai_action_result_queue = xQueueCreate(kAiActionQueueDepth, sizeof(ai_action_result_t));
  if (s_state_mutex == NULL || s_i2c_mutex == NULL || s_power_mutex == NULL ||
      s_ai_mutex == NULL || s_settings_mutex == NULL || s_chat_mutex == NULL ||
      s_ai_action_queue_mutex == NULL ||
      s_chat_queue == NULL || s_syslog_queue == NULL ||
      s_ai_action_queue == NULL || s_ai_action_result_queue == NULL) {
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

  settings_set_defaults(&s_settings);
  esp_err_t settings_err = settings_init_from_nvs();
  if (settings_err != ESP_OK) {
    rover_log_field_t fields[] = {
      rover_log_field_str("err", esp_err_to_name(settings_err)),
    };
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "settings_load_failed",
      .fields = fields,
      .field_count = sizeof(fields) / sizeof(fields[0]),
    };
    rover_log(&rec);
  }

  // Unified logger: mirror JSON UART logs to syslog queue.
  rover_log_set_sink(rover_log_syslog_sink, NULL);

  rover_log_record_t rec_pre_m5 = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "boot_before_m5_begin",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec_pre_m5);

  auto m5cfg = M5.config();
  M5.begin(m5cfg);

  rover_log_record_t rec_post_m5 = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "boot_after_m5_begin",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec_post_m5);

  M5.Display.setRotation(1);

  rover_log_record_t rec_pre_boot_screen = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "boot_before_draw_status",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec_pre_boot_screen);

  draw_boot_status("booting...", "");

  rover_log_record_t rec_post_boot_screen = {
    .level = ESP_LOG_INFO,
    .component = TAG,
    .event = "boot_after_draw_status",
    .fields = NULL,
    .field_count = 0,
  };
  rover_log(&rec_post_boot_screen);

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

  rover_settings_t boot_settings = {};
  settings_snapshot(&boot_settings);
  draw_boot_status("connecting WiFi...",
                   boot_settings.wifi_ssid[0] ? boot_settings.wifi_ssid : "Wi-Fi not set");
  esp_err_t wifi_err = wifi_connect_blocking();

  if (wifi_err == ESP_OK) {
    s_wifi_connected.store(true, std::memory_order_relaxed);
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
    draw_boot_status("ready", "web + chat online");
  } else {
    // Offline fallback — no restart, buttons still work
    s_wifi_connected.store(false, std::memory_order_relaxed);
    esp_err_t ap_err = start_wifi_ap_fallback();
    if (ap_err != ESP_OK) {
      rover_log_field_t fields[] = {
        rover_log_field_str("err", esp_err_to_name(ap_err)),
      };
      rover_log_record_t ap_rec = {
        .level = ESP_LOG_ERROR,
        .component = TAG,
        .event = "wifi_ap_start_failed",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(fields[0]),
      };
      rover_log(&ap_rec);
    }
    rover_log_record_t rec = {
      .level = ESP_LOG_WARN,
      .component = TAG,
      .event = "wifi_offline_fallback",
      .fields = NULL,
      .field_count = 0,
    };
    rover_log(&rec);
    draw_boot_status(ap_err == ESP_OK ? "AP MODE" : "OFFLINE",
                     ap_err == ESP_OK ? s_wifi_ap_ssid : "buttons only");

    s_gripper_open = true;
    (void)rover_set_servo_angle(kGripperServo, kGripperOpenAngle);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    transition_to(STATE_OFFLINE_FALLBACK);
    xSemaphoreGive(s_state_mutex);

    if (ap_err == ESP_OK && s_httpd == NULL) {
      start_web_server();
    }
  }

  mark_activity();

  // Syslog queue task — Core 1, low priority
  xTaskCreatePinnedToCore(syslog_task, "syslog", 4096, NULL, 2, NULL, 1);

  // Chat worker — Core 1 (agent core, long HTTP calls)
  xTaskCreatePinnedToCore(chat_worker_task, "chat_worker", 16384, NULL, 4, NULL, 1);

  // WiFi reconnect task — Core 1, low priority
  xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_reconn", 4096, NULL, 2, NULL, 1);

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
