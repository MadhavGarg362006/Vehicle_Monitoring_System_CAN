require("dotenv").config({ path: ".env.local" });
const express = require("express");
const http = require("http");
const WebSocket = require("ws");
const mqtt = require("mqtt");
const { Pool } = require("pg");

// ============================================================
// EXPRESS + WEBSOCKET SERVER
// ============================================================

const app = express();

const server = http.createServer(app);

const wss = new WebSocket.Server({
    server,
    path: "/ws"
});

// ============================================================
// CONFIGURATION
// ============================================================

const PORT = process.env.PORT || 10000;

const MQTT_BROKER =
    process.env.MQTT_BROKER ||
    "mqtt://localhost:1883";

const MQTT_TOPIC = "vehicle/can";

// ============================================================
// POSTGRESQL / NEON DATABASE
// ============================================================

const pool = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: {
        rejectUnauthorized: false
    }
});

pool.query("SELECT NOW()")
    .then(() => {
        console.log("[DB] Connected to Neon PostgreSQL");
    })
    .catch((err) => {
        console.error("[DB] Connection failed:", err.message);
    });

// ============================================================
// STATE
// ============================================================

let latestData = null;

let dashboardCount = 0;

// Current decoded vehicle values
const vehicleData = {
    temperature: 0.0,
    vibration: 0.0,
    batteryVoltage: 0.0,
    engineTemp: 0.0,
    rpm: 0,
    batteryCurrent: 0.0,
    speed: 0.0,
    humidity: 0.0
};

// CAN message storage
const canMessages = {};

// ============================================================
// SAVE CAN READING TO NEON
// ============================================================

async function saveCanReading(
    canId,
    ecu,
    signal,
    value,
    unit,
    dlc,
    rawData
) {
    try {

        await pool.query(
            `INSERT INTO can_readings
            (
                timestamp,
                can_id,
                ecu,
                signal,
                value,
                unit,
                dlc,
                raw_data
            )
            VALUES
            (
                NOW(),
                $1,
                $2,
                $3,
                $4,
                $5,
                $6,
                $7
            )`,
            [
                canId,
                ecu,
                signal,
                value,
                unit,
                dlc,
                rawData
            ]
        );

        console.log(
            `[DB] Saved ${canId} | ${ecu} | ${signal} = ${value} ${unit || ""}`
        );

    } catch (err) {

        console.error(
            "[DB] Insert failed:",
            err.message
        );
    }
}

// ============================================================
// MQTT CONNECTION
// ============================================================

console.log("======================================");
console.log(" CONNECTING TO MQTT BROKER");
console.log("======================================");

console.log(
    "MQTT Broker:",
    MQTT_BROKER
);

console.log(
    "MQTT Topic:",
    MQTT_TOPIC
);

const mqttClient = mqtt.connect(
    MQTT_BROKER,
    {
        clientId:
            "vhms-node-server-" +
            Math.random()
                .toString(16)
                .substring(2),

        clean: true,

        reconnectPeriod: 1000,

        connectTimeout: 5000,

        keepalive: 60
    }
);

// ============================================================
// MQTT CONNECT
// ============================================================

mqttClient.on("connect", () => {

    console.log("======================================");

    console.log(
        "[MQTT] Connected to broker"
    );

    console.log(
        "[MQTT] Broker:",
        MQTT_BROKER
    );

    console.log(
        "[MQTT] Subscribing to:",
        MQTT_TOPIC
    );

    mqttClient.subscribe(
        MQTT_TOPIC,
        {
            qos: 1
        },
        (err) => {

            if (err) {

                console.error(
                    "[MQTT] Subscribe error:",
                    err.message
                );

                return;
            }

            console.log(
                "[MQTT] Subscription successful"
            );

            console.log("======================================");
        }
    );
});

// ============================================================
// MQTT ERROR
// ============================================================

mqttClient.on("error", (err) => {

    console.error(
        "[MQTT] Error:",
        err.message
    );
});

// ============================================================
// MQTT RECONNECT
// ============================================================

mqttClient.on("reconnect", () => {

    console.log(
        "[MQTT] Attempting reconnect..."
    );
});

