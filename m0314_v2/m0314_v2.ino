/*
  ==========================================================
   ESP32 WEB CAR + EMERGENCY SYSTEM  v4.0
   Wi-Fi Web Interface (No Bluetooth required)
  ==========================================================

  REQUIREMENTS:
    Install "ESPAsyncWebServer" & "AsyncTCP" libraries via Arduino Library Manager.

  ACCESS INSTRUCTIONS:
    1. Connect phone/laptop Wi-Fi to: ESP32_CAR_AP (Password: password123)
    2. Open browser and go to: http://192.168.4.1
  ==========================================================
*/

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <TinyGPSPlus.h>

// ================= PIN CONFIG =================

#define SOS_BUTTON    14

// Limit switch = physical crash sensor.
// Wire: one terminal -> GND, other terminal -> GPIO26.
// INPUT_PULLUP, so LOW = pressed = CRASH (same logic as the test sketch).
// SOS button lives on GPIO14, so each switch has its own pin.
#define LIMIT_SWITCH_PIN 26

#define MPU_SDA       21
#define MPU_SCL       22

#define GPS_RX        16
#define GPS_TX        17

#define GSM_RX        13
#define GSM_TX        12

#define MOTOR_L_IN1   32
#define MOTOR_L_IN2   33
#define MOTOR_R_IN3   25
#define MOTOR_R_IN4   27

// ================= SETTINGS =================

const String PHONE_NUMBER      = "+918007903776";
const float  TILT_THRESHOLD    = 45.0;
const float  IMPACT_THRESHOLD  = 1.8;
const unsigned long CALIB_TIME = 5000;

// Wi-Fi Access Point Credentials (for car driving)
const char* AP_SSID = "ESP32_CAR_AP";
const char* AP_PASS = "12345678";

// >>> FLASK CONFIG — EDIT THESE <<<
// Phone hotspot that ESP32 joins for internet:
const char* STA_SSID  = "Airtel_priy_0001";
const char* STA_PASS  = "air86654";
// Your deployed Flask URL (Render), e.g. https://your-app.onrender.com/api/alert
const char* FLASK_URL = "https://caraccident-zxid.onrender.com//api/alert";
const char* DEVICE_ID = "ESP32_CAR_01";

AsyncWebServer server(80);

// ================= HARDWARE SERIAL =================

HardwareSerial gpsSerial(1);
HardwareSerial gsmSerial(2);
TinyGPSPlus    gps;

// ================= MPU6050 =================

#define MPU_ADDR 0x68

int16_t ax, ay, az, gx, gy, gz;

float calibRoll = 0, calibPitch = 0;
float curRoll   = 0, curPitch   = 0;
float relRoll   = 0, relPitch   = 0;
bool  mpuReady  = false;

unsigned long lastMpuPrint = 0;

// ================= ALERT STATE =================

bool          alertPending   = false;
unsigned long alertStartMs   = 0;
String        alertReason    = "";
bool          lastBtnState   = HIGH;
unsigned long lastBtnMs      = 0;
bool          lastLimState   = HIGH;
unsigned long lastLimMs      = 0;

// ================= MOTOR STATE =================

char lastCmd = 'S';

// Forward function declarations
void triggerAlert(const String& reason);
void runEmergencyProtocol();
void sendAlertToFlask(const String& reason);

// =====================================================
// MOTORS (pure GPIO)
// =====================================================

