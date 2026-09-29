const express = require("express");
const http = require("http");
const WebSocket = require("ws");

const app = express();
const server = http.createServer(app);

const wss = new WebSocket.Server({
    server,
    path: "/ws"
});

const PORT = process.env.PORT || 10000;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || "change-me";

let latestData = null;
let deviceCount = 0;
let dashboardCount = 0;

app.get("/", (req, res) => {
    res.json({
        service: "vehicle-health-websocket-relay",
        websocket: "/ws",
        deviceCount,
        dashboardCount,
        hasData: latestData !== null,
        latestData
    });
});

app.get("/health", (req, res) => {
    res.status(200).send("OK");
});

function broadcast(data) {
    let sent = 0;

    for (const client of wss.clients) {
        if (
            client.readyState === WebSocket.OPEN &&
            client.role === "dashboard"
        ) {
            client.send(data);
            sent++;
        }
    }

    console.log(`[WS] Broadcasted telemetry to ${sent} dashboard(s)`);
}

wss.on("connection", (ws, req) => {

    const url = new URL(
        req.url,
        `http://${req.headers.host}`
    );

    const role = url.searchParams.get("role");

    console.log("======================================");
    console.log("[WS] NEW CONNECTION");
    console.log("[WS] Path:", req.url);
    console.log("[WS] Role:", role);
    console.log("======================================");

    // ==============================
    // DEVICE
    // ==============================

    if (role === "device") {

        const token = url.searchParams.get("token");

        console.log("[DEVICE] Token received:", token ? "YES" : "NO");
        console.log(
            "[DEVICE] Token valid:",
            token === DEVICE_TOKEN
        );

        if (token !== DEVICE_TOKEN) {
            console.log("[DEVICE] INVALID TOKEN - closing");
            ws.close(1008, "Invalid device token");
            return;
        }

        ws.role = "device";
        deviceCount++;

        console.log("[DEVICE] CONNECTED");
        console.log("[DEVICE] Device count:", deviceCount);

        // Send latest data if available
        if (latestData) {
            console.log("[DEVICE] Sending latest data");
            ws.send(JSON.stringify(latestData));
        }

        ws.on("message", (raw) => {

            console.log("--------------------------------------");
            console.log("[DEVICE] MESSAGE RECEIVED");
            console.log("[DEVICE] Raw:", raw.toString());

            try {

                const data = JSON.parse(raw.toString());

                console.log("[DEVICE] JSON parsed successfully");
                console.log("[DEVICE] type:", data.type);

                if (data.type !== "sensor") {
                    console.log("[DEVICE] Ignoring non-sensor message");
                    return;
                }

                data.serverTime = Date.now();

                latestData = data;

                console.log("[DEVICE] TELEMETRY ACCEPTED");
                console.log("[DEVICE] Latest data:", latestData);

                broadcast(JSON.stringify(data));

            } catch (e) {

                console.error(
                    "[DEVICE] INVALID JSON:",
                    e.message
                );

            }
        });

        ws.on("close", (code, reason) => {

            deviceCount--;

            console.log("--------------------------------------");
            console.log("[DEVICE] DISCONNECTED");
            console.log("[DEVICE] Code:", code);
            console.log(
                "[DEVICE] Reason:",
                reason ? reason.toString() : ""
            );
            console.log(
                "[DEVICE] Device count:",
                deviceCount
            );
        });

        ws.on("error", (err) => {
            console.error(
                "[DEVICE] WebSocket error:",
                err.message
            );
        });

    }

    // ==============================
    // DASHBOARD
    // ==============================

    else if (role === "dashboard") {

        ws.role = "dashboard";
        dashboardCount++;

        console.log("[DASHBOARD] CONNECTED");
        console.log(
            "[DASHBOARD] Dashboard count:",
            dashboardCount
        );

        if (latestData) {
            console.log("[DASHBOARD] Sending latest telemetry");
            ws.send(JSON.stringify(latestData));
        }

        ws.on("close", () => {

            dashboardCount--;

            console.log("[DASHBOARD] DISCONNECTED");
            console.log(
                "[DASHBOARD] Dashboard count:",
                dashboardCount
            );
        });

        ws.on("error", (err) => {
            console.error(
                "[DASHBOARD] WebSocket error:",
                err.message
            );
        });

    }

    // ==============================
    // INVALID ROLE
    // ==============================

    else {

        console.log("[WS] INVALID ROLE:", role);

        ws.close(
            1008,
            "role must be device or dashboard"
        );
    }
});

// ==============================
// HEARTBEAT
// ==============================

const heartbeat = setInterval(() => {

    wss.clients.forEach((ws) => {

        if (ws.isAlive === false) {
            console.log("[WS] Terminating dead connection");
            return ws.terminate();
        }

        ws.isAlive = false;
        ws.ping();
    });

}, 30000);

wss.on("connection", (ws) => {

    ws.isAlive = true;

    ws.on("pong", () => {
        ws.isAlive = true;
    });

});

// ==============================
// START SERVER
// ==============================

server.listen(PORT, () => {

    console.log("======================================");
    console.log(" VEHICLE HEALTH WEBSOCKET SERVER");
    console.log("======================================");
    console.log("Listening on port:", PORT);
    console.log("Device token configured:", DEVICE_TOKEN !== "change-me");
    console.log("======================================");

});

// ==============================
// SHUTDOWN
// ==============================

process.on("SIGTERM", () => {

    clearInterval(heartbeat);

    server.close(() => {
        process.exit(0);
    });

});