// ============================================================
// MQTT DISCONNECT
// ============================================================

mqttClient.on("close", () => {

    console.log(
        "[MQTT] Connection closed"
    );
});

// ============================================================
// CAN DECODER
// ============================================================

function decodeCANMessage(message) {

    const canId = message.canId;

    const dataString = message.data;

    if (!canId || !dataString) {

        console.log(
            "[MQTT] Invalid CAN message"
        );

        return null;
    }

    // --------------------------------------------------------
    // Convert:
    //
    // "03 24"
    //
    // into:
    //
    // [0x03, 0x24]
    // --------------------------------------------------------

    const bytes = dataString
        .trim()
        .split(/\s+/)
        .map(
            byte =>
                parseInt(byte, 16)
        );

    if (bytes.length < 2) {

        console.log(
            "[MQTT] CAN frame has less than 2 bytes"
        );

        return null;
    }

    // --------------------------------------------------------
    // 2-byte big-endian integer
    // --------------------------------------------------------

    const raw =
        (bytes[0] << 8) |
        bytes[1];

    let value;
    let signal;
    let unit;

    // ========================================================
    // STM32
    // ========================================================

    if (canId === "0x100") {

        // Engine Temperature
        // Raw / 10

        value = raw / 10;

        vehicleData.engineTemp =
            value;

        signal =
            "Engine Temperature";

        unit =
            "°C";
    }

    else if (canId === "0x101") {

        // Engine RPM
        // Raw / 1

        value = raw;

        vehicleData.rpm =
            value;

        signal =
            "Engine RPM";

        unit =
            "RPM";
    }

    else if (canId === "0x102") {

        // Vibration
        // Raw / 100

        value = raw / 100;

        vehicleData.vibration =
            value;

        signal =
            "Vibration";

        unit =
            "g";
    }

    // ========================================================
    // ARDUINO
    // ========================================================

    else if (canId === "0x103") {

        // Ambient Temperature
        // Raw / 10

        value = raw / 10;

        vehicleData.temperature =
            value;

        signal =
            "Ambient Temperature";

        unit =
            "°C";
    }

    else if (canId === "0x104") {

        // Humidity
        // Raw / 10

        value = raw / 10;

        vehicleData.humidity =
            value;

        signal =
            "Humidity";

        unit =
            "%";
    }

    else if (canId === "0x105") {

        // Vehicle Speed
        // Raw / 10

        value = raw / 10;

        vehicleData.speed =
            value;

        signal =
            "Vehicle Speed";

        unit =
            "km/h";
    }

    // ========================================================
    // ESP32
    // ========================================================

    else if (canId === "0x106") {

        // Battery Voltage
        // Raw / 10

        value = raw / 10;

        vehicleData.batteryVoltage =
            value;

        signal =
            "Battery Voltage";

        unit =
            "V";
    }

    else if (canId === "0x107") {

        // Battery Current
        // Raw / 10

        value = raw / 10;

        vehicleData.batteryCurrent =
            value;

        signal =
            "Battery Current";

        unit =
            "A";
    }

    else {

        console.log(
            "[MQTT] Unknown CAN ID:",
            canId
        );

        return null;
    }

    // ========================================================
    // STORE CAN MESSAGE
    // ========================================================

    canMessages[canId] = {

        signal,

        value,

        unit,

        data:
            dataString
                .toUpperCase(),

        node:
            getNodeFromCANId(canId)
    };

    return {

        canId,

        signal,

        value,

        unit
    };
}

// ============================================================
// CAN NODE IDENTIFICATION
// ============================================================

function getNodeFromCANId(canId) {

    if (
        canId === "0x100" ||
        canId === "0x101" ||
        canId === "0x102"
    ) {

        return "STM32";
    }

    if (
        canId === "0x103" ||
        canId === "0x104" ||
        canId === "0x105"
    ) {

        return "Arduino";
    }

    if (
        canId === "0x106" ||
        canId === "0x107"
    ) {

        return "ESP32";
    }

    return "Unknown";
}

