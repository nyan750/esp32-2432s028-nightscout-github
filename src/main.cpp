// ESP32-024 Nightscout SGV display - PlatformIO / Arduino framework
//
// Screen (landscape 320x240):
//   - SGV value, extra large (font 8 scaled up to 108px), coloured by range
//   - Trend arrow on the right, elapsed time (large, blue) under it
//   - Bottom panel, rotating every minute: big clock (HH:MM,
//     font 8) / HbA1c estimates / temperature + humidity
//     from a DHT22 sensor (data pin: DHT_PIN in config.h)
// Serial output is disabled in this build (no logging).
// No touch handling in this build.
//
// All user settings (Wi-Fi, Nightscout host, thresholds, ...)
// live in include/config.h - edit that file only.

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "config.h"

// Backlight brightness 0-255 (smaller = darker). Defined HERE,
// not in config.h: this project is updated by replacing main.cpp
// alone, and a config.h value would silently override an edit
// made here (that is exactly what happened with the old
// BACKLIGHT_BRIGHTNESS setting).
#define BACKLIGHT_LEVEL 70
#ifndef DHT_PIN
#define DHT_PIN 22
#endif

// Colour fixes for the current board (ESP32-2432S028, ILI9341):
// inversion must be OFF (matches -DTFT_INVERSION_OFF=1 in
// platformio.ini), and colour order is standard RGB on this board
// (no -DTFT_RGB_ORDER flag in platformio.ini; add
// -DTFT_RGB_ORDER=TFT_BGR only if red/blue come out swapped).
// Note: the previous ESP32-024 board was BGR order - that setting
// must NOT be carried over to this board.
#define DISPLAY_INVERT true

TFT_eSPI tft = TFT_eSPI();

// ---- state ---------------------------------------------------
int      g_sgv = -1;
int      g_prevSgv = -1;
String   g_direction = "";
uint64_t g_entryMs = 0;      // epoch ms of the latest SGV entry
bool     g_haveData = false;
bool     g_lastFetchOk = false;
unsigned long g_lastFetch = 0;

// Bottom panel: clock, HbA1c estimates, and temperature/humidity.
bool     g_a1cPanelDirty = true;
int      g_shownPanelMode = -1;         // 0 = clock, 1 = HbA1c, 2 = temp/humidity
char     g_shownClock[8] = "";

// DHT22 readings
float    g_tempC = 0, g_humi = 0;
bool     g_dhtOk = false;
unsigned long g_lastDhtRead = 0;
int      g_shownTemp = -999, g_shownHumi = -999;
bool     g_shownDhtOk = false;

// What is currently on screen. The display is only redrawn where
// these differ from the live values - redrawing the whole screen
// on every fetch / every minute caused visible flicker.
int      g_shownSgv = -2;
String   g_shownDirection = "\x01";   // impossible initial value
int      g_shownArrowColor = -1;
int      g_shownElapsedMin = -2;
bool     g_shownNoData = false;

// ---- helpers -------------------------------------------------
static uint16_t sgvColor(int sgv) {
  if (sgv <= SGV_LOW_MAX)  return TFT_RED;
  if (sgv >= SGV_HIGH_MIN) return TFT_YELLOW;
  return TFT_GREEN;
}

static int elapsedMinutes() {
  if (!g_haveData) return -1;
  time_t now = time(nullptr);
  if (now < 1700000000) return -1;          // NTP not synced yet
  int64_t diffSec = (int64_t)now - (int64_t)(g_entryMs / 1000ULL);
  if (diffSec < 0) diffSec = 0;
  return (int)(diffSec / 60);
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2);
  }
}

// ---- DHT22 / AM2302 (single-wire protocol read directly) -------
// The data format uses 16-bit values with 0.1 resolution, a
// temperature sign bit, and a 1-10ms start signal low pulse.
static bool waitLevel(int level, uint32_t timeoutUs) {
  uint32_t start = micros();
  while (digitalRead(DHT_PIN) != level) {
    if (micros() - start > timeoutUs) return false;
  }
  return true;
}

