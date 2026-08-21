# Fieldline Soil Dashboard
Live 7-in-1 soil sensor dashboard (N, P, K, pH, EC, moisture, temperature) + air temp/humidity, fed by an ESP32 over LAN.

- `index.html` — the dashboard. Open it, click the status pill, enter the ESP32 IP.
- `fieldline_esp32_node.ino` — ESP32 firmware: RS485 Modbus probe + DHT22, serves `GET /data` and `ws://<ip>/ws`.

Note: the hosted (HTTPS) version can only reach the ESP32 if the node is served over HTTPS too or the browser allows insecure private-network requests. For a plain LAN setup, open `index.html` locally.

## Firmware (v1.1)
`firmware/fieldline_node.ino` — ESP32 DevKit. Sensor on RX2/TX2 (GPIO16/17, auto-direction RS485), 16x2 I2C LCD (SDA21/SCL22), LCD reset on D5.
First boot opens Wi-Fi hotspot **Fieldline-Setup** — join it, pick your Wi-Fi. The node then serves the full dashboard at `http://<ip>/` (shown on the LCD) and `http://fieldline-node.local`.
Flashing: download the release zip, plug in USB, double-click `FLASH.bat` (auto-detects the COM port).
Build: arduino-cli, board `esp32:esp32:esp32`, partition `huge_app`, libs ArduinoJson, LiquidCrystal_I2C, WiFiManager, ESPAsyncWebServer (ESP32Async), AsyncTCP. `web_assets.h` is generated from index.html + images.