// ============================================================
// CREATE TELEMETRY
// ============================================================

function createTelemetry() {

    return {

        type: "sensor",

        time:
            Date.now() / 1000,

        temperature:
            vehicleData.temperature,

        vibration:
            vehicleData.vibration,

        batteryVoltage:
            vehicleData.batteryVoltage,

        engineTemp:
            vehicleData.engineTemp,

        rpm:
            vehicleData.rpm,

        batteryCurrent:
            vehicleData.batteryCurrent,

        speed:
            vehicleData.speed,

        humidity:
            vehicleData.humidity,

        canMessages:
            canMessages
    };
}

// ============================================================
// MQTT MESSAGE
// ============================================================

mqttClient.on(
    "message",
    (topic, payload) => {

        if (
            topic !== MQTT_TOPIC
        ) {

            return;
        }

        try {

            const message =
                JSON.parse(
                    payload.toString()
                );

            console.log("--------------------------------------");

            console.log(
                "[MQTT] CAN MESSAGE RECEIVED"
            );

            console.log(
                "[MQTT] Topic:",
                topic
            );

            console.log(
                "[MQTT] CAN ID:",
                message.canId
            );

            console.log(
                "[MQTT] DLC:",
                message.dlc
            );

            console.log(
                "[MQTT] DATA:",
                message.data
            );

            // ==================================================
            // DECODE CAN FRAME
            // ==================================================

            const decoded =
                decodeCANMessage(
                    message
                );

            if (!decoded) {

                return;
            }

            // ==================================================
            // GET ECU
            // ==================================================

            const ecu =
                getNodeFromCANId(
                    decoded.canId
                );

            // ==================================================
            // SAVE TO NEON DATABASE
            // ==================================================

            saveCanReading(
                decoded.canId,
                ecu,
                decoded.signal,
                decoded.value,
                decoded.unit,
                message.dlc,
                message.data
            );

            // ==================================================
            // BUILD TELEMETRY
            // ==================================================

            const telemetry =
                createTelemetry();

            telemetry.serverTime =
                Date.now();

            // ==================================================
            // STORE LATEST TELEMETRY
            // ==================================================

            latestData =
                telemetry;

            // ==================================================
            // LOG DECODED CAN DATA
            // ==================================================

            console.log(
                `[CAN] ${decoded.canId} | ` +
                `${decoded.signal} = ` +
                `${decoded.value.toFixed(2)} ` +
                `${decoded.unit}`
            );

            console.log(
                `[CAN] ECU: ${ecu}`
            );

            // ==================================================
            // BROADCAST TO DASHBOARD
            // ==================================================

            broadcast(
                JSON.stringify(
                    telemetry
                )
            );

        }

        catch (err) {

            console.error(
                "[MQTT] Invalid message:",
                err.message
            );
        }
    }
);

// ============================================================
// HTTP ROUTES
// ============================================================

app.get("/", (req, res) => {

    res.json({

        service:
            "vehicle-health-mqtt-websocket-relay",

        websocket:
            "/ws",

        mqttTopic:
            MQTT_TOPIC,

        dashboardCount,

        hasData:
            latestData !== null,

        latestData
    });
});

app.get("/health", (req, res) => {

    res.status(200).send("OK");
});

// ============================================================
// DATABASE HEALTH ROUTE
// ============================================================

app.get("/db-health", async (req, res) => {

    try {

        const result =
            await pool.query(
                "SELECT NOW() AS server_time"
            );

        res.json({
            database: "connected",
            serverTime: result.rows[0].server_time
        });

    } catch (err) {

        console.error(
            "[DB] Health check failed:",
            err.message
        );

        res.status(500).json({
            database: "error",
            error: err.message
        });
    }
});

// ============================================================
// WEBSOCKET BROADCAST
// ============================================================

function broadcast(data) {

    let sent = 0;

    for (
        const client
        of wss.clients
    ) {

        if (

            client.readyState ===
                WebSocket.OPEN

            &&

            client.role ===
                "dashboard"

        ) {

            client.send(data);

            sent++;
        }
    }

    console.log(
        `[WS] Broadcasted telemetry to ` +
        `${sent} dashboard(s)`
    );
}