static bool readDHT22(float &tempC, float &humi) {
  uint8_t data[5] = {0, 0, 0, 0, 0};
  // Start signal: pull low for 2ms, then release.
  pinMode(DHT_PIN, OUTPUT);
  digitalWrite(DHT_PIN, LOW);
  delay(2);
  digitalWrite(DHT_PIN, HIGH);
  delayMicroseconds(30);
  pinMode(DHT_PIN, INPUT_PULLUP);

  noInterrupts();
  bool ok = waitLevel(LOW, 300) &&    // sensor response: low 80us
            waitLevel(HIGH, 300) &&   // then high 80us
            waitLevel(LOW, 300);      // then data starts (low 50us)
  for (int i = 0; ok && i < 40; i++) {
    if (!waitLevel(HIGH, 200)) { ok = false; break; }   // bit start
    uint32_t t0 = micros();
    if (!waitLevel(LOW, 200)) { ok = false; break; }    // bit end
    int bit = (micros() - t0 > 40) ? 1 : 0;             // 26us=0, 70us=1
    data[i / 8] = (data[i / 8] << 1) | bit;
  }
  interrupts();
  if (!ok) return false;
  if ((((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4])) return false;
  humi = ((data[0] << 8) | data[1]) / 10.0f;
  int16_t raw = ((data[2] & 0x7F) << 8) | data[3];
  tempC = raw / 10.0f;
  if (data[2] & 0x80) tempC = -tempC;
  return true;
}

// ---- Nightscout fetch ----------------------------------------
static String entriesUrl(int count) {
  String path = String(NIGHTSCOUT_PATH);
  int q = path.indexOf("count=");
  if (q >= 0) {   // replace the count=N in the configured path
    int end = path.indexOf('&', q);
    path = path.substring(0, q + 6) + String(count) +
           (end >= 0 ? path.substring(end) : String(""));
  }
  String url = String(NIGHTSCOUT_USE_HTTPS ? "https://" : "http://")
             + NIGHTSCOUT_HOST + ":" + String(NIGHTSCOUT_PORT) + path;
  if (strlen(NIGHTSCOUT_TOKEN) > 0) url += "&token=" + String(NIGHTSCOUT_TOKEN);
  return url;
}

static bool fetchNightscout() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) { g_lastFetchOk = false; return false; }

  String url = entriesUrl(2);

  HTTPClient http;
  http.setTimeout(8000);
  WiFiClientSecure secureClient;
  if (NIGHTSCOUT_USE_HTTPS) {
    secureClient.setInsecure();
    http.begin(secureClient, url);
  } else {
    http.begin(url);
  }

  int code = http.GET();
  if (code != 200) {
    http.end();
    g_lastFetchOk = false;
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    g_lastFetchOk = false;
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  JsonObject e0 = arr[0];
  g_sgv       = e0["sgv"] | -1;
  g_entryMs   = e0["date"] | (uint64_t)0;
  g_direction = (const char*)(e0["direction"] | "NONE");
  g_prevSgv   = (arr.size() > 1) ? (arr[1]["sgv"] | -1) : -1;
  g_haveData  = (g_sgv > 0);
  g_lastFetchOk = g_haveData;
  return g_haveData;
}

// ---- drawing ---------------------------------------------------
static void drawOneArrow(int cx, int cy, int len, int angleDeg, uint16_t color) {
  float rad = angleDeg * (float)PI / 180.0f;
  float dx = cosf(rad), dy = sinf(rad);
  float px = -dy, py = dx;   // perpendicular
  int x0 = (int)(cx - dx * len), y0 = (int)(cy - dy * len);
  int x1 = (int)(cx + dx * len), y1 = (int)(cy + dy * len);
  // thick shaft: 5 parallel lines
  for (int o = -2; o <= 2; o++) {
    tft.drawLine(x0 + (int)(px * o), y0 + (int)(py * o),
                 x1 + (int)(px * o), y1 + (int)(py * o), color);
  }
  // arrow head
  for (int s = -1; s <= 1; s++) {
    float a1 = rad + 2.62f, a2 = rad - 2.62f;   // +/- 150 deg
    int hx1 = x1 + (int)(cosf(a1) * len * 0.55f) + (int)(px * s);
    int hy1 = y1 + (int)(sinf(a1) * len * 0.55f) + (int)(py * s);
    int hx2 = x1 + (int)(cosf(a2) * len * 0.55f) + (int)(px * s);
    int hy2 = y1 + (int)(sinf(a2) * len * 0.55f) + (int)(py * s);
    tft.drawLine(x1, y1, hx1, hy1, color);
    tft.drawLine(x1, y1, hx2, hy2, color);
  }
}

