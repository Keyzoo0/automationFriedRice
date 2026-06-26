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

AsyncWebServer server(80);
ModbusMaster node;
TM1637Display dispAtas(TM1637_ATAS_CLK, TM1637_ATAS_DIO);
TM1637Display dispBawah(TM1637_BAWAH_CLK, TM1637_BAWAH_DIO);

struct SharedState {
  float  current_rpm;
  float  raw_rpm;
  float  setpoint;
  float  sp_pos1;
  float  sp_pos2;
  float  sp_pos3;
  float  kp, ki, kd;
  float  pid_output;
  bool   modbus_ok;
  uint16_t vfd_freq_raw;
  bool   is_running;
  bool   transition_run;
  uint32_t transition_start_ms;
  uint32_t uptime_ms;
};

SharedState g_sys;
SemaphoreHandle_t xMutex;

#define SYS_READ(field)  ({ xSemaphoreTake(xMutex,portMAX_DELAY); auto _v=g_sys.field; xSemaphoreGive(xMutex); _v; })
#define SYS_WRITE(f,v)   { xSemaphoreTake(xMutex,portMAX_DELAY); g_sys.f=(v); xSemaphoreGive(xMutex); }

volatile unsigned long lastPulseMicros = 0;
volatile unsigned long intervalValid = 0;
volatile bool adaPulsa = false;

void IRAM_ATTR readRPM() {
  unsigned long now = micros();
  unsigned long diff = now - lastPulseMicros;
  if (diff > 300000) {
    intervalValid = diff;
    lastPulseMicros = now;
    adaPulsa = true;
  }
}

void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH);
}

void postTransmission() {
  Serial2.flush();
  delayMicroseconds(200);
  digitalWrite(MAX485_DE_RE, LOW);
}

void startSystem() {
  Serial.println("[SYS] startSystem");
  uint8_t res = node.writeSingleRegister(REG_CMD, CMD_RUN);
  bool mbOk = (res == node.ku8MBSuccess);
  if (mbOk) {
    node.writeSingleRegister(REG_FREQ, 1);
  }
  digitalWrite(LED_PIN, HIGH);
  SYS_WRITE(is_running, true);
  SYS_WRITE(transition_run, true);
  SYS_WRITE(transition_start_ms, millis());
  SYS_WRITE(modbus_ok, mbOk);
  Serial.printf("[SYS] Modbus RUN: %s\n", mbOk ? "OK" : "FAIL");
}

void stopSystem() {
  Serial.println("[SYS] stopSystem");
  bool mbOk = false;
  for (int i = 0; i < 3; i++) {
    uint8_t r = node.writeSingleRegister(REG_CMD, CMD_STOP);
    if (r == node.ku8MBSuccess) { mbOk = true; break; }
    delay(150);
  }
  node.writeSingleRegister(REG_FREQ, 0);
  digitalWrite(LED_PIN, LOW);
  SYS_WRITE(is_running, false);
  SYS_WRITE(transition_run, false);
  SYS_WRITE(modbus_ok, mbOk);
  Serial.printf("[SYS] Modbus STOP: %s\n", mbOk ? "OK" : "FAIL");
}

void showRPM(float rpm) {
  if (rpm < 0.1f) {
    uint8_t blank[] = {0, 0, 0, 0};
    dispBawah.setSegments(blank);
    return;
  }
  int intPart = (int)rpm;
  int decDigit = (int)(rpm * 10) % 10;
  int tens = intPart / 10;
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
    if (snap.transition_run) {
      SYS_WRITE(transition_run, false);
    }
    dispAtas.showNumberDec((int)snap.setpoint, false, 4, 0);
    showRPM(snap.current_rpm);
  } else {
    uint8_t stop[] = {0x6D, 0x78, 0x3F, 0x73};
    dispAtas.setSegments(stop);
    uint8_t d[4];
    d[0] = 0x6D;
    d[1] = 0x73;
    int sp = (int)snap.setpoint;
    d[2] = dispBawah.encodeDigit(sp / 10);
    d[3] = dispBawah.encodeDigit(sp % 10);
    dispBawah.setSegments(d);
  }
}

void saveNVS() {
  Preferences p;
  p.begin("afr", false);
  p.putFloat("sp1", g_sys.sp_pos1);
  p.putFloat("sp2", g_sys.sp_pos2);
  p.putFloat("sp3", g_sys.sp_pos3);
  p.putFloat("pidkp", g_sys.kp);
  p.putFloat("pidki", g_sys.ki);
  p.putFloat("pidkd", g_sys.kd);
  p.end();
  Serial.println("[NVS] saved");
}

