# Fieldline Soil Dashboard
Live 7-in-1 soil sensor dashboard (N, P, K, pH, EC, moisture, temperature), fed by an ESP32 over LAN.

- `index.html` — the dashboard. Real data only — it auto-connects to the node (the page's own host when served by the ESP32, else the last-known address, then `fieldline-node.local`) and shows a "No sensor data" dialog until a node is found. No demo mode, no manual IP entry, no database — live WebSocket/polling only.
- `fieldline_esp32_node.ino` — ESP32 firmware: RS485 Modbus soil probe, serves `GET /data` and `ws://<ip>/ws`.

Note: the hosted (HTTPS) version can only reach the ESP32 if the node is served over HTTPS too or the browser allows insecure private-network requests. For a plain LAN setup, open `index.html` locally.

## Firmware (v1.1)
`firmware/fieldline_node.ino` — ESP32 DevKit. Sensor on RX2/TX2 (GPIO16/17, auto-direction RS485), 20x4 I2C LCD (SDA21/SCL22), LCD reset on D5.
First boot opens Wi-Fi hotspot **Fieldline-Setup** — join it, pick your Wi-Fi. The node then serves the full dashboard at `http://<ip>/` (shown on the LCD) and `http://fieldline-node.local`.
Flashing: download the release zip, plug in USB, double-click `FLASH.bat` (auto-detects the COM port).
Build: arduino-cli, board `esp32:esp32:esp32`, partition `huge_app`, libs ArduinoJson, LiquidCrystal_I2C, WiFiManager, ESPAsyncWebServer (ESP32Async), AsyncTCP. `web_assets.h` is generated from index.html + images.