static void drawTrend(int cx, int cy, uint16_t color) {
  // direction string -> angle (0 = right/flat, -90 = up, 90 = down)
  int angle; bool dbl = false; bool known = true;
  if      (g_direction == "DoubleUp")      { angle = -90; dbl = true; }
  else if (g_direction == "SingleUp")      { angle = -90; }
  else if (g_direction == "FortyFiveUp")   { angle = -45; }
  else if (g_direction == "Flat")          { angle = 0; }
  else if (g_direction == "FortyFiveDown") { angle = 45; }
  else if (g_direction == "SingleDown")    { angle = 90; }
  else if (g_direction == "DoubleDown")    { angle = 90; dbl = true; }
  else known = false;

  if (!known) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("?", cx, cy, 4);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  if (dbl) {
    drawOneArrow(cx - 11, cy, 22, angle, color);
    drawOneArrow(cx + 11, cy, 22, angle, color);
  } else {
    drawOneArrow(cx, cy, 26, angle, color);
  }
}

// ---- big SGV in the original font 8, scaled up -----------------
// Font 8 (Arial, digits 55px wide) is 75px tall at native size.
// To show it at ~108px, the string is rendered into a sprite at
// native size and then scaled up (nearest neighbour) into a
// second sprite that is pushed to the screen in one operation.
static void drawBigSgv(int value, uint16_t c) {
  const int DST_H = 108;                 // target height (native: 75)
  String s = String(value);
  int srcW = tft.textWidth(s, 8);        // native width (55/digit)
  int dstW = (srcW * DST_H + 74) / 75;   // scaled width

  static TFT_eSprite sprSrc(&tft);
  static TFT_eSprite sprDst(&tft);
  static bool sprTried = false, sprOk = false;
  if (!sprTried) {
    sprTried = true;
    sprOk = (sprSrc.createSprite(168, 76) != nullptr) &&
            (sprDst.createSprite(240, DST_H) != nullptr);
  }
  if (!sprOk) {                          // fallback: native size
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(c, TFT_BLACK);
    tft.drawString(s, 12, 42, 8);
    return;
  }

  sprSrc.fillSprite(TFT_BLACK);
  sprSrc.setTextDatum(TL_DATUM);
  sprSrc.setTextColor(c, TFT_BLACK);
  sprSrc.drawString(s, 0, 0, 8);

  sprDst.fillSprite(TFT_BLACK);
  for (int dy = 0; dy < DST_H; dy++) {
    int sy = dy * 75 / DST_H;
    for (int dx = 0; dx < dstW; dx++) {
      int sx = dx * srcW / dstW;
      uint16_t px = sprSrc.readPixel(sx, sy);
      if (px != TFT_BLACK) sprDst.drawPixel(dx, dy, px);
    }
  }
  sprDst.pushSprite(4, 12);
}

