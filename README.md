# Vehicle Health Monitoring Dashboard

## Architecture

ESP32 -> WSS WebSocket relay -> GitHub Pages dashboard

The dashboard displays live numeric values and graphs for:
- Ambient temperature
- Vibration
- Battery voltage
- Engine temperature
- Engine RPM
- Battery current
- Vehicle speed
- Humidity

## Important architecture point

GitHub Pages is a static hosting service. It hosts the dashboard files but is not the WebSocket backend.

Use:
- GitHub Pages for `dashboard/index.html`
- Render Web Service for the Node.js WebSocket relay
- ESP32 as a WebSocket client

Render supports public WebSocket connections and recommends `wss://` for public connections.

## Deploy the relay

Repository structure:

vehicle-health-monitor/
  dashboard/index.html
  server/server.js
  server/package.json
  esp32/vehicle_health_websocket.ino

In Render:
1. New -> Web Service
2. Connect the GitHub repository
3. Root Directory: `server`
4. Build Command: `npm install`
5. Start Command: `npm start`
6. Add environment variable:
   `DEVICE_TOKEN = YOUR_LONG_RANDOM_TOKEN`

After deployment, suppose the service is:
`https://vehicle-health-relay.onrender.com`

WebSocket endpoint:
`wss://vehicle-health-relay.onrender.com/ws`

## Configure ESP32

Install `ArduinoWebsockets` by gilmaimon.

Edit the ESP32 sketch:
- WIFI_SSID
- WIFI_PASSWORD
- WS_HOST
- DEVICE_TOKEN in WS_PATH

Example:
`const char* WS_HOST="vehicle-health-relay.onrender.com";`
`const char* WS_PATH="/ws?role=device&token=YOUR_LONG_RANDOM_TOKEN";`

Upload to a normal ESP32.

Serial monitor: 115200.

## Configure GitHub Pages

Edit `dashboard/index.html`:

`const WS_URL="wss://vehicle-health-relay.onrender.com/ws?role=dashboard";`

Push the dashboard folder to GitHub.

Then enable GitHub Pages from repository Settings -> Pages.

## Data format

ESP32 sends:

{
  "type":"sensor",
  "time":12.0,
  "temperature":29.4,
  "vibration":0.31,
  "batteryVoltage":13.2,
  "engineTemp":78.4,
  "rpm":1900,
  "batteryCurrent":3.8,
  "speed":14.2,
  "humidity":61.5
}

The relay broadcasts the latest sensor packet to dashboard clients.

## Current limitation

The ESP32 values are simulated. When real sensors are added, replace only the value-generation code. Keep the JSON field names unchanged so the dashboard does not need to change.

The relay stores only the latest packet in RAM; it is not a historical database.
