#include <M5Unified.h>
#include "M5_RoverC.h"
#include <cstring>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <WebServer.h>
#include "secrets.h"

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
constexpr uint8_t kSequenceSteps = 7;
constexpr uint8_t kDiagMotors = 4;
constexpr uint32_t kDiagRunMs = 1000;
constexpr uint32_t kDiagStopMs = 300;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kScreenSleepMs = 120000;

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
  Serial.println("Entering deep sleep...");
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
  Serial.printf("Action: %s\n", current_action);
}

void setWifiStatus(const char* status) {
  std::snprintf(wifi_status, sizeof(wifi_status), "%s", status);
  Serial.println(wifi_status);
}

bool recoverRover(const bool force_reinit = false) {
  if (rover_ready && !force_reinit) {
    return true;
  }
  rover_ready = roverc.begin();
  Serial.printf("Rover reinit: %s\n", rover_ready ? "OK" : "FAILED");
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
  Serial.printf("Gripper servo=%u angle=%u sent x%u\n",
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
    @media (max-width: 900px){
      .controls{max-width:none}
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

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

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
    setGripperAngle(kGripperCloseAngle);
  }
  setupWifiAndServer();

  M5.Display.setRotation(3);
  M5.Display.setBrightness(80);
  last_activity_at = millis();
  setAction(rover_ready ? "IDLE" : "ROVER I2C FAIL");
  drawStatus();
}

void loop() {
  M5.update();

  if (M5.BtnB.wasPressed() || M5.BtnB.wasClicked()) {
    Serial.println("BtnB pressed");
    noteActivity();
    emergencyStop();
  }

  if (M5.BtnA.wasPressed() || M5.BtnA.wasClicked()) {
    Serial.println("BtnA pressed");
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