void motorForward()
{
  digitalWrite(MOTOR_L_IN1, HIGH); digitalWrite(MOTOR_L_IN2, LOW);
  digitalWrite(MOTOR_R_IN3, HIGH); digitalWrite(MOTOR_R_IN4, LOW);
}
void motorReverse()
{
  digitalWrite(MOTOR_L_IN1, LOW);  digitalWrite(MOTOR_L_IN2, HIGH);
  digitalWrite(MOTOR_R_IN3, LOW);  digitalWrite(MOTOR_R_IN4, HIGH);
}
void motorLeft()
{
  digitalWrite(MOTOR_L_IN1, LOW);  digitalWrite(MOTOR_L_IN2, HIGH);
  digitalWrite(MOTOR_R_IN3, HIGH); digitalWrite(MOTOR_R_IN4, LOW);
}
void motorRight()
{
  digitalWrite(MOTOR_L_IN1, HIGH); digitalWrite(MOTOR_L_IN2, LOW);
  digitalWrite(MOTOR_R_IN3, LOW);  digitalWrite(MOTOR_R_IN4, HIGH);
}
void motorStop()
{
  digitalWrite(MOTOR_L_IN1, LOW); digitalWrite(MOTOR_L_IN2, LOW);
  digitalWrite(MOTOR_R_IN3, LOW); digitalWrite(MOTOR_R_IN4, LOW);
}

// =====================================================
// COMMAND HANDLER
// =====================================================

void handleCmd(char cmd)
{
  if (cmd == lastCmd) return;
  lastCmd = cmd;

  Serial.print("[CMD] "); Serial.println(cmd);

  switch (cmd)
  {
    case 'F': case 'f': motorForward(); break;
    case 'B': case 'b': motorReverse(); break;
    case 'L': case 'l': motorLeft();    break;
    case 'R': case 'r': motorRight();   break;
    case 'S': case 's': motorStop();    break;
    default: break;
  }
}

// =====================================================
// MPU6050
// =====================================================

void writeMPU(byte reg, byte val)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

void readMPU()
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 14);
  if (Wire.available() >= 14)
  {
    ax = Wire.read()<<8 | Wire.read();
    ay = Wire.read()<<8 | Wire.read();
    az = Wire.read()<<8 | Wire.read();
    gx = Wire.read()<<8 | Wire.read();
    gy = Wire.read()<<8 | Wire.read();
    gz = Wire.read()<<8 | Wire.read();
    Wire.read(); Wire.read(); // temp
  }
}

void calcAngles()
{
  curRoll  = atan2((float)ay, sqrt((float)ax*ax+(float)az*az)) * 180.0/PI;
  curPitch = atan2((float)ax, sqrt((float)ay*ay+(float)az*az)) * 180.0/PI;
}

bool checkMPU()
{
  Wire.beginTransmission(MPU_ADDR);
  return Wire.endTransmission() == 0;
}

void calibrateMPU()
{
  Serial.println("[MPU] Calibrating — keep STILL for 5 s...");
  float rs=0, ps=0; int n=0;
  unsigned long t0 = millis();
  while (millis()-t0 < CALIB_TIME)
  {
    readMPU(); calcAngles();
    rs += curRoll; ps += curPitch; n++;
  }
  if (n > 0) { calibRoll = rs/n; calibPitch = ps/n; }
  Serial.print("[MPU] Ref Roll="); Serial.print(calibRoll,2);
  Serial.print("  Pitch="); Serial.println(calibPitch,2);
}

// =====================================================
// WEB INTERFACE HTML / CSS / JS
// =====================================================

