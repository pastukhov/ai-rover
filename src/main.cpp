#include <M5Unified.h>
#include "M5_RoverC.h"
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <WebServer.h>
#include <Syslog.h>
#include "secrets.h"
#include "openrouter.h"

namespace {

constexpr int8_t kSpeedPercent = 100;
constexpr int8_t kWebSpeedPercent = 80;
constexpr uint8_t kGripperServo = 1;
constexpr uint8_t kGripperMinAngle = 25;
constexpr uint8_t kGripperMaxAngle = 155;
constexpr uint8_t kGripperOpenAngle = 150;
constexpr uint8_t kGripperCloseAngle = 35;
constexpr uint8_t kGripperWriteRepeats = 3;
constexpr uint32_t kGripperWriteIntervalMs = 35;

constexpr uint32_t kHeartbeatMs = 1000;
constexpr uint32_t kMotionRefreshMs = 50;
constexpr uint32_t kAiControlLoopMs = 20;
constexpr uint8_t kSequenceSteps = 7;
constexpr uint8_t kDiagMotors = 4;
constexpr uint32_t kDiagRunMs = 1000;
constexpr uint32_t kDiagStopMs = 300;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kScreenSleepMs = 120000;
constexpr const char* kSyslogServer = "192.168.11.2";
constexpr uint16_t kSyslogPort = 514;

constexpr const char* kWifiSsid = WIFI_SSID;
constexpr const char* kWifiPassword = WIFI_PASSWORD;

M5_RoverC roverc;
WebServer server(80);
WiFiUDP syslogUdpClient;
Syslog syslog(syslogUdpClient, SYSLOG_PROTO_BSD);
bool rover_ready = false;
bool web_server_started = false;
bool syslog_ready = false;
openrouter_handle_t ai_handle = nullptr;

bool sequence_running = false;
bool diag_running = false;
uint8_t current_step = 0;
uint32_t step_started_at = 0;
uint32_t last_heartbeat = 0;
uint32_t last_motion_refresh = 0;

char current_action[32] = "IDLE";
char last_drawn_action[32] = "";
char wifi_status[40] = "WiFi: connecting...";
char last_drawn_wifi[40] = "";
int last_drawn_battery = -1;
int8_t current_motion_x = 0;
int8_t current_motion_y = 0;
int8_t current_motion_z = 0;
bool motion_command_active = false;
uint8_t diag_motor_index = 0;
bool diag_motor_phase_run = true;
uint32_t diag_phase_started_at = 0;
uint32_t last_activity_at = 0;

const uint32_t kStepDurationMs[kSequenceSteps] = {
    1000,  // FORWARD 50%
    500,   // STOP
    1000,  // BACKWARD 50%
    500,   // STOP
    500,   // GRIPPER OPEN
    500,   // GRIPPER CLOSE
    0      // IDLE (terminal step)
};

void logMessage(uint16_t priority, const char* message) {
  Serial.println(message);
  if (syslog_ready) {
    syslog.log(priority, message);
  }
}

void logPrintf(uint16_t priority, const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logMessage(priority, buffer);
}

void setupSyslog() {
  syslog.server(kSyslogServer, kSyslogPort);
  syslog.deviceHostname("ai-rover");
  syslog.appName("firmware");
  syslog.defaultPriority(LOG_LOCAL0);
  syslog_ready = true;
  logPrintf(LOG_INFO, "Syslog enabled: %s:%u", kSyslogServer, static_cast<unsigned>(kSyslogPort));
}

void noteActivity() {
  last_activity_at = millis();
}

void stopMotors() {
  roverc.setSpeed(0, 0, 0);
}

void setMotionCommand(int8_t x, int8_t y, int8_t z) {
  current_motion_x = x;
  current_motion_y = y;
  current_motion_z = z;
  motion_command_active = true;
  roverc.setSpeed(current_motion_x, current_motion_y, current_motion_z);
  last_motion_refresh = millis();
}

void disableMotionCommand() {
  motion_command_active = false;
}

void stopAllMotionOutputs() {
  disableMotionCommand();
  stopMotors();
  roverc.setAllPulse(0, 0, 0, 0);
}

void enterDeepSleep() {
  logMessage(LOG_INFO, "Entering deep sleep...");
  if (rover_ready) {
    stopAllMotionOutputs();
  }
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(10);

  esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);              // BtnA
  esp_sleep_enable_ext1_wakeup(1ULL << 39, ESP_EXT1_WAKEUP_ALL_LOW);  // BtnB
  esp_deep_sleep_start();
}

void checkSleepTimeout() {
  if (millis() - last_activity_at >= kScreenSleepMs) {
    enterDeepSleep();
  }
}

