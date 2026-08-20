# Fieldline Soil Dashboard
Live 7-in-1 soil sensor dashboard (N, P, K, pH, EC, moisture, temperature) + air temp/humidity, fed by an ESP32 over LAN.

- `index.html` — the dashboard. Open it, click the status pill, enter the ESP32 IP.
- `fieldline_esp32_node.ino` — ESP32 firmware: RS485 Modbus probe + DHT22, serves `GET /data` and `ws://<ip>/ws`.

Note: the hosted (HTTPS) version can only reach the ESP32 if the node is served over HTTPS too or the browser allows insecure private-network requests. For a plain LAN setup, open `index.html` locally.
