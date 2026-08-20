/*
  Zolve soil node — ESP32 + 7-in-1 RS485 soil sensor (N, P, K, pH, EC, moisture, temp)
  + DHT22 for air temp / humidity. Serves JSON over LAN for the Zolve dashboard.

  Endpoints:
    GET  http://<ip>/data   -> JSON snapshot (CORS enabled)
    WS   ws://<ip>/ws       -> pushes the same JSON every 2 s

  Libraries (Arduino Library Manager):
    ESPAsyncWebServer, AsyncTCP, ArduinoJson, DHT sensor library
  Wiring (ESP32 DevKit):
    RS485 module: RO->GPIO16 (RX2), DI->GPIO17 (TX2), DE+RE->GPIO4, VCC 5V, GND
    Sensor: A/B to RS485 A/B, sensor power 12V (or 5V if your model allows), common GND
    DHT22: data->GPIO15 with 10k pull-up, 3.3V, GND
*/
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* HOSTNAME  = "zolve-node";     // reachable as zolve-node.local on most LANs

#define RS485_DE 4
#define DHT_PIN  15
DHT dht(DHT_PIN, DHT22);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Modbus: addr 0x01, func 0x03, start reg 0x0000, 7 regs, CRC
const uint8_t REQ[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};
float moisture, soilTemp, ec, ph, n, p, k, airTemp, humidity;
bool sensorOk = false;

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
  d["n"] = n; d["p"] = p; d["k"] = k; d["ph"] = ph; d["ec"] = ec;
  d["moisture"] = moisture; d["soilTemp"] = soilTemp;
  d["airTemp"] = airTemp; d["humidity"] = humidity;
  d["rssi"] = WiFi.RSSI(); d["uptime"] = millis() / 1000; d["sensorOk"] = sensorOk;
  String out; serializeJson(d, out); return out;
}

void setup() {
  Serial.begin(115200);
  pinMode(RS485_DE, OUTPUT); digitalWrite(RS485_DE, LOW);
  Serial2.begin(4800, SERIAL_8N1, 16, 17);   // most 7-in-1 probes default to 4800 or 9600
  dht.begin();

  WiFi.mode(WIFI_STA); WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "application/json", snapshot()); });
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Zolve node OK. GET /data"); });
  server.addHandler(&ws);
  server.begin();
}

uint32_t lastRead = 0, lastPush = 0;
void loop() {
  if (millis() - lastRead > 1000) {
    lastRead = millis();
    sensorOk = readSoil();
    float h = dht.readHumidity(), t = dht.readTemperature();
    if (!isnan(h)) humidity = h;
    if (!isnan(t)) airTemp = t;
  }
  if (millis() - lastPush > 2000) { lastPush = millis(); ws.cleanupClients(); ws.textAll(snapshot()); }
}
