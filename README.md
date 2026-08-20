# Fieldline Soil Dashboard
Live 7-in-1 soil sensor dashboard (N, P, K, pH, EC, moisture, temperature) + air temp/humidity, fed by an ESP32 over LAN.

- `index.html` — the dashboard. Open it, click the status pill, enter the ESP32 IP.
- `fieldline_esp32_node.ino` — ESP32 firmware: RS485 Modbus probe + DHT22, serves `GET /data` and `ws://<ip>/ws`.

## ESP32 wiring and flashing

- RS485 converter: `RO -> RX2/GPIO16`, `DI -> TX2/GPIO17`, `DE+RE -> GPIO4`.
- I2C LCD (16x2, address 0x27 or 0x3F): `SDA -> GPIO21`, `SCL -> GPIO22`.
- Wi-Fi reset button: `GPIO5 -> button -> GND`. Hold for 3 seconds to erase the saved Wi-Fi network and restart.
- DHT22 (optional): `DATA -> GPIO15`, with a 10k pull-up to 3.3V.

The soil sensor must use UART2 (GPIO16/17), not the USB/programming UART0 pins
GPIO1/GPIO3. Copy `secrets.example.h` to the git-ignored `secrets.h` and add the
farm Wi-Fi credentials. If those credentials are missing or cannot connect, the
ESP32 creates an open `FIELDLINE-SETUP` access point as a fallback. The LCD shows
the node IP after connection.

The dashboard connects directly to `fieldline-node.local` over the LAN and uses
WebSocket updates from `ws://fieldline-node.local/ws`, falling back to `GET /data`.
There is no database or cloud realtime dependency in this path.

Build, upload, and monitor with:

```powershell
py -m platformio run
py -m platformio run --target upload
py -m platformio device monitor
```

Note: the hosted (HTTPS) version can only reach the ESP32 if the node is served over HTTPS too or the browser allows insecure private-network requests. For a plain LAN setup, open `index.html` locally.
