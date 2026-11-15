/*
  VHMS node: STM32 Nucleo-F446RE  (CAN node over MCP2515)

  Sensors on this board
    DHT11 (D2)          -> 0x100 "engine temperature" stand-in (x10, degC)
    Vibration sensor    -> 0x102 vibration (x100, g)  (relative index, see below)
    INA219 (I2C)        -> 0x106 battery voltage (x10, V)
                           0x107 battery current (x10, A, absolute value)

  CAN: MCP2515 (8 MHz crystal) on SPI, 500 kbit/s, standard 11-bit IDs.
  Same wiring as the working test: SCK D13, MOSI D11, MISO D12 (via divider), CS D10.

  Everything on this board is 3.3 V logic. Power the sensors from 3.3 V.

  Libraries: mcp2515 (autowp), DHT sensor library + Adafruit Unified Sensor,
             Adafruit INA219 + Adafruit BusIO.
*/

#include <SPI.h>
#include <Wire.h>
#include <mcp2515.h>
#include <DHT.h>
#include <Adafruit_INA219.h>

// ---------------- Vibration sensor type ----------------
// 0 = digital output (DO) on D3  -> default, for 3-pin comparator modules (SW-420 type,
//     e.g. the one marked HW V0.2: VCC, GND, DO). Turn the blue potentiometer until the
//     module's LED just stays off when it is still, then it flips when it is shaken.
// 1 = analog output (AO) on A0   -> only if your module really has an AO pin.
#define VIB_ANALOG 0

// Calibration (the module is not a calibrated accelerometer):
//   analog : RMS volts of the AC part that counts as 1.00 g
//   digital: edges per second that count as 1.00 g
const float VIB_VOLTS_PER_G  = 1.0f;
const float VIB_EDGES_PER_G  = 100.0f;

// ---------------- Pins ----------------
const uint8_t PIN_CAN_CS = 10;
const uint8_t PIN_DHT    = 2;      // D2
const uint8_t PIN_VIB_A  = A0;
const uint8_t PIN_VIB_D  = 3;      // D3
// INA219 uses the default I2C pins: SDA D14 (PB9), SCL D15 (PB8)

// ---------------- CAN IDs ----------------
const uint16_t ID_ENGINE_T = 0x100;
const uint16_t ID_VIB      = 0x102;
const uint16_t ID_BATT_V   = 0x106;
const uint16_t ID_BATT_I   = 0x107;

// ---------------- Objects ----------------
MCP2515 mcp2515(PIN_CAN_CS);
DHT dht(PIN_DHT, DHT11);
Adafruit_INA219 ina219;

bool inaOk = false;
float engineTempC = NAN;                 // last good DHT11 value

volatile uint32_t vibEdges = 0;
void vibISR() { vibEdges++; }

unsigned long lastSendMs = 0;
unsigned long lastDhtMs  = 0;
const unsigned long SEND_INTERVAL_MS = 1000;
const unsigned long DHT_INTERVAL_MS  = 2000;

// ---------------- Helpers ----------------
static uint16_t scaleU16(float v, float scale) {
  float x = v * scale;
  if (x < 0) x = 0;
  if (x > 65535.0f) x = 65535.0f;
  return (uint16_t)lroundf(x);
}

static void sendU16(uint16_t id, uint16_t raw) {
  struct can_frame f;
  f.can_id  = id;
  f.can_dlc = 2;
  f.data[0] = raw >> 8;
  f.data[1] = raw & 0xFF;
  if (mcp2515.sendMessage(&f) != MCP2515::ERROR_OK) {
    Serial.print("CAN TX failed for 0x");
    Serial.println(id, HEX);
  }
  delay(8);                               // let the TX buffer drain and give the gateway time to read each frame
}

static float readVibrationG() {
#if VIB_ANALOG
  // ~100 ms window, ~1 kHz sampling: RMS of the AC component
  const int N = 100;
  float sum = 0, sumSq = 0;
  for (int i = 0; i < N; i++) {
    float v = analogRead(PIN_VIB_A) * 3.3f / 4095.0f;
    sum += v;
    sumSq += v * v;
    delayMicroseconds(900);
  }
  float mean = sum / N;
  float var  = sumSq / N - mean * mean;
  if (var < 0) var = 0;
  return sqrtf(var) / VIB_VOLTS_PER_G;
#else
  // edges counted by the interrupt since the last call
  static uint32_t lastEdges = 0;
  static unsigned long lastMs = 0;
  uint32_t e = vibEdges;
  unsigned long now = millis();
  float perSec = 0;
  if (lastMs != 0 && now > lastMs) perSec = (e - lastEdges) * 1000.0f / (now - lastMs);
  lastEdges = e;
  lastMs = now;
  return perSec / VIB_EDGES_PER_G;
#endif
}

static void drainCanRx() {
  struct can_frame rx;
  while (mcp2515.readMessage(&rx) == MCP2515::ERROR_OK) { /* not needed on this node */ }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);

  dht.begin();

  Wire.begin();
  inaOk = ina219.begin();
  if (!inaOk) Serial.println("INA219 not found: check I2C wiring (SDA D14, SCL D15)");

#if !VIB_ANALOG
  pinMode(PIN_VIB_D, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_VIB_D), vibISR, CHANGE);
#endif

  SPI.begin();
  mcp2515.reset();
  if (mcp2515.setBitrate(CAN_500KBPS, MCP_8MHZ) != MCP2515::ERROR_OK) {
    Serial.println("MCP2515 init failed: check SPI wiring, 5V, CS and the SO divider");
    while (true) {}
  }
  mcp2515.setNormalMode();

  Serial.println("VHMS STM32 node ready (CAN 500 kbit/s)");
}

void loop() {
  drainCanRx();

  if (millis() - lastDhtMs >= DHT_INTERVAL_MS) {
    lastDhtMs = millis();
    float t = dht.readTemperature();
    if (!isnan(t)) engineTempC = t;
  }

  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();

    float vibG = readVibrationG();

    if (!isnan(engineTempC)) sendU16(ID_ENGINE_T, scaleU16(engineTempC, 10.0f));
    sendU16(ID_VIB, scaleU16(vibG, 100.0f));

    float battV = NAN, battA = NAN;
    if (inaOk) {
      float busV   = ina219.getBusVoltage_V();
      float shuntV = ina219.getShuntVoltage_mV() / 1000.0f;
      battV = busV + shuntV;                                   // supply side of the shunt
      battA = fabsf(ina219.getCurrent_mA()) / 1000.0f;
      sendU16(ID_BATT_V, scaleU16(battV, 10.0f));
      sendU16(ID_BATT_I, scaleU16(battA, 10.0f));
    }

    Serial.print("Eng=");  Serial.print(engineTempC, 1);
    Serial.print(" Vib="); Serial.print(vibG, 2);
    Serial.print(" V=");   Serial.print(battV, 2);
    Serial.print(" I=");   Serial.println(battA, 3);
  }
}
