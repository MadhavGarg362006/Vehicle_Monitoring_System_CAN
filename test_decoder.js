"use strict";
const assert = require("assert");
const { decodeCANMessage, getNodeFromCANId, vehicleData } = require("./canDecoder");

const frame = (canId, data) => ({ canId, dlc: data.split(" ").length, data });
const close = (a, b) => Math.abs(a - b) < 1e-9;

// existing IDs, same scaling as before
let r = decodeCANMessage(frame("0x100", "03 24"));
assert(close(r.signals[0].value, 80.4) && r.ecu === "STM32");
r = decodeCANMessage(frame("0x101", "07 08"));
assert.strictEqual(r.signals[0].value, 1800);
r = decodeCANMessage(frame("0x102", "00 19"));
assert(close(r.signals[0].value, 0.25));
r = decodeCANMessage(frame("0x103", "01 22"));
assert(close(r.signals[0].value, 29.0));
r = decodeCANMessage(frame("0x106", "00 8A"));
assert(close(r.signals[0].value, 13.8));
r = decodeCANMessage(frame("0x107", "00 2D"));
assert(close(r.signals[0].value, 4.5));

// new IDs
r = decodeCANMessage(frame("0x108", "01 F4"));
assert.strictEqual(r.signals[0].value, 500);
r = decodeCANMessage(frame("0x109", "05 00"));
assert(close(r.signals[0].value, 128.0));

// signed 3-axis acceleration: x=+0.5 g, y=-0.25 g, z=+1.0 g
r = decodeCANMessage(frame("0x10A", "01 F4 FF 06 03 E8"));
assert.strictEqual(r.signals.length, 3);
assert(close(r.signals[0].value, 0.5));
assert(close(r.signals[1].value, -0.25));
assert(close(r.signals[2].value, 1.0));
assert.strictEqual(r.ecu, "MCXN236");

// GPS: encode int32 (1e-6 degree) the way the MCXN236 will, then decode
const enc32 = (deg) => {
    const v = Math.round(deg * 1e6) | 0;
    return [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]
        .map((x) => x.toString(16).padStart(2, "0").toUpperCase());
};
let gps = enc32(12.971599).concat(enc32(77.594566)).join(" ");
r = decodeCANMessage(frame("0x10B", gps));
assert(close(r.signals[0].value, 12.971599) && close(r.signals[1].value, 77.594566));
gps = enc32(-33.868820).concat(enc32(-151.209296)).join(" ");   // negative values
r = decodeCANMessage(frame("0x10B", gps));
assert(close(r.signals[0].value, -33.86882) && close(r.signals[1].value, -151.209296));

// lower-case / unpadded IDs, case-insensitive hex, other formats
r = decodeCANMessage(frame("0x10a", "01 f4 ff 06 03 e8"));
assert(r && r.canId === "0x10A");
assert.strictEqual(getNodeFromCANId("0x105"), "MCXN236");
assert.strictEqual(getNodeFromCANId("0x101"), "Arduino");
assert.strictEqual(getNodeFromCANId("0x7FF"), "Unknown");

// rejects
assert.strictEqual(decodeCANMessage(frame("0x10A", "01 F4")), null);   // too short
assert.strictEqual(decodeCANMessage(frame("0x7FF", "00 00")), null);   // unknown
assert.strictEqual(decodeCANMessage({ canId: "0x100", data: "ZZ 00" }), null);
assert.strictEqual(decodeCANMessage({ canId: "0x100" }), null);

// vehicleData follows the last decode
decodeCANMessage(frame("0x105", "00 FA"));
assert(close(vehicleData.speed, 25.0));

// 0x10C pressure (BME280): 0x278A = 10122 -> 1012.2 hPa
r = decodeCANMessage(frame("0x10C", "27 8A"));
assert(close(r.signals[0].value, 1012.2) && r.ecu === "Arduino");
assert(close(vehicleData.pressure, 1012.2));

console.log("canDecoder: all tests passed");
