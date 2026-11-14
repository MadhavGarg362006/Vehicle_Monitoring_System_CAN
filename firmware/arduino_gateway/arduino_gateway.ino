/*
  VHMS node: Arduino UNO R4 WiFi  (sensors + CAN node + Adafruit IO gateway)

  Sensors on this board
    DHT20 (I2C)        -> 0x103 ambient temp (x10, degC)   0x104 humidity (x10, %)
    Hall effect (D3)   -> 0x101 engine RPM (x1)
    MQ-2 (A0)          -> 0x108 gas level, raw ADC 0..1023
    HC-SR04            -> 0x109 distance (x10, cm), 400.0 when no echo

  Gateway job
    Frames this board sends AND frames it receives from the STM32 / FRDM-MCXN236
    are collected, and every SNAPSHOT_INTERVAL_MS one message is published to the
    Adafruit IO feed:
      {"t":123,"f":[["0x100","03 24"],["0x101","07 08"], ...]}
    Only frames that arrived since the previous snapshot are included.
    (Adafruit IO free tier: 30 data points/minute, so everything goes in ONE message.)

  CAN: MCP2515 (8 MHz crystal) on SPI, 500 kbit/s, standard 11-bit IDs.

  Libraries: mcp2515 (autowp), ArduinoMqttClient, DHT20 (Rob Tillaart).
  WiFiS3 comes with the UNO R4 board package.
*/

#include <SPI.h>
#include <Wire.h>
#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include <mcp2515.h>
#include <DHT20.h>
#include "config.h"

#if SNAPSHOT_INTERVAL_MS < 2000
#error "SNAPSHOT_INTERVAL_MS below 2000 would exceed the Adafruit IO free rate limit"
#endif

// ---------------- Pins ----------------
const uint8_t PIN_CAN_CS = 10;   // MCP2515 CS  (SCK D13, MOSI D11, MISO D12)
const uint8_t PIN_MQ2    = A0;
const uint8_t PIN_TRIG   = 5;
const uint8_t PIN_ECHO   = 6;
const uint8_t PIN_HALL   = 3;    // interrupt capable

// ---------------- CAN IDs ----------------
const uint16_t ID_RPM   = 0x101;
const uint16_t ID_AMB_T = 0x103;
const uint16_t ID_HUM   = 0x104;
const uint16_t ID_GAS   = 0x108;
const uint16_t ID_DIST  = 0x109;

// IDs that go into the snapshot
const uint16_t KNOWN_IDS[] = {0x100, 0x101, 0x102, 0x103, 0x104, 0x105,
                              0x106, 0x107, 0x108, 0x109, 0x10A, 0x10B};
const uint8_t NUM_IDS = sizeof(KNOWN_IDS) / sizeof(KNOWN_IDS[0]);

struct LatestFrame {
  uint8_t dlc;
  uint8_t data[8];
  bool    fresh;
};
LatestFrame latest[NUM_IDS];

// ---------------- Objects ----------------
MCP2515 mcp2515(PIN_CAN_CS);
DHT20   dht(&Wire);

#if AIO_USE_TLS
WiFiSSLClient netClient;
const uint16_t AIO_PORT = 8883;
#else
WiFiClient netClient;
const uint16_t AIO_PORT = 1883;
#endif
MqttClient mqtt(netClient);

char topicFeed[96];
char topicThrottle[64];
char topicErrors[64];

// ---------------- State ----------------
float ambientC = 0, humidityPct = 0;       // last good DHT20 values

volatile uint32_t hallLastUs   = 0;
volatile uint32_t hallPeriodUs = 0;

unsigned long lastSensorMs   = 0;
unsigned long lastSnapshotMs = 0;
unsigned long lastNetTryMs   = 0;
const unsigned long SENSOR_INTERVAL_MS = 1000;
const unsigned long NET_RETRY_MS       = 10000;   // Adafruit allows 20 connects/minute

// ---------------- Hall (RPM) ----------------
void hallISR() {
  uint32_t now = micros();
  uint32_t dt  = now - hallLastUs;
  if (dt > 2000) {            // 2 ms debounce
    hallPeriodUs = dt;
    hallLastUs   = now;
  }
}

static uint16_t readRpm() {
  noInterrupts();
  uint32_t last   = hallLastUs;
  uint32_t period = hallPeriodUs;
  interrupts();
  if (period == 0) return 0;
  if ((uint32_t)(micros() - last) > 2000000UL) return 0;   // no pulse for 2 s
  uint32_t rpm = 60000000UL / period;                      // 1 pulse per rev
  return rpm > 65535UL ? 65535 : (uint16_t)rpm;
}

// ---------------- Ultrasonic ----------------
static float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 25000UL);
  if (us == 0) return 400.0f;                              // no echo
  float cm = us * 0.0343f / 2.0f;
  return cm > 400.0f ? 400.0f : cm;
}

// ---------------- Encoding ----------------
static uint16_t scale10(float v) {
  if (v < 0) v = 0;
  if (v > 6553.0f) v = 6553.0f;
  return (uint16_t)lroundf(v * 10.0f);
}

// ---------------- Frame collection ----------------
static void recordFrame(uint32_t id, uint8_t dlc, const uint8_t *data) {
  for (uint8_t i = 0; i < NUM_IDS; i++) {
    if (KNOWN_IDS[i] == id) {
      if (dlc > 8) dlc = 8;
      latest[i].dlc = dlc;
      memcpy(latest[i].data, data, dlc);
      latest[i].fresh = true;
      return;
    }
  }
}