const char HTML_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>ESP32 Vehicle Control Center</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; }
    body { background-color: #f4f6f8; color: #1a202c; display: flex; flex-direction: column; align-items: center; padding: 16px; min-height: 100vh; }
    
    .header { width: 100%; max-width: 480px; display: flex; justify-content: space-between; align-items: center; padding-bottom: 16px; border-bottom: 1px solid #e2e8f0; margin-bottom: 20px; }
    .title { font-size: 18px; font-weight: 700; color: #2d3748; }
    .badge { background-color: #319795; color: white; padding: 4px 8px; border-radius: 4px; font-size: 11px; font-weight: 600; text-transform: uppercase; }

    .telemetry-grid { width: 100%; max-width: 480px; display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; margin-bottom: 24px; }
    .card { background: white; border: 1px solid #e2e8f0; border-radius: 8px; padding: 12px; box-shadow: 0 1px 3px rgba(0,0,0,0.05); }
    .card-label { font-size: 11px; color: #718096; font-weight: 600; text-transform: uppercase; margin-bottom: 4px; }
    .card-value { font-size: 16px; font-weight: 700; color: #1a202c; }

    .control-panel { width: 100%; max-width: 480px; display: flex; flex-direction: column; align-items: center; gap: 12px; margin-bottom: 24px; }
    .dpad-row { display: flex; gap: 12px; justify-content: center; width: 100%; }

    .btn-drive {
      width: 90px; height: 75px; background: #ffffff; border: 2px solid #cbd5e0; border-radius: 8px;
      font-size: 14px; font-weight: 700; color: #2d3748; display: flex; flex-direction: column; align-items: center;
      justify-content: center; cursor: pointer; user-select: none; -webkit-user-select: none;
      box-shadow: 0 2px 4px rgba(0,0,0,0.05); transition: background-color 0.1s, border-color 0.1s;
    }
    .btn-drive:active { background: #edf2f7; border-color: #4a5568; transform: translateY(1px); }
    .btn-stop { background: #e53e3e; border-color: #c53030; color: white; }
    .btn-stop:active { background: #9b2c2c; }

    .status-banner { width: 100%; max-width: 480px; background: #edf2f7; border-radius: 6px; padding: 10px; font-size: 13px; font-weight: 600; text-align: center; color: #4a5568; }
    .status-alert { background: #fed7d7; color: #9b2c2c; border: 1px solid #feb2b2; }
  </style>
</head>
<body>

  <div class="header">
    <div class="title">ESP32 Control Center</div>
    <div class="badge">SYSTEM READY</div>
  </div>

  <div class="telemetry-grid">
    <div class="card">
      <div class="card-label">Roll / Pitch</div>
      <div class="card-value" id="val-angle">0.0° / 0.0°</div>
    </div>
    <div class="card">
      <div class="card-label">GPS Fix Status</div>
      <div class="card-value" id="val-gps">SEARCHING</div>
    </div>
  </div>

  <div class="control-panel">
    <div class="dpad-row">
      <button class="btn-drive" onmousedown="sendCmd('F')" onmouseup="sendCmd('S')" ontouchstart="sendCmd('F')" ontouchend="sendCmd('S')">▲<br>FORWARD</button>
    </div>
    <div class="dpad-row">
      <button class="btn-drive" onmousedown="sendCmd('L')" onmouseup="sendCmd('S')" ontouchstart="sendCmd('L')" ontouchend="sendCmd('S')">◄<br>LEFT</button>
      <button class="btn-drive btn-stop" onclick="sendCmd('S')">■<br>STOP</button>
      <button class="btn-drive" onmousedown="sendCmd('R')" onmouseup="sendCmd('S')" ontouchstart="sendCmd('R')" ontouchend="sendCmd('S')">►<br>RIGHT</button>
    </div>
    <div class="dpad-row">
      <button class="btn-drive" onmousedown="sendCmd('B')" onmouseup="sendCmd('S')" ontouchstart="sendCmd('B')" ontouchend="sendCmd('S')">▼<br>REVERSE</button>
    </div>
  </div>

  <div class="status-banner" id="status-box">Status: Operational</div>

  <script>
    function sendCmd(cmd) {
      fetch('/cmd?dir=' + cmd).catch(err => console.log(err));
    }

    function updateTelemetry() {
      fetch('/status').then(res => res.json()).then(data => {
        document.getElementById('val-angle').innerText = data.roll + '° / ' + data.pitch + '°';
        document.getElementById('val-gps').innerText = data.gps;
        
        let box = document.getElementById('status-box');
        if (data.alert) {
          box.innerText = "ALERT: " + data.reason;
          box.classList.add("status-alert");
        } else {
          box.innerText = "Status: Operational (Cmd: " + data.cmd + ")";
          box.classList.remove("status-alert");
        }
      }).catch(err => console.log(err));
    }

    setInterval(updateTelemetry, 500);
  </script>

</body>
</html>
)rawliteral";

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);
  pinMode(SOS_BUTTON, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);

  // Motors
  pinMode(MOTOR_L_IN1, OUTPUT); pinMode(MOTOR_L_IN2, OUTPUT);
  pinMode(MOTOR_R_IN3, OUTPUT); pinMode(MOTOR_R_IN4, OUTPUT);
  motorStop();

  // Start Wi-Fi: AP (car control) + STA (internet for Flask)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("[WiFi] AP started at: ");
  Serial.println(WiFi.softAPIP());

  // Connect to hotspot for internet (non-blocking, 10 s try)
  WiFi.begin(STA_SSID, STA_PASS);
  Serial.print("[WiFi] Joining hotspot...");
  unsigned long w0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - w0 < 10000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WiFi] Internet OK, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] No internet — car control still works, Flask alerts will retry at crash time");
  }

  // Web Server Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", HTML_INDEX);
  });

  server.on("/cmd", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("dir")) {
      String dir = request->getParam("dir")->value();
      if (dir.length() > 0 && !alertPending) {
        handleCmd(dir.charAt(0));
      }
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"roll\":" + String(curRoll, 1) + ",";
    json += "\"pitch\":" + String(curPitch, 1) + ",";
    json += "\"gps\":\"" + String(gps.location.isValid() ? "LOCKED" : "SEARCHING") + "\",";
    json += "\"alert\":" + String(alertPending ? "true" : "false") + ",";
    json += "\"reason\":\"" + alertReason + "\",";
    json += "\"cmd\":\"" + String(lastCmd) + "\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.begin();

  // MPU6050
  Wire.begin(MPU_SDA, MPU_SCL);
  delay(200);
  if (checkMPU())
  {
    writeMPU(0x6B, 0x00);
    writeMPU(0x1C, 0x00);
    writeMPU(0x1B, 0x00);
    delay(200);
    mpuReady = true;
    calibrateMPU();
  }

  // Hardware Serials
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
  delay(2000);
  gsmSerial.println("AT");         delay(500);
  gsmSerial.println("ATE0");       delay(500);
  gsmSerial.println("AT+CMGF=1");  delay(500);

  Serial.println("\n[SYSTEM] READY — Connect to Wi-Fi SSID 'ESP32_CAR_AP' and visit http://192.168.4.1");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  // GPS feed
  while (gpsSerial.available())
    gps.encode(gpsSerial.read());

  // MPU read + impact detection
  bool tiltDetected   = false;
  bool impactDetected = false;

  if (mpuReady)
  {
    readMPU();
    calcAngles();

    relRoll  = curRoll  - calibRoll;
    relPitch = curPitch - calibPitch;

    float accG    = sqrt((float)ax*ax+(float)ay*ay+(float)az*az) / 16384.0;
    float accDiff = fabs(accG - 1.0);

    if (fabs(relRoll) > TILT_THRESHOLD || fabs(relPitch) > TILT_THRESHOLD)
      tiltDetected = true;

    if (accDiff > IMPACT_THRESHOLD)
      impactDetected = true;
  }

  // SOS button debounce
  bool btnNow = digitalRead(SOS_BUTTON);
  bool btnPressed = false;
  if (btnNow == LOW && lastBtnState == HIGH && (millis() - lastBtnMs) > 50)
  {
    btnPressed = true;
    lastBtnMs  = millis();
  }
  lastBtnState = btnNow;

  // Limit switch (crash) debounce — LOW = pressed = crash
  bool limNow = digitalRead(LIMIT_SWITCH_PIN);
  bool limPressed = false;
  if (limNow == LOW && lastLimState == HIGH && (millis() - lastLimMs) > 50)
  {
    limPressed = true;
    lastLimMs  = millis();
  }
  lastLimState = limNow;

  // Alert system handling
  if (!alertPending)
  {
    if      (limPressed)       triggerAlert("LIMIT SWITCH CRASH");
    else if (btnPressed)       triggerAlert("CAR DAMAGED");
    else if (impactDetected) triggerAlert("IMPACT DETECTED");
    else if (tiltDetected)   triggerAlert("ACCIDENT OCCURED");
  }
  else
  {
    motorStop();
    unsigned long elapsed = millis() - alertStartMs;

    if (btnPressed && elapsed > 500)
    {
      alertPending = false;
      Serial.println("[ALERT] CANCELLED");
    }
    else if (elapsed >= 5000)
    {
      alertPending = false;
      runEmergencyProtocol();
    }
  }
}

// =====================================================
// ALERT & EMERGENCY PROTOCOL
// =====================================================

void triggerAlert(const String& reason)
{
  alertPending  = true;
  alertStartMs  = millis();
  alertReason   = reason;
  motorStop();
  Serial.println("[ALERT] " + reason + " — sending in 5s");
}

void runEmergencyProtocol()
{
  Serial.println("[EMERGENCY] Protocol started");

  unsigned long t0 = millis();
  while (millis() - t0 < 5000)
    while (gpsSerial.available()) gps.encode(gpsSerial.read());

  // 1) FLASK — notify nearby hospitals (needs internet)
  sendAlertToFlask(alertReason);

  // 2) GSM SMS backup (original behaviour)
  gsmSerial.println("AT");         delay(500);
  gsmSerial.println("ATH");        delay(500);
  gsmSerial.println("AT+CMGF=1");  delay(500);

  gsmSerial.print("AT+CMGS=\""); gsmSerial.print(PHONE_NUMBER); gsmSerial.println("\"");
  delay(2000);

  gsmSerial.println("EMERGENCY ALERT!");
  gsmSerial.print("Reason: "); gsmSerial.println(alertReason);

  if (gps.location.isValid())
  {
    gsmSerial.print("Map: https://maps.google.com/?q=");
    gsmSerial.print(gps.location.lat(), 6); gsmSerial.print(",");
    gsmSerial.println(gps.location.lng(), 6);
  }
  else gsmSerial.println("GPS: no fix");

  gsmSerial.write(26); // Send SMS
  delay(6000);

  gsmSerial.print("ATD"); gsmSerial.print(PHONE_NUMBER); gsmSerial.println(";");
  delay(15000);
  gsmSerial.println("ATH");
}

// =====================================================
// FLASK ALERT — POST to /api/alert, server fans out to nearby hospitals
// =====================================================
void sendAlertToFlask(const String& reason)
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FLASK] No WiFi, trying reconnect...");
    WiFi.begin(STA_SSID, STA_PASS);
    unsigned long w0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - w0 < 8000) delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FLASK] FAILED — no internet, GSM SMS still sent");
    return;
  }
  if (!gps.location.isValid()) {
    Serial.println("[FLASK] WARNING — no GPS fix, sending 0,0 (server will notify ALL hospitals, location unknown)");
  }

  HTTPClient http;
  http.begin(FLASK_URL);
  http.addHeader("Content-Type", "application/json");

  String body = "{";
  body += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  body += "\"reason\":\"" + reason + "\",";
  body += "\"lat\":" + String(gps.location.isValid() ? gps.location.lat() : 0, 6) + ",";
  body += "\"lon\":" + String(gps.location.isValid() ? gps.location.lng() : 0, 6) + ",";
  body += "\"roll\":" + String(relRoll, 1) + ",";
  body += "\"pitch\":" + String(relPitch, 1);
  body += "}";

  Serial.print("[FLASK] POST "); Serial.println(body);
  int code = http.POST(body);
  Serial.print("[FLASK] HTTP code: "); Serial.println(code);
  if (code > 0) Serial.println(http.getString());
  http.end();
}