void refreshMotionCommand() {
  if (!rover_ready || !motion_command_active) {
    return;
  }
  const uint32_t now = millis();
  if (now - last_motion_refresh < kMotionRefreshMs) {
    return;
  }
  roverc.setSpeed(current_motion_x, current_motion_y, current_motion_z);
  last_motion_refresh = now;
}

void setAction(const char* name) {
  std::snprintf(current_action, sizeof(current_action), "%s", name);
  logPrintf(LOG_INFO, "Action: %s", current_action);
}

void setWifiStatus(const char* status) {
  std::snprintf(wifi_status, sizeof(wifi_status), "%s", status);
  logMessage(LOG_INFO, wifi_status);
}

bool recoverRover(const bool force_reinit = false) {
  if (rover_ready && !force_reinit) {
    return true;
  }
  rover_ready = roverc.begin();
  logPrintf(LOG_INFO, "Rover reinit: %s", rover_ready ? "OK" : "FAILED");
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
    return false;
  }
  return true;
}

void setGripperAngle(const uint8_t angle) {
  const uint8_t safe_angle = constrain(angle, kGripperMinAngle, kGripperMaxAngle);
  for (uint8_t i = 0; i < kGripperWriteRepeats; ++i) {
    roverc.setServoAngle(kGripperServo, safe_angle);
    delay(kGripperWriteIntervalMs);
  }
  logPrintf(LOG_INFO,
            "Gripper servo=%u angle=%u sent x%u",
            static_cast<unsigned>(kGripperServo),
            static_cast<unsigned>(safe_angle),
            static_cast<unsigned>(kGripperWriteRepeats));
}

void applyStep(uint8_t step) {
  if (!recoverRover()) {
    sequence_running = false;
    return;
  }
  switch (step) {
    case 0:
      setAction("FORWARD 50%");
      setMotionCommand(0, kSpeedPercent, 0);
      break;
    case 1:
      setAction("STOP");
      setMotionCommand(0, 0, 0);
      break;
    case 2:
      setAction("BACKWARD 50%");
      setMotionCommand(0, -kSpeedPercent, 0);
      break;
    case 3:
      setAction("STOP");
      setMotionCommand(0, 0, 0);
      break;
    case 4:
      setAction("GRIPPER OPEN");
      disableMotionCommand();
      stopMotors();
      setGripperAngle(kGripperOpenAngle);
      break;
    case 5:
      setAction("GRIPPER CLOSE");
      disableMotionCommand();
      stopMotors();
      setGripperAngle(kGripperCloseAngle);
      break;
    default:
      setAction("IDLE");
      disableMotionCommand();
      stopMotors();
      sequence_running = false;
      break;
  }
}

void emergencyStop() {
  sequence_running = false;
  diag_running = false;
  if (rover_ready) {
    stopAllMotionOutputs();
  }
  setAction("EMERGENCY STOP");
}

void commandMove(const char* action, int8_t x, int8_t y, int8_t z) {
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
    return;
  }
  sequence_running = false;
  diag_running = false;
  setAction(action);
  setMotionCommand(x, y, z);
}

void commandStop(const char* action = "STOP") {
  sequence_running = false;
  diag_running = false;
  setAction(action);
  stopAllMotionOutputs();
}

void commandGripperOpen() {
  if (!recoverRover(true)) {
    return;
  }
  sequence_running = false;
  diag_running = false;
  setAction("GRIPPER OPEN");
  stopAllMotionOutputs();
  setGripperAngle(kGripperOpenAngle);
}

void commandGripperClose() {
  if (!recoverRover(true)) {
    return;
  }
  sequence_running = false;
  diag_running = false;
  setAction("GRIPPER CLOSE");
  stopAllMotionOutputs();
  setGripperAngle(kGripperCloseAngle);
}

char* makeToolResponse(const char* status, const char* action) {
  char payload[96];
  std::snprintf(payload, sizeof(payload), "{\"status\":\"%s\",\"action\":\"%s\"}", status, action);
  return strdup(payload);
}

char* makeImuResponse(const char* status,
                      const float ax, const float ay, const float az,
                      const float gx, const float gy, const float gz) {
  char payload[224];
  std::snprintf(payload,
                sizeof(payload),
                "{\"status\":\"%s\",\"accel\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}",
                status,
                static_cast<double>(ax),
                static_cast<double>(ay),
                static_cast<double>(az),
                static_cast<double>(gx),
                static_cast<double>(gy),
                static_cast<double>(gz));
  return strdup(payload);
}

