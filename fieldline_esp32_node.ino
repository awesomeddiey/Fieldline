/*
  Fieldline soil node — ESP32 + 7-in-1 RS485 soil sensor (N, P, K, pH, EC, moisture, temp)
  + DHT22 for air temp / humidity. Serves JSON over LAN for the Fieldline dashboard.

  Endpoints:
    GET  http://<ip>/data   -> JSON snapshot (CORS enabled)
    WS   ws://<ip>/ws       -> pushes the same JSON every 2 s

  Libraries are pinned in platformio.ini.
  Wiring (ESP32 DevKit):
    RS485 module: RO->GPIO16 (RX2), DI->GPIO17 (TX2), DE+RE->GPIO4, VCC 5V, GND
    Sensor: A/B to RS485 A/B, sensor power 12V (or 5V if your model allows), common GND
    DHT22: data->GPIO15 with 10k pull-up, 3.3V, GND
    I2C LCD: SDA->GPIO21, SCL->GPIO22, VCC and GND (0x27 or 0x3F)
    Reset button: GPIO5 to GND; hold for 3 seconds to clear Wi-Fi and restart

  The sensor uses UART2 rather than the USB/programming UART0. Do not connect the
  RS485 module to GPIO1/GPIO3, because that can prevent flashing and serial logs.
*/
#include <WiFi.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define FIELDLINE_WIFI_SSID ""
#define FIELDLINE_WIFI_PASSWORD ""
#endif

const char* HOSTNAME  = "fieldline-node";     // reachable as fieldline-node.local on most LANs
const char* SETUP_AP  = "FIELDLINE-SETUP";

#define RS485_RX  16
#define RS485_TX  17
#define RS485_DE  4
#define RESET_PIN 5
#define DHT_PIN   15
#define I2C_SDA   21
#define I2C_SCL   22

DHT dht(DHT_PIN, DHT22);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
Preferences preferences;
LiquidCrystal_I2C* lcd = nullptr;
bool configPortalActive = false;
uint32_t restartAt = 0;

// Modbus: addr 0x01, func 0x03, start reg 0x0000, 7 regs, CRC
const uint8_t REQ[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};
float moisture, soilTemp, ec, ph, n, p, k, airTemp, humidity;
bool sensorOk = false;
bool airTempOk = false;
bool humidityOk = false;

void lcdLine(uint8_t row, const String& message) {
  if (!lcd) return;
  String text = message.substring(0, 16);
  while (text.length() < 16) text += ' ';
  lcd->setCursor(0, row);
  lcd->print(text);
}

void lcdMessage(const String& top, const String& bottom) {
  lcdLine(0, top);
  lcdLine(1, bottom);
}

void beginLcd() {
  Wire.begin(I2C_SDA, I2C_SCL);
  uint8_t address = 0;
  const uint8_t candidates[] = {0x27, 0x3F};
  for (uint8_t candidate : candidates) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) {
      address = candidate;
      break;
    }
  }
  if (!address) {
    Serial.println("LCD not found at I2C address 0x27 or 0x3F");
    return;
  }
  lcd = new LiquidCrystal_I2C(address, 16, 2);
  lcd->init();
  lcd->backlight();
  lcdMessage("FIELDLINE", "Starting...");
  Serial.printf("LCD found at 0x%02X\n", address);
}

void clearWifiIfHeldAtBoot() {
  if (digitalRead(RESET_PIN) != LOW) return;
  lcdMessage("Hold to reset", "WiFi settings");
  uint32_t started = millis();
  while (digitalRead(RESET_PIN) == LOW && millis() - started < 3000) delay(10);
  if (millis() - started >= 3000) {
    lcdMessage("WiFi cleared", "Restarting...");
    preferences.clear();
    delay(800);
    ESP.restart();
  }
}

bool connectWifi() {
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  if (ssid.isEmpty() && strlen(FIELDLINE_WIFI_SSID)) {
    ssid = FIELDLINE_WIFI_SSID;
    password = FIELDLINE_WIFI_PASSWORD;
  }
  if (ssid.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  lcdMessage("Connecting WiFi", ssid);
  Serial.printf("Connecting to Wi-Fi: %s", ssid.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void startConfigPortal() {
  configPortalActive = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP);
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    const char* page =
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<title>Fieldline Wi-Fi setup</title>"
      "<style>body{font:18px system-ui;max-width:420px;margin:40px auto;padding:20px}"
      "input,button{box-sizing:border-box;width:100%;padding:12px;margin:6px 0}"
      "button{background:#15803d;color:white;border:0;border-radius:6px}</style>"
      "<h1>Fieldline setup</h1><p>Enter the farm Wi-Fi details.</p>"
      "<form method=post action=/save><label>Wi-Fi name</label>"
      "<input name=ssid required maxlength=32><label>Password</label>"
      "<input name=password type=password maxlength=64>"
      "<button type=submit>Save and connect</button></form>";
    request->send(200, "text/html", page);
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", "Wi-Fi name is required");
      return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String password = request->hasParam("password", true)
                        ? request->getParam("password", true)->value() : "";
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    request->send(200, "text/html",
                  "<h1>Saved</h1><p>Fieldline is restarting and connecting...</p>");
    lcdMessage("WiFi saved", "Restarting...");
    restartAt = millis() + 1500;
  });
  server.onNotFound([](AsyncWebServerRequest* request) { request->redirect("/"); });
  server.begin();
  Serial.printf("Wi-Fi setup AP: %s, portal: http://%s/\n", SETUP_AP,
                WiFi.softAPIP().toString().c_str());
  lcdMessage("WiFi setup", SETUP_AP);
}