void taskRPM(void* pv) {
  float rpmBuf[5] = {0};
  int idx = 0;
  float total = 0;
  int cnt = 0;
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
    if (now - lastPulse > 2000000) {
      total = 0; idx = 0; cnt = 0;
      for (int i = 0; i < 5; i++) rpmBuf[i] = 0;
      SYS_WRITE(current_rpm, 0);
    } else if (pulse && interval > 0) {
      float raw = (60000000.0f / interval) / 5.0f;
      total -= rpmBuf[idx];
      rpmBuf[idx] = raw;
      total += rpmBuf[idx];
      idx = (idx + 1) % 5;
      if (cnt < 5) cnt++;
      float avg = total / cnt;
      xSemaphoreTake(xMutex, portMAX_DELAY);
      g_sys.current_rpm = avg;
      g_sys.raw_rpm = raw;
      xSemaphoreGive(xMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void taskPID(void* pv) {
  float integral = 0;
  float lastErr = 0;
  unsigned long lastTime = 0;
  while (true) {
    SharedState snap;
    xSemaphoreTake(xMutex, portMAX_DELAY);
    snap = g_sys;
    xSemaphoreGive(xMutex);

    if (snap.is_running) {
      unsigned long now = millis();
      float dt = (now - lastTime) / 1000.0f;
      if (lastTime == 0 || dt <= 0) dt = 0.15f;
      float err = snap.setpoint - snap.current_rpm;
      integral += err * dt;
      if (integral > 200.0f) integral = 200.0f;
      if (integral < -200.0f) integral = -200.0f;
      float deriv = (err - lastErr) / dt;
      float base = (66.0f * snap.setpoint) + 530.0f;
      float out = base + (snap.kp * err) + (snap.ki * integral) + (snap.kd * deriv);
      if (out < 0) out = 0;
      if (out > 5000) out = 5000;
      uint8_t res = node.writeSingleRegister(REG_FREQ, (uint16_t)out);
      xSemaphoreTake(xMutex, portMAX_DELAY);
      g_sys.pid_output = out;
      g_sys.vfd_freq_raw = (uint16_t)out;
      g_sys.modbus_ok = (res == node.ku8MBSuccess);
      xSemaphoreGive(xMutex);
      Serial.printf("[PID] SP=%.1f RPM=%.1f err=%.2f out=%.0f mod=%s\n",
        snap.setpoint, snap.current_rpm, err, out, res == node.ku8MBSuccess ? "OK" : "FAIL");
      lastErr = err;
      lastTime = now;
    } else {
      lastTime = 0;
      integral = 0;
      lastErr = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(150));
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
      debouncing = true;
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
      float sp = SYS_READ(setpoint);
      bool chg = false;
      if (digitalRead(ROT1_PIN) == LOW) { sp = SYS_READ(sp_pos1); chg = true; }
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

void taskWebSvr(void* pv) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void handleStatus(AsyncWebServerRequest* r) {
  JsonDocument doc;
  xSemaphoreTake(xMutex, portMAX_DELAY);
  doc["is_running"] = g_sys.is_running;
  doc["current_rpm"] = g_sys.current_rpm;
  doc["setpoint"] = g_sys.setpoint;
  doc["sp_pos1"] = g_sys.sp_pos1;
  doc["sp_pos2"] = g_sys.sp_pos2;
  doc["sp_pos3"] = g_sys.sp_pos3;
  doc["pid_output"] = g_sys.pid_output;
  doc["vfd_freq_raw"] = g_sys.vfd_freq_raw;
  doc["modbus_ok"] = g_sys.modbus_ok;
  doc["kp"] = g_sys.kp;
  doc["ki"] = g_sys.ki;
  doc["kd"] = g_sys.kd;
  doc["uptime_ms"] = g_sys.uptime_ms;
  doc["heap"] = ESP.getFreeHeap();
  xSemaphoreGive(xMutex);
  String resp;
  serializeJson(doc, resp);
  r->send(200, "application/json", resp);
}

void handleStart(AsyncWebServerRequest* r) {
  startSystem();
  r->redirect("/");
}

void handleStop(AsyncWebServerRequest* r) {
  stopSystem();
  r->redirect("/");
}

void handleSetpoint(AsyncWebServerRequest* r) {
  if (r->hasParam("pos") && r->hasParam("value")) {
    int pos = r->getParam("pos")->value().toInt();
    float val = r->getParam("value")->value().toFloat();
    xSemaphoreTake(xMutex, portMAX_DELAY);
    if (pos == 1) g_sys.sp_pos1 = val;
    else if (pos == 2) g_sys.sp_pos2 = val;
    else if (pos == 3) g_sys.sp_pos3 = val;
    xSemaphoreGive(xMutex);
    saveNVS();
  }
  r->redirect("/");
}

void handlePID(AsyncWebServerRequest* r) {
  if (r->hasParam("kp") && r->hasParam("ki") && r->hasParam("kd")) {
    xSemaphoreTake(xMutex, portMAX_DELAY);
    g_sys.kp = r->getParam("kp")->value().toFloat();
    g_sys.ki = r->getParam("ki")->value().toFloat();
    g_sys.kd = r->getParam("kd")->value().toFloat();
    xSemaphoreGive(xMutex);
    saveNVS();
  }
  r->redirect("/");
}

void handleRestart(AsyncWebServerRequest* r) {
  r->send(200, "text/plain", "restarting...");
  delay(100);
  ESP.restart();
}

void setupRoutes() {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/start", HTTP_GET, handleStart);
  server.on("/api/stop", HTTP_GET, handleStop);
  server.on("/api/setpoint", HTTP_GET, handleSetpoint);
  server.on("/api/pid", HTTP_GET, handlePID);
  server.on("/api/restart", HTTP_GET, handleRestart);
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

  xMutex = xSemaphoreCreateMutex();

  dispAtas.setBrightness(7);
  dispBawah.setBrightness(7);

  uint8_t dash[] = {0x40, 0x40, 0x40, 0x40};
  uint8_t blank[] = {0, 0, 0, 0};
  showDebugSegments("GPIO init", dash, dash);

  {
    Preferences p;
    p.begin("afr", true);
    g_sys.sp_pos1 = p.getFloat("sp1", SP1_DEF);
    g_sys.sp_pos2 = p.getFloat("sp2", SP2_DEF);
    g_sys.sp_pos3 = p.getFloat("sp3", SP3_DEF);
    g_sys.kp = p.getFloat("pidkp", KP_DEF);
    g_sys.ki = p.getFloat("pidki", KI_DEF);
    g_sys.kd = p.getFloat("pidkd", KD_DEF);
    p.end();
  }
  g_sys.setpoint = g_sys.sp_pos2;
  g_sys.is_running = false;
  g_sys.transition_run = false;
  g_sys.modbus_ok = false;

  uint8_t nus[] = {0x54, 0x3E, 0x6D, 0x00};
  showDebugSegments("NVS loaded", blank, nus);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(ENCODER_PIN, INPUT_PULLUP);
  pinMode(ROT1_PIN, INPUT_PULLUP);
  pinMode(ROT2_PIN, INPUT);
  pinMode(ROT3_PIN, INPUT);
  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN), readRPM, FALLING);

  Serial2.begin(MODBUS_BAUD, SERIAL_8E1, MAX485_RX, MAX485_TX);
  node.begin(MODBUS_SLAVE, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  uint8_t res = node.writeSingleRegister(REG_FREQ, 0);
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
    if (WiFi.status() == WL_CONNECTED) {
      wifiOk = true;
      break;
    }
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (wifiOk) {
    IPAddress ip = WiFi.localIP();
    Serial.printf("[WIFI] Connected: %s\n", ip.toString().c_str());
    uint8_t ipTop[4], ipBot[4];
    ipTop[0] = dispAtas.encodeDigit(ip[0] / 100);
    ipTop[1] = dispAtas.encodeDigit((ip[0] / 10) % 10);
    ipTop[2] = dispAtas.encodeDigit(ip[0] % 10);
    ipTop[3] = 0x00;
    ipBot[0] = dispBawah.encodeDigit(ip[1] / 100);
    ipBot[1] = dispBawah.encodeDigit((ip[1] / 10) % 10);
    ipBot[2] = dispBawah.encodeDigit(ip[1] % 10);
    ipBot[3] = 0x00;
    dispAtas.setSegments(ipTop);
    dispBawah.setSegments(ipBot);
    delay(1500);
  } else {
    Serial.println("[WIFI] Failed, continuing without network");
    uint8_t errw[] = {0x79, 0x50, 0x50, 0x00};
    showDebugSegments("WiFi failed", errw, wifi);
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
  xTaskCreatePinnedToCore(taskUI, "UI", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(taskWebSvr, "WEB", 2048, NULL, 2, NULL, 0);

  Serial.println("[INIT] All tasks created, system ready");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