// Redraw only the parts whose values changed (anti-flicker).
// Regions: SGV (left) / arrow (right top) / elapsed (right,
// under the arrow). Nothing here clears the whole area.
static void drawMainArea() {
  if (!g_haveData) {
    if (!g_shownNoData) {
      g_shownNoData = true;
      g_shownSgv = -2; g_shownDirection = "\x01"; g_shownElapsedMin = -2; g_shownArrowColor = -1;
      tft.fillRect(0, 0, 320, 160, TFT_BLACK);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(WiFi.status() == WL_CONNECTED ? "No data..." : "WiFi...", 160, 100, 4);
      tft.setTextDatum(TL_DATUM);
    }
    return;
  }
  if (g_shownNoData) {          // coming back from the No-data screen
    g_shownNoData = false;
    tft.fillRect(0, 0, 320, 160, TFT_BLACK);
    g_shownSgv = -2;
    g_shownDirection = "\x01"; g_shownElapsedMin = -2; g_shownArrowColor = -1;
  }

  uint16_t color = sgvColor(g_sgv);

  // SGV: only when the value changed (no delta display)
  if (g_sgv != g_shownSgv) {
    g_shownSgv = g_sgv;
    tft.fillRect(0, 6, 244, 124, TFT_BLACK);
    drawBigSgv(g_sgv, color);              // font 8 scaled to 108px
  }

  // trend arrow: only when direction or its colour changed
  if (g_direction != g_shownDirection || (int)color != g_shownArrowColor) {
    g_shownDirection = g_direction;
    g_shownArrowColor = (int)color;
    tft.fillRect(246, 2, 74, 88, TFT_BLACK);
    drawTrend(276, 42, color);
  }

  // elapsed time: only when the minute value changed.
  // Large and blue: total minutes in font 6 (48px) with a "min"
  // label, under the arrow in the right column. Always blue.
  int em = elapsedMinutes();
  if (em != g_shownElapsedMin) {
    g_shownElapsedMin = em;
    uint16_t col = 0x1C9F;   // vivid blue (dodger blue in RGB565)
    String num = (em < 0) ? "--" : String(em);
    // NOTE: the clear rect starts at x=244, right of the widest
    // SGV ink (~x239 for 3 digits). Starting it further left used
    // to erase the lower-right of the last SGV digit every minute.
    tft.fillRect(244, 84, 76, 76, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, TFT_BLACK);
    if (num.length() > 2) tft.drawString(num, 282, 110, 4);   // 100+ min: narrower font
    else                  tft.drawString(num, 282, 110, 6);   // 48px digits
    tft.drawString("min", 282, 146, 4);    // 26px label
    tft.setTextDatum(TL_DATUM);
  }
}

// ---- bottom panel: clock / HbA1c / temp+humi, rotating ---------
// Panel: y 160..240. The display switches every minute,
// rotating clock -> HbA1c -> temperature/humidity.
#define PANEL_TOP 160

static void drawPanelClock(bool force) {
  time_t now = time(nullptr);
  char buf[8];
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }
  if (!force && strcmp(buf, g_shownClock) == 0) return;
  strcpy(g_shownClock, buf);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, 160, 201, 8);      // font 8: 75px tall
  tft.setTextDatum(TL_DATUM);
}

// ---- estimated HbA1c (from sampled CGM means) ------------------
// A 30-day mean cannot be fetched entry-by-entry (thousands of
// entries), so the mean is estimated from samples: one entry
// every 12 hours over the past 30 days (60 samples; the newest
// 28 of them are the 2-week set). Conversion is the GMI formula
// HbA1c(%) = 3.31 + 0.02392 * mean glucose (mg/dL).
// Sampling runs as a background round in loop(): one anchor
// request every 300 ms, a new round every 6 hours.
float g_a1c2w = -1, g_a1c1m = -1;      // %, -1 = no data yet
static bool     g_a1cBusy = false;
static int      g_a1cIdx = 0;
static uint64_t g_a1cStartMs = 0;
static long     g_a1cSum14 = 0, g_a1cSum30 = 0;
static int      g_a1cN14 = 0, g_a1cN30 = 0;
static unsigned long g_a1cLastTick = 0;
static unsigned long g_a1cLastRound = 0;
#define A1C_ANCHORS 60
#define A1C_SPACING_MS (12ULL * 3600ULL * 1000ULL)

// newest entry at or before anchorMs; -1 if none within 3 hours
static int fetchA1cSample(uint64_t anchorMs) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return -1;
  }
  char abuf[24];
  snprintf(abuf, sizeof(abuf), "%llu", (unsigned long long)anchorMs);
  String url = String(NIGHTSCOUT_USE_HTTPS ? "https://" : "http://")
             + NIGHTSCOUT_HOST + ":" + String(NIGHTSCOUT_PORT)
             + "/api/v1/entries/sgv.json?count=1&find%5Bdate%5D%5B%24lte%5D=" + abuf;
  if (strlen(NIGHTSCOUT_TOKEN) > 0) url += "&token=" + String(NIGHTSCOUT_TOKEN);
  HTTPClient http;
  http.setTimeout(4000);
  WiFiClientSecure secureClient;
  int code;
  if (NIGHTSCOUT_USE_HTTPS) { secureClient.setInsecure(); http.begin(secureClient, url); }
  else                      { http.begin(url); }
  code = http.GET();
  if (code != 200) { http.end(); return -1; }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload) || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) return -1;
  JsonObject e = doc.as<JsonArray>()[0];
  int v = e["sgv"] | -1;
  uint64_t d = e["date"] | (uint64_t)0;
  if (v <= 0 || d == 0) return -1;
  if (anchorMs - d > 3ULL * 3600ULL * 1000ULL) return -1;   // stale (sensor gap)
  return v;
}

