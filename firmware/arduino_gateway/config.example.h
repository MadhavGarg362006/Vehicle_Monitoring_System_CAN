#pragma once

// ---------------- WiFi ----------------
#define WIFI_SSID      "YOUR_WIFI"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// ---------------- Adafruit IO (MQTT) ----------------
// io.adafruit.com -> My Key shows your username and AIO key.
// The AIO key is a secret: do not commit this file to GitHub.
#define AIO_USERNAME   "YOUR_ADAFRUIT_IO_USERNAME"
#define AIO_KEY        "YOUR_ADAFRUIT_IO_KEY"

// Feed key (lowercase, no spaces). It is created automatically on the first publish.
#define AIO_FEED       "vhms-can"

// 1 = TLS on port 8883 (recommended, the key stays encrypted).
// 0 = plain MQTT on port 1883 (only for testing; the key travels unencrypted).
// If the TLS connection fails on the UNO R4 WiFi, test with 0 first.
#define AIO_USE_TLS    1

// Free Adafruit IO accounts allow 30 data points per minute and disconnect
// you when you go over. One snapshot message = one data point, so 3000 ms
// gives 20 per minute. Do not go below 2000.
#define SNAPSHOT_INTERVAL_MS  3000UL