char* cb_read_imu(const char* function_name, const char* arguments, void* user_data) {
  (void)function_name;
  (void)arguments;
  (void)user_data;

  if (!M5.Imu.isEnabled()) {
    return makeToolResponse("imu_unavailable", "read_imu");
  }

  float ax = 0;
  float ay = 0;
  float az = 0;
  float gx = 0;
  float gy = 0;
  float gz = 0;
  const bool accel_ok = M5.Imu.getAccel(&ax, &ay, &az);
  const bool gyro_ok = M5.Imu.getGyro(&gx, &gy, &gz);
  if (!accel_ok || !gyro_ok) {
    return makeToolResponse("imu_read_failed", "read_imu");
  }

  return makeImuResponse("ok", ax, ay, az, gx, gy, gz);
}

char* cb_move(const char* function_name, const char* arguments, void* user_data) {
  (void)function_name;
  (void)user_data;

  if (!recoverRover(true)) {
    return makeToolResponse("error", "move");
  }

  double x = 0;
  double y = 0;
  double z = 0;
  double duration_ms = 1000;
  cJSON* args = cJSON_Parse(arguments ? arguments : "{}");
  if (args) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, "x");
    if (cJSON_IsNumber(item)) {
      x = item->valuedouble;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "y");
    if (cJSON_IsNumber(item)) {
      y = item->valuedouble;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "z");
    if (cJSON_IsNumber(item)) {
      z = item->valuedouble;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "duration_ms");
    if (cJSON_IsNumber(item)) {
      duration_ms = item->valuedouble;
    }
    cJSON_Delete(args);
  }

  const int8_t move_x = static_cast<int8_t>(constrain(static_cast<int>(x), -100, 100));
  const int8_t move_y = static_cast<int8_t>(constrain(static_cast<int>(y), -100, 100));
  const int8_t move_z = static_cast<int8_t>(constrain(static_cast<int>(z), -100, 100));
  const uint32_t move_duration_ms = static_cast<uint32_t>(constrain(static_cast<int>(duration_ms), 100, 5000));

  noteActivity();
  sequence_running = false;
  diag_running = false;
  setAction("AI MOVE");
  const uint32_t started = millis();
  while (millis() - started < move_duration_ms) {
    roverc.setSpeed(move_x, move_y, move_z);
    delay(50);
  }
  stopAllMotionOutputs();

  return makeToolResponse("ok", "move");
}

char* cb_turn(const char* function_name, const char* arguments, void* user_data) {
  (void)function_name;
  (void)user_data;

  if (!recoverRover(true)) {
    return makeToolResponse("error", "turn");
  }
  if (!M5.Imu.isEnabled()) {
    return makeToolResponse("imu_unavailable", "turn");
  }

  const char* direction = "left";
  double angle_deg = 90;
  double speed_percent = 50;
  cJSON* args = cJSON_Parse(arguments ? arguments : "{}");
  if (args) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, "direction");
    if (cJSON_IsString(item) && item->valuestring) {
      direction = item->valuestring;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "angle_deg");
    if (cJSON_IsNumber(item)) {
      angle_deg = item->valuedouble;
    }
    item = cJSON_GetObjectItemCaseSensitive(args, "speed_percent");
    if (cJSON_IsNumber(item)) {
      speed_percent = item->valuedouble;
    }
  }

  const bool turn_left = std::strcmp(direction, "right") != 0;
  const float target_angle_deg = static_cast<float>(constrain(static_cast<int>(angle_deg), 5, 360));
  const int8_t speed = static_cast<int8_t>(constrain(static_cast<int>(speed_percent), 20, 100));
  const int8_t turn_z = turn_left ? -speed : speed;
  const uint32_t timeout_ms = constrain(static_cast<int>(target_angle_deg * 100.0f), 2000, 12000);

  noteActivity();
  sequence_running = false;
  diag_running = false;
  setAction(turn_left ? "AI TURN LEFT" : "AI TURN RIGHT");

  float turned_deg = 0.0f;
  uint32_t started_at = millis();
  uint32_t prev_ms = started_at;
  while (turned_deg < target_angle_deg && millis() - started_at < timeout_ms) {
    float gx = 0;
    float gy = 0;
    float gz = 0;
    M5.Imu.getGyro(&gx, &gy, &gz);
    const uint32_t now = millis();
    const float dt_s = static_cast<float>(now - prev_ms) / 1000.0f;
    prev_ms = now;

    roverc.setSpeed(0, 0, turn_z);
    const float rate_dps = fmaxf(fabsf(gx), fmaxf(fabsf(gy), fabsf(gz)));
    if (rate_dps > 3.0f) {
      turned_deg += rate_dps * dt_s;
    }
    delay(kAiControlLoopMs);
  }

  stopAllMotionOutputs();
  if (args) {
    cJSON_Delete(args);
  }

  char payload[160];
  std::snprintf(payload,
                sizeof(payload),
                "{\"status\":\"%s\",\"action\":\"turn\",\"target_deg\":%.1f,\"measured_deg\":%.1f}",
                turned_deg >= target_angle_deg ? "ok" : "timeout",
                static_cast<double>(target_angle_deg),
                static_cast<double>(turned_deg));
  return strdup(payload);
}