// ---------------- Network ----------------
static void onMqttMessage(int size) {
  // Adafruit IO posts rate-limit warnings and errors to these topics
  Serial.print("Adafruit IO [");
  Serial.print(mqtt.messageTopic());
  Serial.print("]: ");
  while (mqtt.available()) Serial.write(mqtt.read());
  Serial.println();
}

static void ensureNetwork() {
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) return;
  if (lastNetTryMs != 0 && millis() - lastNetTryMs < NET_RETRY_MS) return;
  lastNetTryMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("WiFi connecting... ");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);       // blocks up to several seconds
    if (WiFi.status() != WL_CONNECTED) { Serial.println("failed"); return; }
    Serial.println(WiFi.localIP());
  }

  if (!mqtt.connected()) {
    mqtt.setUsernamePassword(AIO_USERNAME, AIO_KEY);
    Serial.print("Adafruit IO connecting... ");
    if (mqtt.connect("io.adafruit.com", AIO_PORT)) {
      Serial.println("connected");
      mqtt.subscribe(topicThrottle);
      mqtt.subscribe(topicErrors);
    } else {
      Serial.print("failed, error ");
      Serial.println(mqtt.connectError());
    }
  }
}

static void publishSnapshot() {
  bool any = false;
  for (uint8_t i = 0; i < NUM_IDS; i++) if (latest[i].fresh) any = true;
  if (!any || !mqtt.connected()) return;

  char buf[480];
  int n = snprintf(buf, sizeof(buf), "{\"t\":%lu,\"f\":[", millis() / 1000UL);
  bool first = true;

  for (uint8_t i = 0; i < NUM_IDS; i++) {
    if (!latest[i].fresh) continue;
    if (n > (int)sizeof(buf) - 48) break;                // never overflow the buffer

    n += snprintf(buf + n, sizeof(buf) - n, "%s[\"0x%03X\",\"",
                  first ? "" : ",", (unsigned)KNOWN_IDS[i]);
    first = false;
    for (uint8_t b = 0; b < latest[i].dlc; b++) {
      n += snprintf(buf + n, sizeof(buf) - n, b ? " %02X" : "%02X", latest[i].data[b]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "\"]");
    latest[i].fresh = false;
  }
  snprintf(buf + n, sizeof(buf) - n, "]}");

  mqtt.beginMessage(topicFeed);                          // QoS 0, not retained
  mqtt.print(buf);
  mqtt.endMessage();

  Serial.print("Published snapshot, ");
  Serial.print(strlen(buf));
  Serial.println(" bytes");
}

// ---------------- CAN ----------------
static void sendU16(uint16_t id, uint16_t raw) {
  struct can_frame f;
  f.can_id  = id;
  f.can_dlc = 2;
  f.data[0] = raw >> 8;
  f.data[1] = raw & 0xFF;

  if (mcp2515.sendMessage(&f) == MCP2515::ERROR_OK) {
    recordFrame(id, 2, f.data);
  } else {
    Serial.print("CAN TX failed for 0x");
    Serial.println(id, HEX);
  }
  delay(2);                                      // let the TX buffer drain
}

static void drainCanRx() {
  struct can_frame rx;
  while (mcp2515.readMessage(&rx) == MCP2515::ERROR_OK) {
    if (rx.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG)) continue;   // standard data frames only
    recordFrame(rx.can_id & CAN_SFF_MASK, rx.can_dlc, rx.data);
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}

  snprintf(topicFeed, sizeof(topicFeed), "%s/feeds/%s", AIO_USERNAME, AIO_FEED);
  snprintf(topicThrottle, sizeof(topicThrottle), "%s/throttle", AIO_USERNAME);
  snprintf(topicErrors, sizeof(topicErrors), "%s/errors", AIO_USERNAME);
  mqtt.onMessage(onMqttMessage);

  analogReadResolution(10);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_HALL, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_HALL), hallISR, FALLING);

  Wire.begin();
  if (!dht.begin()) Serial.println("DHT20 not found: check I2C wiring");

  SPI.begin();
  mcp2515.reset();
  if (mcp2515.setBitrate(CAN_500KBPS, MCP_8MHZ) != MCP2515::ERROR_OK) {
    Serial.println("MCP2515 init failed: check SPI wiring, 5V and CS pin");
    while (true) {}
  }
  mcp2515.setNormalMode();

  Serial.println("VHMS Arduino gateway ready (CAN 500 kbit/s)");
}

void loop() {
  ensureNetwork();
  mqtt.poll();
  drainCanRx();

  if (millis() - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = millis();

    if (dht.read() == DHT20_OK) {
      ambientC    = dht.getTemperature();
      humidityPct = dht.getHumidity();
    }

    uint16_t rpm  = readRpm();
    uint16_t gas  = (uint16_t)analogRead(PIN_MQ2);
    float    dist = readDistanceCm();

    sendU16(ID_AMB_T, scale10(ambientC));
    sendU16(ID_HUM,   scale10(humidityPct));
    sendU16(ID_RPM,   rpm);
    sendU16(ID_GAS,   gas);
    sendU16(ID_DIST,  scale10(dist));

    Serial.print("T=");   Serial.print(ambientC, 1);
    Serial.print(" H=");  Serial.print(humidityPct, 1);
    Serial.print(" RPM=");Serial.print(rpm);
    Serial.print(" MQ2=");Serial.print(gas);
    Serial.print(" D=");  Serial.print(dist, 1);
    Serial.println(" cm");
  }

  if (millis() - lastSnapshotMs >= SNAPSHOT_INTERVAL_MS) {
    lastSnapshotMs = millis();
    drainCanRx();                                // pick up the latest frames first
    publishSnapshot();
  }
}
