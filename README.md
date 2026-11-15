# Vehicle Health Monitoring System (CAN)

Three microcontrollers act as CAN nodes on one 500 kbit/s classic CAN bus (11-bit IDs).
The Arduino also works as the gateway: it forwards every CAN frame over MQTT.

```
STM32 Nucleo-F446RE ──┐
FRDM-MCXN236 ─────────┼── CAN bus ── Arduino UNO R4 WiFi ── MQTT (Adafruit IO)
Arduino UNO R4 WiFi ──┘                                         │
                                                      Node server (Render)
                                                       │              │
                                              Neon Postgres     WebSocket
                                                                      │
                                              Dashboard (GitHub Pages, index.html)
```

## Nodes and sensors

| Node | CAN interface | Sensors |
|------|---------------|---------|
| Arduino UNO R4 WiFi | MCP2515 (SPI) | BMP280, MQ-2, HC-SR04 ultrasonic, Hall sensor |
| STM32 Nucleo-F446RE | MCP2515 (SPI) | DHT11, INA219, vibration sensor module |
| NXP FRDM-MCXN236 | onboard FlexCAN1 + TJA1057 | onboard FXLS8974 accelerometer, GPS (NMEA) |

## CAN IDs (big-endian)

| ID | Signal | Encoding |
|----|--------|----------|
| 0x100 | Engine temp (DHT11 stand-in) | uint16, °C x10 |
| 0x101 | RPM (Hall) | uint16 |
| 0x102 | Vibration | uint16, g x100 |
| 0x103 | Ambient temp (BMP280) | uint16, °C x10 |
| 0x104 | Pressure (BMP280) | uint16, hPa x10 |
| 0x105 | Speed (GPS) | uint16, km/h x10 |
| 0x106 | Battery voltage (INA219) | uint16, V x10 |
| 0x107 | Battery current (INA219) | uint16, A x10 |
| 0x108 | Gas (MQ-2) | uint16, raw ADC |
| 0x109 | Distance (HC-SR04) | uint16, cm x10 (4000 = no echo) |
| 0x10A | Acceleration | 3 x int16, milli-g (X, Y, Z) |
| 0x10B | GPS position | 2 x int32, 1e-6 degree (lat, lon) |

The NXP node sends 0x105 and 0x10B only while the GPS has a fix. The dashboard shows
"GPS: CONNECTING…" until then.

## Repository layout

- `index.html` dashboard (GitHub Pages). Has a live GPS status line and a DOWNLOAD CSV button.
- `server.js`, `canDecoder.js` Node server: MQTT subscriber, CAN decoder, Neon storage, WebSocket relay.
- `test_decoder.js` run `node test_decoder.js` (prints "all tests passed").
- `firmware/arduino_gateway` Arduino sketch (copy `config.example.h` to `config.h` and fill it in; `config.h` is git-ignored).
- `firmware/stm32_node` STM32 sketch (Arduino IDE, STM32duino).
- `firmware/nxp_frdm_mcxn236` MCUXpresso (SDK flexcan interrupt_transfer example) firmware and guide.
- `CAN.py`, `Hcl(python).py` simulators / offline demos.

## Adafruit IO

Free accounts allow 30 data points per minute. The gateway therefore publishes one JSON
snapshot of all frames every 3 s (20 per minute) to a single feed, `vhms-can`:
`{"t":123,"f":[["0x100","03 24"],["0x101","07 08"]]}`. The server decodes it.

## Server environment (`.env.local`, never commit it)

```
MQTT_BROKER=mqtts://io.adafruit.com:8883
MQTT_USERNAME=<Adafruit IO username>
MQTT_PASSWORD=<Adafruit IO key>
MQTT_TOPIC=<username>/feeds/vhms-can
DATABASE_URL=<Neon connection string>
```

Run locally with `npm install` then `node server.js`. On Render use the same values as
environment variables. The dashboard connects to the server over `wss://…/ws?role=dashboard`.