char* cb_stop(const char* function_name, const char* arguments, void* user_data) {
  (void)function_name;
  (void)arguments;
  (void)user_data;
  noteActivity();
  commandStop("AI STOP");
  return makeToolResponse("ok", "stop");
}

char* cb_gripper_open(const char* function_name, const char* arguments, void* user_data) {
  (void)function_name;
  (void)arguments;
  (void)user_data;
  noteActivity();
  commandGripperOpen();
  return makeToolResponse("ok", "gripper_open");
}

char* cb_gripper_close(const char* function_name, const char* arguments, void* user_data) {
  (void)function_name;
  (void)arguments;
  (void)user_data;
  noteActivity();
  commandGripperClose();
  return makeToolResponse("ok", "gripper_close");
}

void startSequence() {
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
    return;
  }
  diag_running = false;
  sequence_running = true;
  current_step = 0;
  step_started_at = millis();
  applyStep(current_step);
}

void applyDiagRunMotor(uint8_t motor_index) {
  const int8_t p0 = (motor_index == 0) ? 127 : 0;
  const int8_t p1 = (motor_index == 1) ? 127 : 0;
  const int8_t p2 = (motor_index == 2) ? 127 : 0;
  const int8_t p3 = (motor_index == 3) ? 127 : 0;
  roverc.setAllPulse(p0, p1, p2, p3);
  char label[24];
  snprintf(label, sizeof(label), "DIAG M%u", static_cast<unsigned>(motor_index + 1));
  setAction(label);
  logPrintf(LOG_INFO, "DIAG motor %u ON", static_cast<unsigned>(motor_index + 1));
}

void startMotorDiagnostic() {
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
    return;
  }
  sequence_running = false;
  diag_running = true;
  diag_motor_index = 0;
  diag_motor_phase_run = true;
  diag_phase_started_at = millis();
  stopAllMotionOutputs();
  applyDiagRunMotor(diag_motor_index);
}

void updateMotorDiagnostic() {
  if (!diag_running) {
    return;
  }
  const uint32_t now = millis();
  if (diag_motor_phase_run) {
    if (now - diag_phase_started_at >= kDiagRunMs) {
      roverc.setAllPulse(0, 0, 0, 0);
      setAction("DIAG STOP");
      diag_motor_phase_run = false;
      diag_phase_started_at = now;
    }
    return;
  }

  if (now - diag_phase_started_at < kDiagStopMs) {
    return;
  }

  ++diag_motor_index;
  if (diag_motor_index >= kDiagMotors) {
    diag_running = false;
    setAction("IDLE");
    logMessage(LOG_INFO, "DIAG completed");
    return;
  }
  diag_motor_phase_run = true;
  diag_phase_started_at = now;
  applyDiagRunMotor(diag_motor_index);
}

void updateSequence() {
  if (!sequence_running) {
    return;
  }

  const uint32_t duration = kStepDurationMs[current_step];
  if (duration == 0) {
    return;
  }

  const uint32_t now = millis();
  if (now - step_started_at < duration) {
    return;
  }

  ++current_step;
  if (current_step >= kSequenceSteps) {
    current_step = kSequenceSteps - 1;
  }
  step_started_at = now;
  applyStep(current_step);
}

uint16_t actionColor() {
  if (std::strstr(current_action, "EMERGENCY")) return RED;
  if (std::strstr(current_action, "FAIL")) return RED;
  if (std::strstr(current_action, "STOP")) return 0xFDA0;  // orange
  if (std::strstr(current_action, "IDLE")) return GREEN;
  return CYAN;
}

