/*
  Fieldline soil node v1.1 — ESP32 DevKit
  ─────────────────────────────────────────
  Sensor : 7-in-1 RS485 soil probe (N, P, K, pH, EC, moisture, temp)
           via auto-direction TTL↔RS485 module on UART2  → RX2 = GPIO16, TX2 = GPIO17
  LCD    : 20x4 I2C (0x27 or 0x3F auto-detected)         → SDA = GPIO21, SCL = GPIO22
  D5     : LCD reset line (pulsed LOW at boot)            → GPIO5
  Wi-Fi  : first boot opens hotspot "Fieldline-Setup" — join it on your phone,
           pick your Wi-Fi, done. Credentials are stored on the chip.
  Web    : http://<ip>/        full dashboard (served from flash, auto-connects)
           http://<ip>/data    JSON snapshot (CORS *)
           ws://<ip>/ws        JSON pushed every 2 s
           http://fieldline-node.local  (mDNS)
*/
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "web_assets.h"

#define FW_VERSION   "1.1.0"
#define RS485_RX     16
#define RS485_TX     17
#define LCD_RESET    5
#define SENSOR_ADDR  0x01

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
LiquidCrystal_I2C* lcd = nullptr;

float moisture = NAN, soilTemp = NAN, ec = NAN, ph = NAN, n = NAN, p = NAN, k = NAN;
bool  sensorOk = false;
uint32_t sensorFails = 0, lastGood = 0, sensorBaud = 0;
String ipStr = "no wifi";

/* ───────── Modbus RTU ───────── */
uint16_t crc16(const uint8_t* b, size_t len) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < len; i++) { c ^= b[i];
    for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}
bool modbusRead(uint8_t addr, uint16_t reg, uint8_t count, uint16_t* out) {
  uint8_t req[8] = {addr, 0x03, (uint8_t)(reg >> 8), (uint8_t)reg, 0, count, 0, 0};
  uint16_t c = crc16(req, 6); req[6] = c & 0xFF; req[7] = c >> 8;
  while (Serial2.available()) Serial2.read();
  Serial2.write(req, 8); Serial2.flush();
  const size_t want = 5 + count * 2; uint8_t buf[40]; size_t got = 0; uint32_t t0 = millis();
  while (got < want && millis() - t0 < 400) if (Serial2.available()) buf[got++] = Serial2.read();
  if (got < want || buf[0] != addr || buf[1] != 0x03 || buf[2] != count * 2) return false;
  if ((uint16_t)(buf[want - 2] | (buf[want - 1] << 8)) != crc16(buf, want - 2)) return false;
  for (int i = 0; i < count; i++) out[i] = (buf[3 + i * 2] << 8) | buf[4 + i * 2];
  return true;
}
void sensorBegin(uint32_t baud) { Serial2.end(); delay(20); Serial2.begin(baud, SERIAL_8N1, RS485_RX, RS485_TX); sensorBaud = baud; }

bool readSoil() {
  uint16_t r[7];
  if (!modbusRead(SENSOR_ADDR, 0x0000, 7, r)) {
    // some probes answer only at 9600; flip baud after 3 consecutive misses
    if (++sensorFails % 3 == 0) sensorBegin(sensorBaud == 4800 ? 9600 : 4800);
    return false;
  }
  sensorFails = 0; lastGood = millis();
  moisture = r[0] / 10.0f;              // %
  soilTemp = (int16_t)r[1] / 10.0f;     // °C
  ec       = r[2];                      // µS/cm
  ph       = r[3] / 10.0f;              // pH
  n = r[4]; p = r[5]; k = r[6];         // mg/kg
  return true;
}