// ============================================================
// WEBSOCKET CONNECTION
// ============================================================

wss.on(
    "connection",
    (ws, req) => {

        const url =
            new URL(
                req.url,
                `http://${req.headers.host}`
            );

        const role =
            url.searchParams.get(
                "role"
            );

        console.log(
            "======================================"
        );

        console.log(
            "[WS] NEW CONNECTION"
        );

        console.log(
            "[WS] Path:",
            req.url
        );

        console.log(
            "[WS] Role:",
            role
        );

        console.log(
            "======================================"
        );

        // ====================================================
        // DASHBOARD
        // ====================================================

        if (
            role === "dashboard"
        ) {

            ws.role =
                "dashboard";

            dashboardCount++;

            console.log(
                "[DASHBOARD] CONNECTED"
            );

            console.log(
                "[DASHBOARD] Dashboard count:",
                dashboardCount
            );

            // Send latest telemetry
            if (latestData) {

                console.log(
                    "[DASHBOARD] Sending latest telemetry"
                );

                ws.send(
                    JSON.stringify(
                        latestData
                    )
                );
            }

            ws.on(
                "close",
                () => {

                    dashboardCount--;

                    console.log(
                        "[DASHBOARD] DISCONNECTED"
                    );

                    console.log(
                        "[DASHBOARD] Dashboard count:",
                        dashboardCount
                    );
                }
            );

            ws.on(
                "error",
                (err) => {

                    console.error(
                        "[DASHBOARD] WebSocket error:",
                        err.message
                    );
                }
            );
        }

        // ====================================================
        // DEVICE
        // ====================================================

        else if (
            role === "device"
        ) {

            console.log(
                "[WS] Device WebSocket is no longer used."
            );

            ws.close(
                1008,
                "Use MQTT for device telemetry"
            );
        }

        // ====================================================
        // INVALID ROLE
        // ====================================================

        else {

            console.log(
                "[WS] INVALID ROLE:",
                role
            );

            ws.close(
                1008,
                "role must be dashboard"
            );
        }
    }
);

// ============================================================
// WEBSOCKET HEARTBEAT
// ============================================================

const heartbeat =
    setInterval(
        () => {

            wss.clients.forEach(
                (ws) => {

                    if (
                        ws.isAlive === false
                    ) {

                        console.log(
                            "[WS] Terminating dead connection"
                        );

                        return ws.terminate();
                    }

                    ws.isAlive =
                        false;

                    ws.ping();
                }
            );

        },
        30000
    );

wss.on(
    "connection",
    (ws) => {

        ws.isAlive =
            true;

        ws.on(
            "pong",
            () => {

                ws.isAlive =
                    true;
            }
        );
    }
);

// ============================================================
// START SERVER
// ============================================================

server.listen(
    PORT,
    () => {

        console.log(
            "======================================"
        );

        console.log(
            " VEHICLE HEALTH MQTT + WEBSOCKET SERVER"
        );

        console.log(
            "======================================"
        );

        console.log(
            "Listening on port:",
            PORT
        );

        console.log(
            "MQTT broker:",
            MQTT_BROKER
        );

        console.log(
            "MQTT topic:",
            MQTT_TOPIC
        );

        console.log(
            "Database: Neon PostgreSQL"
        );

        console.log(
            "Dashboard WebSocket:",
            `/ws?role=dashboard`
        );

        console.log(
            "======================================"
        );
    }
);

// ============================================================
// SHUTDOWN
// ============================================================

process.on(
    "SIGTERM",
    async () => {

        console.log(
            "[SERVER] Shutting down..."
        );

        clearInterval(
            heartbeat
        );

        mqttClient.end(
            () => {

                server.close(
                    async () => {

                        try {

                            await pool.end();

                            console.log(
                                "[DB] PostgreSQL pool closed"
                            );

                        } catch (err) {

                            console.error(
                                "[DB] Shutdown error:",
                                err.message
                            );
                        }

                        process.exit(0);
                    }
                );
            }
        );
    }
);
