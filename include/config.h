#pragma once
// ============================================================
//  ESP32-024 Nightscout SGV display - user settings
//  Edit ONLY this file for your own environment.
// ============================================================

// ---- Wi-Fi -------------------------------------------------
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// ---- Nightscout --------------------------------------------
// Set the server host to its LAN IP address or hostname,
// e.g. "192.168.1.20" or "raspberrypi.local"
#define NIGHTSCOUT_HOST ""
#define NIGHTSCOUT_PORT 1337          // Nightscout default port
#define NIGHTSCOUT_USE_HTTPS 0        // 0 = http (normal for local LAN)

// If your Nightscout requires an access token for readable subjects,
// set it here (token name/subject with read permission). Leave "" if unused.
// It is sent as ?token=... on the API request.
#define NIGHTSCOUT_TOKEN ""

// API path used to fetch the latest entries (2 = current + previous, for delta)
#define NIGHTSCOUT_PATH "/api/v1/entries/sgv.json?count=2"

// ---- Refresh ------------------------------------------------
// How often to ask Nightscout for a new value (ms).
// CGM data itself usually updates every 5 minutes.
#define FETCH_INTERVAL_MS 30000

// ---- SGV colour thresholds (mg/dL) --------------------------
//   sgv <= SGV_LOW_MAX   -> red
//   sgv >= SGV_HIGH_MIN  -> yellow
//   between them         -> green
// (with the defaults: <=70 red, 71-179 green, >=180 yellow)
#define SGV_LOW_MAX  70
#define SGV_HIGH_MIN 180

// Elapsed time is shown in orange when data is this old or older (minutes)
#define STALE_WARN_MIN 10

// ---- DHT22 temperature / humidity sensor ----------------------
// Data pin of the DHT22 (module with 3 pins, or sensor + 10k pull-up
// to 3.3V on the data line). VCC = 3.3V, GND = GND.
#define DHT_PIN 22

// ---- Display -------------------------------------------------
// Backlight brightness, 0 (off) .. 255 (full). 200 is slightly dimmed.
// (brightness moved to BACKLIGHT_LEVEL in main.cpp - see there)

// ---- Clock ---------------------------------------------------
// Japan = UTC+9, no daylight saving
#define TZ_OFFSET_SEC (9 * 3600)
#define NTP_SERVER1 "ntp.nict.jp"
#define NTP_SERVER2 "pool.ntp.org"