uint16_t crc16(const uint8_t* b, size_t len) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < len; i++) { c ^= b[i];
    for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}

bool readSoil() {
  while (Serial2.available()) Serial2.read();
  digitalWrite(RS485_DE, HIGH); delayMicroseconds(50);
  Serial2.write(REQ, sizeof(REQ)); Serial2.flush();
  digitalWrite(RS485_DE, LOW);
  uint8_t buf[19]; size_t got = 0; uint32_t t0 = millis();
  while (got < 19 && millis() - t0 < 300) if (Serial2.available()) buf[got++] = Serial2.read();
  if (got < 19 || buf[0] != 0x01 || buf[1] != 0x03 || buf[2] != 14) return false;
  uint16_t crc = buf[17] | (buf[18] << 8);
  if (crc != crc16(buf, 17)) return false;
  auto reg = [&](int i) { return (uint16_t)((buf[3 + i * 2] << 8) | buf[4 + i * 2]); };
  moisture = reg(0) / 10.0;             // %
  soilTemp = (int16_t)reg(1) / 10.0;    // °C
  ec       = reg(2);                    // µS/cm
  ph       = reg(3) / 10.0;             // pH
  n        = reg(4);                    // mg/kg
  p        = reg(5);
  k        = reg(6);
  return true;
}

String snapshot() {
  StaticJsonDocument<320> d;
  if (sensorOk) {
    d["n"] = n; d["p"] = p; d["k"] = k; d["ph"] = ph; d["ec"] = ec;
    d["moisture"] = moisture; d["soilTemp"] = soilTemp;
  } else {
    d["n"] = nullptr; d["p"] = nullptr; d["k"] = nullptr;
    d["ph"] = nullptr; d["ec"] = nullptr;
    d["moisture"] = nullptr; d["soilTemp"] = nullptr;
  }
  if (airTempOk) d["airTemp"] = airTemp; else d["airTemp"] = nullptr;
  if (humidityOk) d["humidity"] = humidity; else d["humidity"] = nullptr;
  d["rssi"] = WiFi.RSSI(); d["uptime"] = millis() / 1000; d["sensorOk"] = sensorOk;
  String out; serializeJson(d, out); return out;
}

void updateLcd() {
  if (!lcd) return;
  if (configPortalActive) {
    lcdMessage("WiFi setup", SETUP_AP);
    return;
  }
  char top[17], bottom[17];
  static uint8_t page = 0;
  if (!sensorOk) {
    snprintf(top, sizeof(top), "Sensor: NO DATA");
    snprintf(bottom, sizeof(bottom), "IP %s", WiFi.localIP().toString().c_str());
  } else if (page == 0) {
    snprintf(top, sizeof(top), "M:%3.0f%% pH:%3.1f", moisture, ph);
    snprintf(bottom, sizeof(bottom), "T:%3.1f EC:%4.0f", soilTemp, ec);
  } else {
    snprintf(top, sizeof(top), "N:%3.0f P:%3.0f", n, p);
    snprintf(bottom, sizeof(bottom), "K:%3.0f WiFi:%ld", k, WiFi.RSSI());
  }
  lcdMessage(top, bottom);
  page = (page + 1) % 2;
}

void handleResetButton() {
  static uint32_t pressedAt = 0;
  static bool handled = false;
  if (digitalRead(RESET_PIN) == LOW) {
    if (!pressedAt) pressedAt = millis();
    if (!handled && millis() - pressedAt >= 3000) {
      handled = true;
      lcdMessage("WiFi cleared", "Restarting...");
      Serial.println("Reset button held: clearing Wi-Fi settings");
      preferences.clear();
      delay(800);
      ESP.restart();
    }
  } else {
    pressedAt = 0;
    handled = false;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);
  beginLcd();
  preferences.begin("fieldline", false);
  clearWifiIfHeldAtBoot();
  pinMode(RS485_DE, OUTPUT); digitalWrite(RS485_DE, LOW);
  Serial2.begin(4800, SERIAL_8N1, RS485_RX, RS485_TX); // many 7-in-1 probes default to 4800
  dht.begin();

  if (!connectWifi()) {
    startConfigPortal();
    return;
  }
  Serial.printf("Wi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  lcdMessage("WiFi connected", WiFi.localIP().toString());
  if (MDNS.begin(HOSTNAME)) Serial.printf("mDNS: http://%s.local/\n", HOSTNAME);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Private-Network", "true");
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "application/json", snapshot()); });
  server.addHandler(&ws);
  if (LittleFS.begin(false)) {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    Serial.println("Dashboard filesystem mounted");
  } else {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
      r->send(503, "text/plain", "Dashboard files missing. Upload the LittleFS image.");
    });
    Serial.println("Dashboard filesystem missing");
  }
  server.begin();
}

uint32_t lastRead = 0, lastPush = 0;
void loop() {
  if (configPortalActive) dnsServer.processNextRequest();
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
  handleResetButton();
  if (millis() - lastRead > 1000) {
    lastRead = millis();
    sensorOk = readSoil();
    float h = dht.readHumidity(), t = dht.readTemperature();
    humidityOk = !isnan(h);
    airTempOk = !isnan(t);
    if (humidityOk) humidity = h;
    if (airTempOk) airTemp = t;
  }
  if (millis() - lastPush > 2000) {
    lastPush = millis();
    ws.cleanupClients();
    ws.textAll(snapshot());
    updateLcd();
  }
}