void drawStatus() {
  const int battery = M5.Power.getBatteryLevel();
  if (std::strcmp(last_drawn_action, current_action) == 0 &&
      std::strcmp(last_drawn_wifi, wifi_status) == 0 &&
      last_drawn_battery == battery) {
    return;
  }

  const int w = M5.Display.width();
  const int h = M5.Display.height();
  const int kRowH = h / 3;
  constexpr uint16_t kRow1Bg = 0x18C3;
  constexpr uint16_t kRow2Bg = 0x1082;
  constexpr uint16_t kRow3Bg = 0x0000;

  M5.Display.fillScreen(BLACK);
  M5.Display.fillRect(0, 0, w, kRowH, kRow1Bg);
  M5.Display.fillRect(0, kRowH, w, kRowH, kRow2Bg);
  M5.Display.fillRect(0, kRowH * 2, w, h - (kRowH * 2), kRow3Bg);
  M5.Display.drawFastHLine(0, kRowH, w, 0x4208);
  M5.Display.drawFastHLine(0, kRowH * 2, w, 0x4208);

  M5.Display.setTextDatum(textdatum_t::top_center);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, kRow1Bg);
  M5.Display.drawString("BATTERY", w / 2, 2);
  M5.Display.setTextColor(WHITE, kRow2Bg);
  M5.Display.drawString("IP ADDRESS", w / 2, kRowH + 2);
  M5.Display.setTextColor(actionColor(), kRow3Bg);
  M5.Display.drawString("OPERATION", w / 2, kRowH * 2 + 2);

  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(WHITE, kRow1Bg);
  M5.Display.drawString(String(battery) + "%", w / 2, kRowH / 2 + 8);

  const char* wifi_text = wifi_status;
  if (const char* p = std::strstr(wifi_status, ": ")) {
    wifi_text = p + 2;
  }
  String wifi_short = wifi_text;
  if (wifi_short.length() > 18) {
    wifi_short = wifi_short.substring(0, 18);
  }
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(WHITE, kRow2Bg);
  M5.Display.drawString(wifi_short, w / 2, kRowH + (kRowH / 2) + 8);

  String op_short = current_action;
  if (op_short.length() > 14) {
    op_short = op_short.substring(0, 14);
  }
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(actionColor(), kRow3Bg);
  M5.Display.drawString(op_short, w / 2, kRowH * 2 + (kRowH / 2) + 8);

  std::snprintf(last_drawn_action, sizeof(last_drawn_action), "%s", current_action);
  std::snprintf(last_drawn_wifi, sizeof(last_drawn_wifi), "%s", wifi_status);
  last_drawn_battery = battery;
}

void emitHeartbeat() {
  const uint32_t now = millis();
  if (now - last_heartbeat < kHeartbeatMs) {
    return;
  }
  last_heartbeat = now;

  const int battery = M5.Power.getBatteryLevel();
  logPrintf(LOG_INFO,
            "HB t=%lu action=%s battery=%d%% running=%d step=%u",
            static_cast<unsigned long>(now),
            current_action,
            battery,
            sequence_running ? 1 : 0,
            static_cast<unsigned>(current_step));
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    logPrintf(LOG_INFO, "BTN state A=%d B=%d", M5.BtnA.isPressed() ? 1 : 0, M5.BtnB.isPressed() ? 1 : 0);
  }
}

