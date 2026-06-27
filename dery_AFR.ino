#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ModbusMaster.h>
#include <TM1637Display.h>
#include <Preferences.h>

#define BTN_PIN        2
#define ENCODER_PIN    4
#define ROT1_PIN       25
#define ROT2_PIN       35
#define ROT3_PIN       34
#define MAX485_DE_RE   27
#define MAX485_RX      16
#define MAX485_TX      17
#define TM1637_ATAS_CLK  21
#define TM1637_ATAS_DIO  22
#define TM1637_BAWAH_CLK 33
#define TM1637_BAWAH_DIO 32
#define LED_PIN        23

#define WIFI_SSID     "Biznet"
#define WIFI_PASS     "12345678"
#define MDNS_NAME     "afr"
#define WIFI_TIMEOUT  10000

#define MODBUS_SLAVE  1
#define MODBUS_BAUD   9600
#define REG_CMD       0x2000
#define REG_FREQ      0x2001
#define CMD_RUN       0x0001
#define CMD_STOP      0x0005

#define SP1_DEF  30.0f
#define SP2_DEF  25.0f
#define SP3_DEF  20.0f
#define KP_DEF   0.23f
#define KI_DEF   0.30f
#define KD_DEF   21.4f

// ==================== RPM CONFIG ====================
#define PULSE_PER_REV  5
#define MAX_RPM         100.0f
#define RPM_SPIKE_CEIL (MAX_RPM * 1.5f)
#define RPM_EMA_ALPHA   0.02f
#define RPM_TIMEOUT_US  2000000

// ==================== PID CONFIG ====================
// BUG FIX #4: Integral windup limit disesuaikan.
// Maks output = 5000. Base pada SP=30 sudah ~2500.
// Sisa untuk integral = 2500. Margin ±500 cukup untuk koreksi.
#define PID_INTEGRAL_MAX   100.0f
#define PID_INTEGRAL_MIN  -100.0f

// BUG FIX #1: Deadband — jika |error| < nilai ini, integral BERHENTI akumulasi.
// Mencegah integral terus naik setelah setpoint tercapai.
#define PID_DEADBAND_RPM   0.1f

AsyncWebServer server(80);
ModbusMaster node;
TM1637Display dispAtas(TM1637_ATAS_CLK, TM1637_ATAS_DIO);
TM1637Display dispBawah(TM1637_BAWAH_CLK, TM1637_BAWAH_DIO);

struct SharedState {
  float    current_rpm;
  float    raw_rpm;
  float    setpoint;
  float    sp_pos1, sp_pos2, sp_pos3;
  float    kp, ki, kd;
  float    pid_output;
  bool     modbus_ok;
  uint16_t vfd_freq_raw;
  bool     is_running;
  bool     transition_run;
  uint32_t transition_start_ms;
  uint32_t uptime_ms;
  // BUG FIX #5: Flag untuk PID reset integral saat restart
  bool     pid_reset_req;
};

SharedState g_sys;
SemaphoreHandle_t xMutex;
SemaphoreHandle_t xMutexModbus;

#define SYS_READ(field)  ({ xSemaphoreTake(xMutex,portMAX_DELAY); auto _v=g_sys.field; xSemaphoreGive(xMutex); _v; })
#define SYS_WRITE(f,v)   { xSemaphoreTake(xMutex,portMAX_DELAY); g_sys.f=(v); xSemaphoreGive(xMutex); }

// ==================== ISR DATA ====================
static volatile unsigned long lastPulseMicros = 0;
static volatile unsigned long intervalValid   = 0;
static volatile bool          adaPulsa        = false;

void IRAM_ATTR readRPM() {
  unsigned long now  = micros();
  unsigned long diff = now - lastPulseMicros;
  if (diff > 250000) {
    intervalValid  = diff;
    lastPulseMicros = now;
    adaPulsa       = true;
  }
}

void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH);
}

void postTransmission() {
  Serial2.flush();
  delay(2);
  digitalWrite(MAX485_DE_RE, LOW);
}

uint8_t modbusWriteRegister(uint16_t reg, uint16_t val) {
  xSemaphoreTake(xMutexModbus, portMAX_DELAY);
  uint8_t res = node.writeSingleRegister(reg, val);
  xSemaphoreGive(xMutexModbus);
  return res;
}

