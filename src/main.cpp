#include <M5Unified.h>
#include "M5_RoverC.h"
#include <cstring>
#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h"

namespace {

constexpr int8_t kSpeedPercent = 100;
constexpr int8_t kWebSpeedPercent = 80;
constexpr uint8_t kGripperServo = 0;
constexpr uint8_t kGripperOpenAngle = 90;
constexpr uint8_t kGripperCloseAngle = 10;

constexpr uint32_t kHeartbeatMs = 1000;
constexpr uint32_t kMotionRefreshMs = 50;
constexpr uint8_t kSequenceSteps = 7;
constexpr uint8_t kDiagMotors = 4;
constexpr uint32_t kDiagRunMs = 1000;
constexpr uint32_t kDiagStopMs = 300;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;

constexpr const char* kWifiSsid = WIFI_SSID;
constexpr const char* kWifiPassword = WIFI_PASSWORD;

M5_RoverC roverc;
WebServer server(80);
bool rover_ready = false;
bool web_server_started = false;

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

const uint32_t kStepDurationMs[kSequenceSteps] = {
    1000,  // FORWARD 50%
    500,   // STOP
    1000,  // BACKWARD 50%
    500,   // STOP
    500,   // GRIPPER OPEN
    500,   // GRIPPER CLOSE
    0      // IDLE (terminal step)
};

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
  Serial.printf("Action: %s\n", current_action);
}

void setWifiStatus(const char* status) {
  std::snprintf(wifi_status, sizeof(wifi_status), "%s", status);
  Serial.println(wifi_status);
}

void applyStep(uint8_t step) {
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
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
      roverc.setServoAngle(kGripperServo, kGripperOpenAngle);
      break;
    case 5:
      setAction("GRIPPER CLOSE");
      disableMotionCommand();
      stopMotors();
      roverc.setServoAngle(kGripperServo, kGripperCloseAngle);
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
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
    return;
  }
  sequence_running = false;
  diag_running = false;
  setAction("GRIPPER OPEN");
  stopAllMotionOutputs();
  roverc.setServoAngle(kGripperServo, kGripperOpenAngle);
}

void commandGripperClose() {
  if (!rover_ready) {
    setAction("ROVER I2C FAIL");
    return;
  }
  sequence_running = false;
  diag_running = false;
  setAction("GRIPPER CLOSE");
  stopAllMotionOutputs();
  roverc.setServoAngle(kGripperServo, kGripperCloseAngle);
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
  Serial.printf("DIAG motor %u ON\n", static_cast<unsigned>(motor_index + 1));
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
    Serial.println("DIAG completed");
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

void drawStatus() {
  const int battery = M5.Power.getBatteryLevel();
  if (std::strcmp(last_drawn_action, current_action) == 0 &&
      std::strcmp(last_drawn_wifi, wifi_status) == 0 &&
      last_drawn_battery == battery) {
    return;
  }

  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(6, 16);
  M5.Display.printf("Action:");
  M5.Display.setCursor(6, 44);
  M5.Display.printf("%s", current_action);
  M5.Display.setCursor(6, 92);
  M5.Display.printf("Battery: %d%%", battery);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(6, 118);
  M5.Display.printf("%s", wifi_status);

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
  Serial.printf("HB t=%lu action=%s battery=%d%% running=%d step=%u\n",
                static_cast<unsigned long>(now),
                current_action,
                battery,
                sequence_running ? 1 : 0,
                static_cast<unsigned>(current_step));
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    Serial.printf("BTN state A=%d B=%d\n", M5.BtnA.isPressed() ? 1 : 0, M5.BtnB.isPressed() ? 1 : 0);
  }
}

void handleRoot() {
  static const char kHtml[] = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Rover Control</title>
  <style>
    body{font-family:Arial,sans-serif;margin:16px;background:#101820;color:#f2f2f2}
    h1{margin:0 0 12px}
    .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;max-width:420px}
    button{padding:14px;border:0;border-radius:8px;background:#2d7dd2;color:#fff;font-size:16px}
    button.stop{background:#d7263d}
    .wide{grid-column:1/4}
  </style>
</head>
<body>
  <h1>Rover Web Control</h1>
  <div class="grid">
    <button onclick="cmd('forward')">Forward</button>
    <button onclick="cmd('left')">Left</button>
    <button onclick="cmd('right')">Right</button>
    <button onclick="cmd('backward')">Backward</button>
    <button onclick="cmd('rotate_l')">Rotate L</button>
    <button onclick="cmd('rotate_r')">Rotate R</button>
    <button class="wide stop" onclick="cmd('stop')">STOP</button>
    <button onclick="cmd('open')">Gripper Open</button>
    <button onclick="cmd('close')">Gripper Close</button>
    <button onclick="cmd('demo')">Run Demo</button>
    <button class="stop" onclick="cmd('emergency')">Emergency</button>
  </div>
  <script>
    function cmd(act){ fetch('/cmd?act='+encodeURIComponent(act)).catch(()=>{}); }
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

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.begin();
  web_server_started = true;
  Serial.printf("Web server started: http://%s/\n", WiFi.localIP().toString().c_str());
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  rover_ready = roverc.begin();
  Serial.printf("Rover begin: %s\n", rover_ready ? "OK" : "FAILED");
  if (rover_ready) {
    stopAllMotionOutputs();
    roverc.setServoAngle(kGripperServo, kGripperCloseAngle);
  }
  setupWifiAndServer();

  M5.Display.setRotation(3);
  setAction(rover_ready ? "IDLE" : "ROVER I2C FAIL");
  drawStatus();
}

void loop() {
  M5.update();

  if (M5.BtnB.wasPressed() || M5.BtnB.wasClicked()) {
    Serial.println("BtnB pressed");
    emergencyStop();
  }

  if (M5.BtnA.wasPressed() || M5.BtnA.wasClicked()) {
    Serial.println("BtnA pressed");
    startSequence();
  }

  updateSequence();
  updateMotorDiagnostic();
  refreshMotionCommand();
  if (web_server_started) {
    server.handleClient();
  }
  drawStatus();
  emitHeartbeat();
}