void handleRoot() {
  static const char kHtml[] = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <title>Rover Control</title>
  <style>
    body{font-family:Arial,sans-serif;margin:16px;background:#101820;color:#f2f2f2}
    h1{margin:0 0 12px}
    .layout{display:flex;flex-direction:column;align-items:flex-start;gap:12px}
    .controls{width:100%;max-width:420px}
    .joy-wrap{
      width:100%;max-width:420px;display:flex;justify-content:center
    }
    canvas{touch-action:none}
    .speed{max-width:420px;margin:0 0 12px;display:flex;align-items:center;gap:10px}
    .speed input{flex:1;accent-color:#2d7dd2}
    .speed span{min-width:42px;text-align:right;font-size:18px;font-weight:bold}
    .btns{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;max-width:420px}
    button{padding:14px;border:0;border-radius:8px;background:#2d7dd2;color:#fff;font-size:16px}
    button.stop{background:#d7263d}
    .wide{grid-column:1/4}
    .chat{max-width:420px;width:100%;margin-top:12px}
    .chat-msgs{height:200px;overflow-y:auto;background:#1a2a3a;border-radius:8px;padding:8px;font-size:14px}
    .chat-msgs .u{color:#4a9de8;margin:4px 0;word-wrap:break-word}
    .chat-msgs .a{color:#f2f2f2;margin:4px 0;word-wrap:break-word}
    .chat-msgs .e{color:#d7263d;margin:4px 0}
    .chat-in{display:flex;gap:8px;margin-top:8px}
    .chat-in input{flex:1;padding:10px;border:1px solid #2d7dd2;border-radius:8px;background:#1a2a3a;color:#f2f2f2;font-size:16px;outline:none}
    .chat-in button{padding:10px 16px}
    @media (max-width: 900px){
      .controls{max-width:none}
      .chat{max-width:none}
    }
  </style>
</head>
<body>
  <h1>Rover Web Control</h1>
  <div class="layout">
    <div class="joy-wrap"><canvas id="joy" width="200" height="200"></canvas></div>
    <div class="controls">
      <div class="speed">
        <span>&#x1F3CE;</span>
        <input type="range" id="spd" min="10" max="100" step="10" value="80">
        <span id="spdVal">80%</span>
      </div>
      <div class="btns">
        <button onclick="rot(-1)">Rotate L</button>
        <button class="stop" onclick="cmd('stop')">STOP</button>
        <button onclick="rot(1)">Rotate R</button>
        <button onclick="cmd('open')">Gripper Open</button>
        <button onclick="cmd('close')">Gripper Close</button>
        <button onclick="cmd('demo')">Run Demo</button>
        <button class="wide stop" onclick="cmd('emergency')">Emergency</button>
      </div>
    </div>
    <div class="chat">
      <div class="chat-msgs" id="msgs"></div>
      <div class="chat-in">
        <input type="text" id="chatIn" placeholder="Ask the rover...">
        <button id="chatSend" onclick="sendChat()">&#9654;</button>
      </div>
    </div>
  </div>
  <script>
    function cmd(a){fetch('/cmd?act='+encodeURIComponent(a)).catch(function(){});}
    var sl=document.getElementById('spd'),sv=document.getElementById('spdVal');
    var spd=80;
    sl.oninput=function(){spd=+this.value;sv.textContent=spd+'%';};
    function rot(dir){
      var z=Math.round(spd*dir);
      fetch('/cmd?act=move&x=0&y=0&z='+z).catch(function(){});
    }
    var C=document.getElementById('joy'),ctx=C.getContext('2d');
    var W=200,cx=100,cy=100,bR=80,kR=25,dz=10;
    var kx=cx,ky=cy,act=false,tmr=null;
    function draw(){
      ctx.clearRect(0,0,W,W);
      ctx.beginPath();ctx.arc(cx,cy,bR,0,6.283);
      ctx.fillStyle='#1a2a3a';ctx.fill();
      ctx.strokeStyle='#2d7dd2';ctx.lineWidth=2;ctx.stroke();
      ctx.strokeStyle='#223344';ctx.lineWidth=1;
      ctx.beginPath();ctx.moveTo(cx-bR,cy);ctx.lineTo(cx+bR,cy);ctx.stroke();
      ctx.beginPath();ctx.moveTo(cx,cy-bR);ctx.lineTo(cx,cy+bR);ctx.stroke();
      ctx.beginPath();ctx.arc(kx,ky,kR,0,6.283);
      ctx.fillStyle=act?'#4a9de8':'#2d7dd2';ctx.fill();
    }
    function sendJoy(){
      var dx=kx-cx,dy=-(ky-cy);
      if(Math.abs(dx)<dz)dx=0;if(Math.abs(dy)<dz)dy=0;
      var x=Math.round(Math.max(-100,Math.min(100,dx/bR*100))*spd/100);
      var y=Math.round(Math.max(-100,Math.min(100,dy/bR*100))*spd/100);
      fetch('/cmd?act=move&x='+x+'&y='+y).catch(function(){});
    }
    function onDown(e){
      act=true;C.setPointerCapture(e.pointerId);
      onMv(e);sendJoy();tmr=setInterval(sendJoy,100);
    }
    function onMv(e){
      if(!act)return;
      var r=C.getBoundingClientRect();
      var dx=e.clientX-r.left-cx,dy=e.clientY-r.top-cy;
      var d=Math.sqrt(dx*dx+dy*dy);
      if(d>bR){dx=dx/d*bR;dy=dy/d*bR;}
      kx=cx+dx;ky=cy+dy;draw();
    }
    function onUp(){
      act=false;kx=cx;ky=cy;draw();
      if(tmr){clearInterval(tmr);tmr=null;}
      cmd('stop');
    }
    C.addEventListener('pointerdown',onDown);
    C.addEventListener('pointermove',onMv);
    C.addEventListener('pointerup',onUp);
    C.addEventListener('pointercancel',onUp);
    draw();
    var msgs=document.getElementById('msgs'),chatIn=document.getElementById('chatIn');
    function addMsg(cls,txt){
      var d=document.createElement('div');d.className=cls;d.textContent=txt;
      msgs.appendChild(d);msgs.scrollTop=msgs.scrollHeight;return d;
    }
    function sendChat(){
      var m=chatIn.value.trim();if(!m)return;
      chatIn.value='';addMsg('u','> '+m);
      var dots=addMsg('a','...');
      chatIn.disabled=true;
      fetch('/chat?msg='+encodeURIComponent(m))
        .then(function(r){return r.json();})
        .then(function(d){
          dots.textContent=d.reply||d.error||'No response';
          if(d.error)dots.className='e';
        })
        .catch(function(e){dots.textContent='Error: '+e;dots.className='e';})
        .finally(function(){chatIn.disabled=false;chatIn.focus();});
    }
    chatIn.addEventListener('keydown',function(e){if(e.key==='Enter')sendChat();});
  </script>
</body>
</html>
)HTML";
  server.send(200, "text/html", kHtml);
}

void handleCmd() {
  if (!server.hasArg("act")) {
    server.send(400, "text/plain", "Missing act");
    return;
  }

  const String act = server.arg("act");
  noteActivity();
  if (act == "forward") {
    commandMove("WEB FORWARD", 0, kWebSpeedPercent, 0);
  } else if (act == "backward") {
    commandMove("WEB BACKWARD", 0, -kWebSpeedPercent, 0);
  } else if (act == "left") {
    commandMove("WEB LEFT", -kWebSpeedPercent, 0, 0);
  } else if (act == "right") {
    commandMove("WEB RIGHT", kWebSpeedPercent, 0, 0);
  } else if (act == "rotate_l") {
    commandMove("WEB ROTATE L", 0, 0, -kWebSpeedPercent);
  } else if (act == "rotate_r") {
    commandMove("WEB ROTATE R", 0, 0, kWebSpeedPercent);
  } else if (act == "move") {
    int x = server.hasArg("x") ? server.arg("x").toInt() : 0;
    int y = server.hasArg("y") ? server.arg("y").toInt() : 0;
    int z = server.hasArg("z") ? server.arg("z").toInt() : 0;
    x = constrain(x, -100, 100);
    y = constrain(y, -100, 100);
    z = constrain(z, -100, 100);
    commandMove("WEB JOYSTICK", static_cast<int8_t>(x), static_cast<int8_t>(y), static_cast<int8_t>(z));
  } else if (act == "stop") {
    commandStop("WEB STOP");
  } else if (act == "open") {
    commandGripperOpen();
  } else if (act == "close") {
    commandGripperClose();
  } else if (act == "demo") {
    startSequence();
  } else if (act == "emergency") {
    emergencyStop();
  } else {
    server.send(400, "text/plain", "Unknown act");
    return;
  }

  server.send(200, "text/plain", "OK");
}

void handleChat() {
  if (!server.hasArg("msg")) {
    server.send(400, "application/json", "{\"error\":\"Missing msg\"}");
    return;
  }
  noteActivity();

  if (!ai_handle) {
    server.send(503, "application/json", "{\"error\":\"AI not available\"}");
    return;
  }

  String msg = server.arg("msg");
  char response[2048] = {};
  esp_err_t err = openrouter_call_with_tools(ai_handle, msg.c_str(), response, sizeof(response), 5);

  auto jsonEscape = [](const char* s) {
    String out;
    for (size_t i = 0; s[i]; ++i) {
      char c = s[i];
      if (c == '"') out += "\\\"";
      else if (c == '\\') out += "\\\\";
      else if (c == '\n') out += "\\n";
      else if (c == '\r') continue;
      else if (c == '\t') out += "\\t";
      else out += c;
    }
    return out;
  };

  if (err == ESP_OK) {
    server.send(200, "application/json",
                "{\"reply\":\"" + jsonEscape(response) + "\"}");
  } else {
    String detail = esp_err_to_name(err);
    detail += " (0x" + String(static_cast<uint32_t>(err), HEX) + ")";
    if (response[0]) {
      detail += ": ";
      detail += response;
    }
    server.send(500, "application/json",
                "{\"error\":\"" + jsonEscape(detail.c_str()) + "\"}");
  }
}

void setupWifiAndServer() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  setWifiStatus("WiFi: connecting...");

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiConnectTimeoutMs) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    setWifiStatus("WiFi: connect failed");
    return;
  }

  char line[40];
  std::snprintf(line, sizeof(line), "WiFi: %s", WiFi.localIP().toString().c_str());
  setWifiStatus(line);
  setupSyslog();

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/chat", handleChat);
  server.begin();
  web_server_started = true;
  logPrintf(LOG_INFO, "Web server started: http://%s/", WiFi.localIP().toString().c_str());
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  rover_ready = roverc.begin();
  logPrintf(LOG_INFO, "Rover begin: %s", rover_ready ? "OK" : "FAILED");
  if (rover_ready) {
    stopAllMotionOutputs();
    setGripperAngle(kGripperCloseAngle);
  }
  setupWifiAndServer();

#ifdef OPENROUTER_API_KEY
  if (WiFi.status() == WL_CONNECTED) {
    static const char* kTurnDirectionEnum[] = {"left", "right", nullptr};
    static const openrouter_param_t kMoveParams[] = {
        {"x", "number", "Lateral speed from -100 to 100", true, nullptr},
        {"y", "number", "Forward speed from -100 to 100", true, nullptr},
        {"z", "number", "Rotation speed from -100 to 100", false, nullptr},
        {"duration_ms", "number", "Move duration in milliseconds (100-5000)", false, nullptr},
        {nullptr, nullptr, nullptr, false, nullptr},
    };
    static const openrouter_param_t kTurnParams[] = {
        {"direction", "string", "Turn direction", true, kTurnDirectionEnum},
        {"angle_deg", "number", "Target angle in degrees (5-360)", false, nullptr},
        {"speed_percent", "number", "Rotation speed percent (20-100)", false, nullptr},
        {nullptr, nullptr, nullptr, false, nullptr},
    };
    static const openrouter_simple_function_t kToolMove = {
        "move",
        "Move the rover for duration_ms, then stop.",
        kMoveParams,
        cb_move,
        nullptr,
    };
    static const openrouter_simple_function_t kToolTurn = {
        "turn",
        "Rotate the rover in place by angle_deg using IMU feedback.",
        kTurnParams,
        cb_turn,
        nullptr,
    };
    static const openrouter_simple_function_t kToolStop = {
        "stop",
        "Stop all rover motion immediately.",
        nullptr,
        cb_stop,
        nullptr,
    };
    static const openrouter_simple_function_t kToolGripperOpen = {
        "gripper_open",
        "Open the rover gripper.",
        nullptr,
        cb_gripper_open,
        nullptr,
    };
    static const openrouter_simple_function_t kToolGripperClose = {
        "gripper_close",
        "Close the rover gripper.",
        nullptr,
        cb_gripper_close,
        nullptr,
    };
    static const openrouter_simple_function_t kToolReadImu = {
        "read_imu",
        "Read current accelerometer and gyroscope values.",
        nullptr,
        cb_read_imu,
        nullptr,
    };

    openrouter_config_t ai_cfg = {};
    ai_cfg.api_key = OPENROUTER_API_KEY;
    ai_cfg.default_model = "google/gemini-2.0-flash-lite-001";
    ai_cfg.default_system_role =
        "You are the AI brain of a mecanum-wheel rover robot with a gripper. "
        "Use the provided tools to control the rover when the user asks. "
        "For movement commands with duration, call move() which blocks for the specified time then stops. "
        "For angle-based rotations, use turn(direction, angle_deg) which uses IMU feedback. "
        "You can inspect sensors with read_imu(). "
        "You can chain multiple tool calls for sequences like 'forward then turn'. "
        "Respond naturally in the user's language.";
    ai_cfg.max_tokens = 256;
    ai_cfg.enable_streaming = false;
    ai_cfg.enable_tools = true;
    ai_handle = openrouter_create(&ai_cfg);
    logPrintf(LOG_INFO, "AI handle: %s", ai_handle ? "OK" : "FAILED");
    if (ai_handle) {
      esp_err_t reg_err = openrouter_register_simple_function(ai_handle, &kToolMove);
      if (reg_err == ESP_OK) reg_err = openrouter_register_simple_function(ai_handle, &kToolTurn);
      if (reg_err == ESP_OK) reg_err = openrouter_register_simple_function(ai_handle, &kToolStop);
      if (reg_err == ESP_OK) reg_err = openrouter_register_simple_function(ai_handle, &kToolGripperOpen);
      if (reg_err == ESP_OK) reg_err = openrouter_register_simple_function(ai_handle, &kToolGripperClose);
      if (reg_err == ESP_OK) reg_err = openrouter_register_simple_function(ai_handle, &kToolReadImu);
      logPrintf(LOG_INFO,
                "AI tools: %s (%s)",
                reg_err == ESP_OK ? "READY" : "FAILED",
                esp_err_to_name(reg_err));

      char warmup_response[1024] = {};
      const esp_err_t warmup_err = openrouter_call(ai_handle,
                                                   "Say exactly: Rover AI online.",
                                                   warmup_response,
                                                   sizeof(warmup_response));
      if (warmup_err == ESP_OK) {
        logPrintf(LOG_INFO, "AI response: %s", warmup_response);
      } else {
        logPrintf(LOG_ERR,
                  "AI warmup failed: %s (0x%08lx)",
                  esp_err_to_name(warmup_err),
                  static_cast<unsigned long>(warmup_err));
      }
    }
  }
#endif

  M5.Display.setRotation(3);
  M5.Display.setBrightness(80);
  last_activity_at = millis();
  setAction(rover_ready ? "IDLE" : "ROVER I2C FAIL");
  drawStatus();
}

void loop() {
  M5.update();

  if (M5.BtnB.wasPressed() || M5.BtnB.wasClicked()) {
    logMessage(LOG_INFO, "BtnB pressed");
    noteActivity();
    emergencyStop();
  }

  if (M5.BtnA.wasPressed() || M5.BtnA.wasClicked()) {
    logMessage(LOG_INFO, "BtnA pressed");
    noteActivity();
    startSequence();
  }

  updateSequence();
  updateMotorDiagnostic();
  refreshMotionCommand();
  if (web_server_started) {
    server.handleClient();
  }
  checkSleepTimeout();
  drawStatus();
  emitHeartbeat();
}