void startSystem() {
  Serial.println("[SYS] startSystem");
  uint8_t res = modbusWriteRegister(REG_CMD, CMD_RUN);
  bool mbOk = (res == node.ku8MBSuccess);
  if (mbOk) modbusWriteRegister(REG_FREQ, 1);
  digitalWrite(LED_PIN, HIGH);
  xSemaphoreTake(xMutex, portMAX_DELAY);
  g_sys.is_running          = true;
  g_sys.transition_run      = true;
  g_sys.transition_start_ms = millis();
  g_sys.modbus_ok           = mbOk;
  g_sys.pid_reset_req       = true;   // BUG FIX #5: minta PID reset integral
  xSemaphoreGive(xMutex);
  Serial.printf("[SYS] Modbus RUN: %s\n", mbOk ? "OK" : "FAIL");
}

void stopSystem() {
  Serial.println("[SYS] stopSystem");
  bool mbOk = false;
  for (int i = 0; i < 3; i++) {
    uint8_t r = modbusWriteRegister(REG_CMD, CMD_STOP);
    if (r == node.ku8MBSuccess) { mbOk = true; break; }
    delay(150);
  }
  modbusWriteRegister(REG_FREQ, 0);
  digitalWrite(LED_PIN, LOW);
  xSemaphoreTake(xMutex, portMAX_DELAY);
  g_sys.is_running     = false;
  g_sys.transition_run = false;
  g_sys.modbus_ok      = mbOk;
  g_sys.pid_reset_req  = true;   // BUG FIX #5: reset juga saat stop
  xSemaphoreGive(xMutex);
  Serial.printf("[SYS] Modbus STOP: %s\n", mbOk ? "OK" : "FAIL");
}

void showRPM(float rpm) {
  if (rpm < 0.1f) {
    uint8_t blank[] = {0, 0, 0, 0};
    dispBawah.setSegments(blank);
    return;
  }
  int intPart  = (int)rpm;
  int decDigit = (int)(rpm * 10) % 10;
  int tens  = intPart / 10;
  int units = intPart % 10;
  uint8_t segs[4];
  segs[0] = tens > 0 ? dispBawah.encodeDigit(tens) : 0x00;
  segs[1] = dispBawah.encodeDigit(units) | 0x80;
  segs[2] = dispBawah.encodeDigit(decDigit);
  segs[3] = 0x00;
  dispBawah.setSegments(segs);
}

void updateDisplays() {
  SharedState snap;
  xSemaphoreTake(xMutex, portMAX_DELAY);
  snap = g_sys;
  xSemaphoreGive(xMutex);

  if (snap.transition_run && (millis() - snap.transition_start_ms < 2000)) {
    uint8_t run[] = {0x50, 0x3E, 0x54, 0x00};
    dispAtas.setSegments(run);
    showRPM(snap.current_rpm);
  } else if (snap.is_running) {
    if (snap.transition_run) SYS_WRITE(transition_run, false);
    dispAtas.showNumberDec((int)snap.setpoint, false, 4, 0);
    showRPM(snap.current_rpm);
  } else {
    uint8_t stop[] = {0x6D, 0x78, 0x3F, 0x73};
    dispAtas.setSegments(stop);
    uint8_t d[4];
    d[0] = 0x6D; d[1] = 0x73;
    int sp = (int)snap.setpoint;
    d[2] = dispBawah.encodeDigit(sp / 10);
    d[3] = dispBawah.encodeDigit(sp % 10);
    dispBawah.setSegments(d);
  }
}

// BUG FIX #6: saveNVS pakai mutex agar tidak race condition
void saveNVS() {
  float s1, s2, s3, kp, ki, kd;
  xSemaphoreTake(xMutex, portMAX_DELAY);
  s1 = g_sys.sp_pos1;
  s2 = g_sys.sp_pos2;
  s3 = g_sys.sp_pos3;
  kp = g_sys.kp;
  ki = g_sys.ki;
  kd = g_sys.kd;
  xSemaphoreGive(xMutex);

  Preferences p;
  p.begin("afr", false);
  p.putFloat("sp1",   s1);
  p.putFloat("sp2",   s2);
  p.putFloat("sp3",   s3);
  p.putFloat("pidkp", kp);
  p.putFloat("pidki", ki);
  p.putFloat("pidkd", kd);
  p.end();
  Serial.println("[NVS] saved");
}

