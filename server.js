require("dotenv").config({ path: ".env.local" });
const express = require("express");
const http = require("http");
const WebSocket = require("ws");
const mqtt = require("mqtt");
const { Pool } = require("pg");
const {
    decodeCANMessage,
    getNodeFromCANId,
    vehicleData,
    canMessages,
    gpsStatus
} = require("./canDecoder");

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

const MQTT_TOPIC =
    process.env.MQTT_TOPIC ||
    "vehicle/can";

const MQTT_USERNAME =
    process.env.MQTT_USERNAME || "";

const MQTT_PASSWORD =
    process.env.MQTT_PASSWORD || "";

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

// vehicleData and canMessages now live in canDecoder.js

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

        username: MQTT_USERNAME,

        password: MQTT_PASSWORD,

        clean: true,

        reconnectPeriod: 5000,

        connectTimeout: 10000,

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
// CREATE TELEMETRY
// ============================================================

function createTelemetry() {

    const gps = gpsStatus();

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

        pressure:
            vehicleData.pressure,

        gas:
            vehicleData.gas,

        distance:
            vehicleData.distance,

        accelX:
            vehicleData.accelX,

        accelY:
            vehicleData.accelY,

        accelZ:
            vehicleData.accelZ,

        latitude:
            vehicleData.latitude,

        longitude:
            vehicleData.longitude,

        gpsFix:
            gps.fix,

        gpsAgeMs:
            gps.ageMs,

        canMessages:
            canMessages
    };
}

// ============================================================
// MQTT MESSAGE
//
// Two payload shapes are accepted:
//   1) one frame:   {"canId":"0x100","dlc":2,"data":"03 24"}
//   2) a snapshot:  {"t":123,"f":[["0x100","03 24"],["0x101","07 08"]]}
//      (the Arduino gateway sends this to Adafruit IO, one message per
//       few seconds, because the free tier allows 30 messages/minute)
// ============================================================

function framesFromMessage(message) {

    if (message && Array.isArray(message.f)) {

        return message.f
            .filter((pair) => Array.isArray(pair) && pair.length >= 2)
            .map(([canId, data]) => ({
                canId,
                data,
                dlc: String(data).trim().split(/\s+/).length
            }));
    }

    return [message];
}

function processFrame(frame) {

    const decoded = decodeCANMessage(frame);

    if (!decoded) {
        return false;
    }

    for (const s of decoded.signals) {

        saveCanReading(
            decoded.canId,
            decoded.ecu,
            s.signal,
            s.value,
            s.unit,
            frame.dlc,
            frame.data
        );

        console.log(
            `[CAN] ${decoded.canId} | ${decoded.ecu} | ` +
            `${s.signal} = ${s.value.toFixed(2)} ${s.unit}`
        );
    }

    return true;
}

mqttClient.on(
    "message",
    (topic, payload) => {

        if (topic !== MQTT_TOPIC) {
            return;
        }

        try {

            const message = JSON.parse(payload.toString());

            const frames = framesFromMessage(message);

            console.log("--------------------------------------");
            console.log(
                `[MQTT] ${frames.length} CAN frame(s) received on ${topic}`
            );

            let decodedCount = 0;

            for (const frame of frames) {
                if (processFrame(frame)) {
                    decodedCount++;
                }
            }

            if (decodedCount === 0) {
                return;
            }

            // Build and broadcast telemetry once per MQTT message
            const telemetry = createTelemetry();

            telemetry.serverTime = Date.now();

            latestData = telemetry;

            broadcast(JSON.stringify(telemetry));
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