/* ───────── JSON ───────── */
String snapshot() {
  JsonDocument d;
  auto put = [&](const char* key, float v) { if (isnan(v)) d[key] = nullptr; else d[key] = v; };
  put("n", n); put("p", p); put("k", k); put("ph", ph); put("ec", ec);
  put("moisture", moisture); put("soilTemp", soilTemp);
  d["sensorOk"] = sensorOk; d["sensorBaud"] = sensorBaud;
  d["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  d["uptime"] = millis() / 1000; d["fw"] = FW_VERSION; d["node"] = "FL-02";
  d["ip"] = ipStr;
  String out; serializeJson(d, out); return out;
}

/* ───────── LCD ───────── */
void lcdInit() {
  pinMode(LCD_RESET, OUTPUT); digitalWrite(LCD_RESET, LOW); delay(60); digitalWrite(LCD_RESET, HIGH); delay(60);
  Wire.begin(21, 22);
  for (uint8_t a : {0x27, 0x3F, 0x26, 0x20}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { lcd = new LiquidCrystal_I2C(a, 20, 4); break; }
  }
  if (lcd) { lcd->init(); lcd->backlight(); }
}
void lcdLine(uint8_t row, const char* txt) {
  if (!lcd) return;
  char b[21]; snprintf(b, 21, "%-20.20s", txt);
  lcd->setCursor(0, row); lcd->print(b);
}
void lcdShow(const String& l1, const String& l2) {
  if (!lcd) return;
  lcdLine(0, l1.c_str()); lcdLine(1, l2.c_str()); lcdLine(2, ""); lcdLine(3, "");
}
String fv(float v, uint8_t dec = 0) {      // "--" when no reading
  if (isnan(v)) return "--";
  char b[12]; dtostrf(v, 0, dec, b); return String(b);
}
// One static 20x4 screen: IP / NPK / moisture-EC-temp / pH
void lcdStatus() {
  static uint32_t t = 0;
  if (millis() - t < 2000) return; t = millis();
  char l[24];
  lcdLine(0, ("IP " + ipStr).c_str());
  snprintf(l, 21, "N%s P%s K%s", fv(n).c_str(), fv(p).c_str(), fv(k).c_str());
  lcdLine(1, l);
  snprintf(l, 21, "M%s%% C%s T%sC", fv(moisture).c_str(), fv(ec).c_str(), fv(soilTemp, 1).c_str());
  lcdLine(2, l);
  if (sensorOk) snprintf(l, 21, "pH %s  %ddBm", fv(ph, 2).c_str(), WiFi.isConnected() ? (int)WiFi.RSSI() : 0);
  else          snprintf(l, 21, "pH --  probe offline");
  lcdLine(3, l);
}

/* ───────── Setup ───────── */
void setup() {
  Serial.begin(115200);
  lcdInit();
  lcdShow("Fieldline", "booting...");
  sensorBegin(4800);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager*) { lcdShow("WiFi setup: join", "Fieldline-Setup"); });
  WiFi.setHostname("fieldline-node");
  if (!wm.autoConnect("Fieldline-Setup")) { lcdShow("WiFi failed", "restarting"); delay(2000); ESP.restart(); }
  ipStr = WiFi.localIP().toString();
  Serial.printf("\nFieldline %s  IP %s\n", FW_VERSION, ipStr.c_str());
  lcdShow("WiFi connected", ipStr);
  MDNS.begin("fieldline-node"); MDNS.addService("http", "tcp", 80);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", INDEX_GZ, INDEX_GZ_len);
    res->addHeader("Content-Encoding", "gzip"); res->addHeader("Cache-Control", "no-cache"); r->send(res); });
  server.on("/bg.webp", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "image/webp", BG_WEBP, BG_WEBP_len);
    res->addHeader("Cache-Control", "max-age=604800"); r->send(res); });
  server.on("/crop.webp", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "image/webp", CROP_WEBP, CROP_WEBP_len);
    res->addHeader("Cache-Control", "max-age=604800"); r->send(res); });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "application/json", snapshot()); });
  server.on("/reset-wifi", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Wi-Fi cleared, rebooting into setup"); delay(300); WiFiManager().resetSettings(); ESP.restart(); });
  server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "not found"); });
  server.addHandler(&ws);
  server.begin();
}

/* ───────── Loop ───────── */
uint32_t lastRead = 0, lastPush = 0;
void loop() {
  if (millis() - lastRead > 1000) { lastRead = millis(); sensorOk = readSoil(); if (!sensorOk && millis() - lastGood > 10000) { moisture = soilTemp = ec = ph = n = p = k = NAN; } }
  if (millis() - lastPush > 2000) { lastPush = millis(); ws.cleanupClients(); if (ws.count()) ws.textAll(snapshot()); }
  if (WiFi.status() != WL_CONNECTED) { static uint32_t t = 0; if (millis() - t > 15000) { t = millis(); WiFi.reconnect(); } }
  else ipStr = WiFi.localIP().toString();
  lcdStatus();
}