// ==================== TASK RPM — EMA + Spike Rejection ====================
void taskRPM(void* pv) {
  float rpm_filtered = 0.0f;
  while (true) {
    bool pulse;
    unsigned long interval;
    unsigned long lastPulse;
    noInterrupts();
    pulse = adaPulsa;
    interval = intervalValid;
    adaPulsa = false;
    lastPulse = lastPulseMicros;
    interrupts();
    unsigned long now = micros();
    if (now - lastPulse > RPM_TIMEOUT_US) {
      rpm_filtered = 0.0f;
    } else if (pulse && interval > 0) {
      float raw = (60000000.0f / interval) / (float)PULSE_PER_REV;
      if (raw > RPM_SPIKE_CEIL) raw = rpm_filtered;
      if (rpm_filtered < 1.0f) {
        rpm_filtered = raw;
      } else {
        rpm_filtered = RPM_EMA_ALPHA * raw + (1.0f - RPM_EMA_ALPHA) * rpm_filtered;
      }
    }
    xSemaphoreTake(xMutex, portMAX_DELAY);
    g_sys.current_rpm = rpm_filtered;
    g_sys.raw_rpm     = (pulse && interval > 0) ? (60000000.0f / interval) / (float)PULSE_PER_REV : 0;
    xSemaphoreGive(xMutex);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==================== TASK PID (DIPERBAIKI) ====================
void taskPID(void* pv) {
  float integral = 0.0f;
  float lastErr  = 0.0f;
  unsigned long lastTime = 0;

  while (true) {
    SharedState snap;
    xSemaphoreTake(xMutex, portMAX_DELAY);
    snap = g_sys;
    xSemaphoreGive(xMutex);

    // BUG FIX #5: Reset integral jika ada permintaan (start/stop)
    if (snap.pid_reset_req) {
      integral  = 0.0f;
      lastErr   = 0.0f;
      lastTime  = 0;
      SYS_WRITE(pid_reset_req, false);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (snap.is_running) {
      unsigned long now = millis();
      float dt = (lastTime == 0) ? 0.1f : (float)(now - lastTime) / 1000.0f;
      if (dt <= 0.0f || dt > 1.0f) dt = 0.1f;  // clamp dt

      float err = snap.setpoint - snap.current_rpm;

      // BUG FIX #1: Deadband — hentikan akumulasi integral jika sudah di dekat setpoint
      // Ini mencegah integral terus naik setelah RPM tercapai.
      if (fabsf(err) >= PID_DEADBAND_RPM) {
        integral += snap.ki * err * dt;
        // BUG FIX #4: Windup guard dengan limit yang masuk akal
        if (integral > PID_INTEGRAL_MAX) integral = PID_INTEGRAL_MAX;
        if (integral < PID_INTEGRAL_MIN) integral = PID_INTEGRAL_MIN;
      }

      float deriv = (err - lastErr) / dt;

      // Rumus Base: 60 RPM = 50Hz (5000). Jadi 1 RPM = 83.33 unit.
      // CATATAN: output modbus TIDAK diubah
      float base = snap.setpoint * (5000.0f / 60.0f);
      float out  = base + (snap.kp * err) + integral + (snap.kd * deriv);

      if (out < 0.0f)    out = 0.0f;
      if (out > 5000.0f) out = 5000.0f;

      uint8_t res = modbusWriteRegister(REG_FREQ, (uint16_t)out);

      xSemaphoreTake(xMutex, portMAX_DELAY);
      g_sys.pid_output   = out;
      g_sys.vfd_freq_raw = (uint16_t)out;
      g_sys.modbus_ok    = (res == node.ku8MBSuccess);
      xSemaphoreGive(xMutex);

      lastErr  = err;
      lastTime = now;
    } else {
      // Motor berhenti: reset semua state PID
      integral = 0.0f;
      lastErr  = 0.0f;
      lastTime = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void taskUI(void* pv) {
  int lastBtn = HIGH;
  bool debouncing = false;
  unsigned long debounceStart = 0;
  unsigned long lastDisp = 0;

  while (true) {
    SYS_WRITE(uptime_ms, millis());

    int btn = digitalRead(BTN_PIN);
    if (btn == LOW && lastBtn == HIGH && !debouncing) {
      debouncing    = true;
      debounceStart = millis();
    }
    lastBtn = btn;
    if (debouncing && (millis() - debounceStart >= 50)) {
      debouncing = false;
      if (digitalRead(BTN_PIN) == LOW) {
        if (!SYS_READ(is_running)) startSystem();
        else stopSystem();
      }
    }

    if (!SYS_READ(is_running)) {
      float sp  = SYS_READ(setpoint);
      bool  chg = false;
      if      (digitalRead(ROT1_PIN) == LOW) { sp = SYS_READ(sp_pos1); chg = true; }
      else if (digitalRead(ROT2_PIN) == LOW) { sp = SYS_READ(sp_pos2); chg = true; }
      else if (digitalRead(ROT3_PIN) == LOW) { sp = SYS_READ(sp_pos3); chg = true; }
      if (chg) SYS_WRITE(setpoint, sp);
    }

    digitalWrite(LED_PIN, SYS_READ(is_running) ? HIGH : LOW);

    if (millis() - lastDisp >= 250) {
      lastDisp = millis();
      updateDisplays();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void handleStatus(AsyncWebServerRequest* r) {
  JsonDocument doc;
  xSemaphoreTake(xMutex, portMAX_DELAY);
  doc["is_running"]   = g_sys.is_running;
  doc["current_rpm"]  = g_sys.current_rpm;
  doc["setpoint"]     = g_sys.setpoint;
  doc["sp_pos1"]      = g_sys.sp_pos1;
  doc["sp_pos2"]      = g_sys.sp_pos2;
  doc["sp_pos3"]      = g_sys.sp_pos3;
  doc["pid_output"]   = g_sys.pid_output;
  doc["vfd_freq_raw"] = g_sys.vfd_freq_raw;
  doc["modbus_ok"]    = g_sys.modbus_ok;
  doc["kp"]           = g_sys.kp;
  doc["ki"]           = g_sys.ki;
  doc["kd"]           = g_sys.kd;
  doc["uptime_ms"]    = g_sys.uptime_ms;
  doc["heap"]         = ESP.getFreeHeap();
  xSemaphoreGive(xMutex);
  String resp;
  serializeJson(doc, resp);
  r->send(200, "application/json", resp);
}

void handleSetpoint(AsyncWebServerRequest* r) {
  JsonDocument doc;
  if (r->hasParam("pos") && r->hasParam("value")) {
    int   pos = r->getParam("pos")->value().toInt();
    float val = r->getParam("value")->value().toFloat();
    if (val < 10.0f)  val = 10.0f;
    if (val > 100.0f) val = 100.0f;
    xSemaphoreTake(xMutex, portMAX_DELAY);
    if      (pos == 1) g_sys.sp_pos1 = val;
    else if (pos == 2) g_sys.sp_pos2 = val;
    else if (pos == 3) g_sys.sp_pos3 = val;
    xSemaphoreGive(xMutex);
    saveNVS();
    doc["status"] = "ok";
    doc["pos"]    = pos;
    doc["value"]  = val;
    String resp;
    serializeJson(doc, resp);
    r->send(200, "application/json", resp);
  } else {
    doc["status"]  = "error";
    doc["message"] = "missing pos or value";
    String resp;
    serializeJson(doc, resp);
    r->send(400, "application/json", resp);
  }
}

void handlePID(AsyncWebServerRequest* r) {
  JsonDocument doc;
  if (r->hasParam("kp") && r->hasParam("ki") && r->hasParam("kd")) {
    xSemaphoreTake(xMutex, portMAX_DELAY);
    g_sys.kp = r->getParam("kp")->value().toFloat();
    g_sys.ki = r->getParam("ki")->value().toFloat();
    g_sys.kd = r->getParam("kd")->value().toFloat();
    xSemaphoreGive(xMutex);
    saveNVS();
    doc["status"] = "ok";
    doc["kp"]     = g_sys.kp;
    doc["ki"]     = g_sys.ki;
    doc["kd"]     = g_sys.kd;
    String resp;
    serializeJson(doc, resp);
    r->send(200, "application/json", resp);
  } else {
    doc["status"]  = "error";
    doc["message"] = "missing kp, ki, or kd";
    String resp;
    serializeJson(doc, resp);
    r->send(400, "application/json", resp);
  }
}

void handleStart(AsyncWebServerRequest* r) {
  JsonDocument doc;
  startSystem();
  doc["status"] = "ok";
  String resp;
  serializeJson(doc, resp);
  r->send(200, "application/json", resp);
}

void handleStop(AsyncWebServerRequest* r) {
  JsonDocument doc;
  stopSystem();
  doc["status"] = "ok";
  String resp;
  serializeJson(doc, resp);
  r->send(200, "application/json", resp);
}

void handleRestart(AsyncWebServerRequest* r) {
  r->send(200, "text/plain", "restarting...");
  delay(100);
  ESP.restart();
}

void setupRoutes() {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.on("/api/status",   HTTP_GET, handleStatus);
  server.on("/api/start",    HTTP_GET, handleStart);
  server.on("/api/stop",     HTTP_GET, handleStop);
  server.on("/api/setpoint", HTTP_GET, handleSetpoint);
  server.on("/api/pid",      HTTP_GET, handlePID);
  server.on("/api/restart",  HTTP_GET, handleRestart);
}

void showDebugSegments(const char* name, const uint8_t* segsTop, const uint8_t* segsBot) {
  if (segsTop) dispAtas.setSegments(segsTop, 4, 0);
  if (segsBot) dispBawah.setSegments(segsBot, 4, 0);
  Serial.printf("[INIT] %s\n", name);
  delay(800);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[INIT] AFR booting...");

  xMutex       = xSemaphoreCreateMutex();
  xMutexModbus = xSemaphoreCreateMutex();

  dispAtas.setBrightness(7);
  dispBawah.setBrightness(7);

  uint8_t dash[]  = {0x40, 0x40, 0x40, 0x40};
  uint8_t blank[] = {0, 0, 0, 0};
  showDebugSegments("GPIO init", dash, dash);

  {
    Preferences p;
    p.begin("afr", true);
    g_sys.sp_pos1 = p.getFloat("sp1",   SP1_DEF);
    g_sys.sp_pos2 = p.getFloat("sp2",   SP2_DEF);
    g_sys.sp_pos3 = p.getFloat("sp3",   SP3_DEF);
    g_sys.kp      = p.getFloat("pidkp", KP_DEF);
    g_sys.ki      = p.getFloat("pidki", KI_DEF);
    g_sys.kd      = p.getFloat("pidkd", KD_DEF);
    p.end();
  }
  g_sys.setpoint          = g_sys.sp_pos2;
  g_sys.is_running        = false;
  g_sys.transition_run    = false;
  g_sys.modbus_ok         = false;
  g_sys.pid_reset_req     = false;   // BUG FIX #5: init flag

  uint8_t nus[] = {0x54, 0x3E, 0x6D, 0x00};
  showDebugSegments("NVS loaded", blank, nus);

  pinMode(BTN_PIN,      INPUT_PULLUP);
  pinMode(ENCODER_PIN,  INPUT_PULLUP);
  pinMode(ROT1_PIN,     INPUT_PULLUP);
  pinMode(ROT2_PIN,     INPUT);
  pinMode(ROT3_PIN,     INPUT);
  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN), readRPM, FALLING);

  Serial2.begin(MODBUS_BAUD, SERIAL_8E1, MAX485_RX, MAX485_TX);
  node.begin(MODBUS_SLAVE, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  uint8_t res = modbusWriteRegister(REG_FREQ, 0);
  g_sys.modbus_ok = (res == node.ku8MBSuccess);
  Serial.printf("[MODBUS] Init test: %s\n", g_sys.modbus_ok ? "OK" : "FAIL");

  uint8_t bus[] = {0x7C, 0x3E, 0x6D, 0x00};
  showDebugSegments("Modbus init", bus, dash);

  if (!LittleFS.begin(true)) {
    Serial.println("[INIT] LittleFS mount failed");
  } else {
    Serial.println("[INIT] LittleFS mounted");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint8_t wifi[] = {0x3E, 0x06, 0x71, 0x06};
  showDebugSegments("WiFi connecting", wifi, dash);

  unsigned long wifiStart = millis();
  bool wifiOk = false;
  while (millis() - wifiStart < WIFI_TIMEOUT) {
    if (WiFi.status() == WL_CONNECTED) { wifiOk = true; break; }
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  uint8_t wifiSeg[] = {0x1C, 0x06, 0x71, 0x06};
  if (wifiOk) {
    Serial.printf("[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
    uint8_t rddy[] = {0x50, 0x5C, 0x6E, 0x00};
    showDebugSegments("WiFi connected", wifiSeg, rddy);
  } else {
    Serial.println("[WIFI] Failed, continuing without network");
    uint8_t fail[] = {0x71, 0x77, 0x38, 0x38};
    showDebugSegments("WiFi failed", wifiSeg, fail);
  }

  if (wifiOk) {
    if (!MDNS.begin(MDNS_NAME)) {
      Serial.println("[INIT] mDNS failed");
    } else {
      Serial.printf("[INIT] mDNS: http://%s.local\n", MDNS_NAME);
    }
  }

  setupRoutes();
  server.begin();
  Serial.println("[INIT] Server started");

  showDebugSegments("Ready", dash, dash);
  delay(500);

  uint8_t stop[] = {0x6D, 0x78, 0x3F, 0x73};
  dispAtas.setSegments(stop);
  int sp = (int)g_sys.setpoint;
  uint8_t spd[4];
  spd[0] = 0x6D; spd[1] = 0x73;
  spd[2] = dispBawah.encodeDigit(sp / 10);
  spd[3] = dispBawah.encodeDigit(sp % 10);
  dispBawah.setSegments(spd);

  xTaskCreatePinnedToCore(taskRPM, "RPM", 2048, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(taskPID, "PID", 4096, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(taskUI,  "UI",  4096, NULL, 3, NULL, 0);

  Serial.println("[INIT] All tasks created, system ready");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}