static void drawPanelA1c() {
  String a2 = (g_a1c2w > 0) ? String(g_a1c2w, 1) + "%" : "--";
  String a1 = (g_a1c1m > 0) ? String(g_a1c1m, 1) + "%" : "--";

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("2W", 8, 163, 2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString("1M", 168, 163, 2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(a2, 80, 201, 8);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString(a1, 240, 201, 8);
  tft.setTextDatum(TL_DATUM);
}

static void drawPanelDht() {
  // No TEMP/HUMI labels (user request); values in font 8 (75px),
  // centred vertically in the panel. At this size the temperature
  // is shown as an integer - "24.5" in font 8 plus the unit would
  // be wider than the half panel it has to fit in.
  if (!g_dhtOk) return;
  String ts = String((int)lroundf(g_tempC));
  String hs = String((int)lroundf(g_humi));

  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // temperature group, centred at x=80: digits + degree + C
  int tw = tft.textWidth(ts, 8);
  int tu = 10 + 4 + tft.textWidth("C", 4);   // degree circle + gap + C
  int tx = 80 - (tw + 6 + tu) / 2;
  tft.setTextDatum(ML_DATUM);
  tft.drawString(ts, tx, 201, 8);
  tft.drawCircle(tx + tw + 6 + 4, 176, 4, TFT_WHITE);   // degree mark
  tft.setTextDatum(TL_DATUM);
  tft.drawString("C", tx + tw + 6 + 10, 184, 4);

  // humidity group, centred at x=240: digits + %
  int hw = tft.textWidth(hs, 8);
  int hu = tft.textWidth("%", 4);
  int hx = 240 - (hw + 6 + hu) / 2;
  tft.setTextDatum(ML_DATUM);
  tft.drawString(hs, hx, 201, 8);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("%", hx + hw + 6, 184, 4);
  tft.setTextDatum(TL_DATUM);
}

static void drawBottomPanel() {
  time_t now = time(nullptr);
  int mode = 0;                          // clock until NTP syncs
  if (now > 1700000000) {
    int panelCount = g_dhtOk ? 3 : 2;
    mode = (int)((now / 60) % panelCount);
  }

  if (mode != g_shownPanelMode) {
    g_shownPanelMode = mode;
    tft.fillRect(0, PANEL_TOP, 320, 240 - PANEL_TOP, TFT_BLACK);
    tft.drawLine(0, PANEL_TOP, 319, PANEL_TOP, TFT_DARKGREY);
    // Frame the panel's outer edge. The app does partial redraws
    // everywhere else, so one-shot full-screen drawings (splash,
    // error screens) can leave pixels along the panel border
    // that nothing ever repaints.
    tft.drawRect(0, PANEL_TOP + 1, 320, 239 - PANEL_TOP, TFT_DARKGREY);
    if (mode == 0)      { g_shownClock[0] = '\0'; drawPanelClock(true); }
    else if (mode == 1) { drawPanelA1c(); g_a1cPanelDirty = false; }
    else                { drawPanelDht(); g_shownTemp = (int)lroundf(g_tempC); g_shownHumi = (int)lroundf(g_humi); g_shownDhtOk = g_dhtOk; }
    return;
  }
  if (mode == 0) drawPanelClock(false);
  else if (mode == 1 && g_a1cPanelDirty) {
    tft.fillRect(1, PANEL_TOP + 2, 318, 240 - PANEL_TOP - 4, TFT_BLACK);
    drawPanelA1c();
    g_a1cPanelDirty = false;
  }
  else if (mode == 2) {                  // refresh when readings change
    int t = (int)lroundf(g_tempC), h = (int)lroundf(g_humi);
    if ((g_dhtOk != g_shownDhtOk) || (g_dhtOk && (t != g_shownTemp || h != g_shownHumi))) {
      tft.fillRect(1, PANEL_TOP + 2, 318, 240 - PANEL_TOP - 4, TFT_BLACK);
      drawPanelDht();
      g_shownTemp = t; g_shownHumi = h; g_shownDhtOk = g_dhtOk;
    }
  }
}

// ---- setup / loop ------------------------------------------------
void setup() {
  delay(300);

  tft.init();
  tft.invertDisplay(DISPLAY_INVERT);
  tft.setRotation(1);   // landscape 320x240
  tft.fillScreen(TFT_BLACK);

#ifdef TFT_BL
  // Backlight dimming: PWM on TFT_BL (GPIO21 on this board).
  // The pitch-black screen reported on 2026-10-29 turned out
  // to be an upload from the wrong project, not this code.
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, BACKLIGHT_LEVEL);
#endif
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Nightscout SGV", 20, 20, 4);
  tft.drawString("Connecting WiFi...", 20, 60, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("v8", 292, 88, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // Detect the optional sensor once at startup, allowing time for it
  // to become ready. A missing sensor is not probed again until reboot.
  for (int attempt = 0; attempt < 3; attempt++) {
    float initialTemp, initialHumi;
    if (readDHT22(initialTemp, initialHumi)) {
      g_tempC = initialTemp;
      g_humi = initialHumi;
      g_dhtOk = true;
      break;
    }
    if (attempt < 2) delay(2000);
  }
  g_lastDhtRead = millis();

  connectWiFi();
  fetchNightscout();
  g_lastFetch = millis();

  // Clear the splash completely before the first real draw -
  // otherwise the splash title ("Nightscout SGV" at y=20) leaves
  // leftover pixels ("garbage") along the top of the screen,
  // because the partial redraws never repaint that band.
  tft.fillScreen(TFT_BLACK);
  drawMainArea();
  drawBottomPanel();
}

void loop() {
  unsigned long nowMs = millis();

  // periodic fetch (drawMainArea only repaints what changed)
  if (nowMs - g_lastFetch >= (unsigned long)FETCH_INTERVAL_MS) {
    g_lastFetch = nowMs;
    fetchNightscout();
    drawMainArea();
  }

  // HbA1c sampling round: one anchor request every 300 ms,
  // a new round every 6 hours (needs NTP time for the anchors)
  if (!g_a1cBusy && time(nullptr) > 1700000000 &&
      (g_a1cLastRound == 0 || nowMs - g_a1cLastRound >= 6UL * 3600UL * 1000UL)) {
    g_a1cBusy = true;
    g_a1cIdx = 0;
    g_a1cSum14 = g_a1cSum30 = 0;
    g_a1cN14 = g_a1cN30 = 0;
    g_a1cStartMs = (uint64_t)time(nullptr) * 1000ULL;
    g_a1cLastTick = nowMs;
  }
  if (g_a1cBusy && nowMs - g_a1cLastTick >= 300UL) {
    g_a1cLastTick = nowMs;
    uint64_t anchorMs = g_a1cStartMs - (uint64_t)g_a1cIdx * A1C_SPACING_MS;
    int v = fetchA1cSample(anchorMs);
    if (v > 0) {
      g_a1cSum30 += v; g_a1cN30++;
      if (g_a1cIdx < 28) { g_a1cSum14 += v; g_a1cN14++; }   // newest 28 anchors = 14 days
    }
    if (++g_a1cIdx >= A1C_ANCHORS) {
      g_a1cBusy = false;
      g_a1cLastRound = nowMs;
      if (g_a1cN14 >= 10) g_a1c2w = 3.31f + 0.02392f * (g_a1cSum14 / (float)g_a1cN14);
      if (g_a1cN30 >= 20) g_a1c1m = 3.31f + 0.02392f * (g_a1cSum30 / (float)g_a1cN30);
      g_a1cPanelDirty = true;
    }
  }

  // Keep updating readings only if the sensor was detected at startup.
  if (g_dhtOk && nowMs - g_lastDhtRead >= 2000UL) {
    g_lastDhtRead = nowMs;
    float t, h;
    if (readDHT22(t, h)) {
      g_tempC = t;
      g_humi = h;
    }
  }

  // elapsed time: drawMainArea repaints just that region when
  // the minute value rolls over, and does nothing otherwise
  drawMainArea();

  // bottom panel: clock / HbA1c / temp+humi, rotating
  drawBottomPanel();

  delay(20);
}
