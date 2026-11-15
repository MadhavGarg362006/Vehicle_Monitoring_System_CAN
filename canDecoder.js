"use strict";

// ============================================================
// CAN FRAME DECODER (table driven)
//
// Frame format coming from MQTT:
//   { "canId": "0x100", "dlc": 2, "data": "03 24" }
// All multi-byte values are big-endian.
// ============================================================

// Latest decoded values (sent to the dashboard)
const vehicleData = {
    temperature: 0.0,       // ambient, 0x103
    vibration: 0.0,         // 0x102
    batteryVoltage: 0.0,    // 0x106
    engineTemp: 0.0,        // 0x100
    rpm: 0,                 // 0x101
    batteryCurrent: 0.0,    // 0x107
    speed: 0.0,             // 0x105
    humidity: 0.0,          // 0x104
    gas: 0,                 // 0x108
    distance: 0.0,          // 0x109
    accelX: 0.0,            // 0x10A
    accelY: 0.0,
    accelZ: 0.0,
    latitude: 0.0,          // 0x10B
    longitude: 0.0
};

// Last frame seen per CAN ID
const canMessages = {};

// When each CAN ID was last received (ms since epoch)
const lastSeen = {};

const u16 = (b, o = 0) => ((b[o] << 8) | b[o + 1]);
const i16 = (b, o = 0) => (((b[o] << 8) | b[o + 1]) << 16) >> 16;
const i32 = (b, o = 0) =>
    ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) | 0;

const sig = (key, signal, value, unit) => ({ key, signal, value, unit });

// ecu = which board sends this ID, minBytes = shortest valid payload
const FRAMES = {
    // ---- STM32 Nucleo-F446RE ----
    "0x100": { ecu: "STM32", minBytes: 2, decode: (b) => [
        sig("engineTemp", "Engine Temp (DHT11)", u16(b) / 10, "°C")] },
    "0x102": { ecu: "STM32", minBytes: 2, decode: (b) => [
        sig("vibration", "Vibration (sensor module)", u16(b) / 100, "g")] },
    "0x106": { ecu: "STM32", minBytes: 2, decode: (b) => [
        sig("batteryVoltage", "Battery Voltage (INA219)", u16(b) / 10, "V")] },
    "0x107": { ecu: "STM32", minBytes: 2, decode: (b) => [
        sig("batteryCurrent", "Battery Current (INA219)", u16(b) / 10, "A")] },

    // ---- Arduino UNO R4 WiFi ----
    "0x101": { ecu: "Arduino", minBytes: 2, decode: (b) => [
        sig("rpm", "Engine RPM (Hall)", u16(b), "RPM")] },
    "0x103": { ecu: "Arduino", minBytes: 2, decode: (b) => [
        sig("temperature", "Ambient Temp (DHT20)", u16(b) / 10, "°C")] },
    "0x104": { ecu: "Arduino", minBytes: 2, decode: (b) => [
        sig("humidity", "Humidity (DHT20)", u16(b) / 10, "%")] },
    "0x108": { ecu: "Arduino", minBytes: 2, decode: (b) => [
        sig("gas", "Gas Level (MQ-2)", u16(b), "ADC")] },
    "0x109": { ecu: "Arduino", minBytes: 2, decode: (b) => [
        sig("distance", "Distance (HC-SR04)", u16(b) / 10, "cm")] },

    // ---- NXP FRDM-MCXN236 ----
    "0x105": { ecu: "MCXN236", minBytes: 2, decode: (b) => [
        sig("speed", "Speed (GPS)", u16(b) / 10, "km/h")] },
    // 3 x int16, milli-g
    "0x10A": { ecu: "MCXN236", minBytes: 6, decode: (b) => [
        sig("accelX", "Acceleration X (FXLS8974)", i16(b, 0) / 1000, "g"),
        sig("accelY", "Acceleration Y (FXLS8974)", i16(b, 2) / 1000, "g"),
        sig("accelZ", "Acceleration Z (FXLS8974)", i16(b, 4) / 1000, "g")] },
    // 2 x int32, 1e-6 degree
    "0x10B": { ecu: "MCXN236", minBytes: 8, decode: (b) => [
        sig("latitude", "Latitude (GPS)", i32(b, 0) / 1e6, "°"),
        sig("longitude", "Longitude (GPS)", i32(b, 4) / 1e6, "°")] }
};

function normalizeCanId(raw) {
    const n = parseInt(String(raw), 16);
    if (!Number.isFinite(n)) return null;
    return "0x" + n.toString(16).toUpperCase().padStart(3, "0");
}

function getNodeFromCANId(canId) {
    const entry = FRAMES[normalizeCanId(canId)];
    return entry ? entry.ecu : "Unknown";
}

// Returns { canId, ecu, signals: [...] } or null
function decodeCANMessage(message) {
    if (!message || !message.canId || !message.data) {
        console.log("[MQTT] Invalid CAN message");
        return null;
    }

    const canId = normalizeCanId(message.canId);
    const entry = canId && FRAMES[canId];

    if (!entry) {
        console.log("[MQTT] Unknown CAN ID:", message.canId);
        return null;
    }

    const bytes = String(message.data)
        .trim()
        .split(/\s+/)
        .map((s) => parseInt(s, 16));

    if (bytes.some((x) => !Number.isFinite(x) || x < 0 || x > 255)) {
        console.log("[MQTT] Bad data bytes:", message.data);
        return null;
    }

    if (bytes.length < entry.minBytes) {
        console.log(`[MQTT] ${canId}: expected ${entry.minBytes} bytes, got ${bytes.length}`);
        return null;
    }

    const signals = entry.decode(bytes);
    lastSeen[canId] = Date.now();

    for (const s of signals) {
        vehicleData[s.key] = s.value;
    }

    // "signal/value/unit" keep the old single-signal shape for the dashboard;
    // "signals" has everything for multi-signal frames.
    canMessages[canId] = {
        signal: signals[0].signal,
        value: signals[0].value,
        unit: signals[0].unit,
        signals,
        data: String(message.data).trim().toUpperCase(),
        node: entry.ecu
    };

    return { canId, ecu: entry.ecu, signals };
}

// GPS counts as "fix" while position frames (0x10B) keep arriving. The NXP node
// only sends them when the receiver has a valid fix.
function gpsStatus(maxAgeMs = 10000) {
    const t = lastSeen["0x10B"];
    const ageMs = t ? Date.now() - t : null;
    return { fix: ageMs !== null && ageMs <= maxAgeMs, ageMs };
}

module.exports = {
    gpsStatus,
    decodeCANMessage,
    getNodeFromCANId,
    vehicleData,
    canMessages
};
