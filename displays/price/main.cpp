#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "secrets.h"

// Mälarenergi spot-price endpoint. Public, no auth. Returns ~700 15-min slots
// in öre/kWh plus a `current` pointer to the slot for "now".
#define PRICE_API_URL  "https://bff.malarenergi.se/spotpriser/api/v1/prices/area/SE3"
#define AREA_LABEL     "SE3"
#define SLOTS_NEEDED   12                          // 3h × 4 slots/h (averaging window)
#define SLOTS_GRAPH    96                          // 24h × 4 slots/h — full day, midnight to midnight local
#define RETRY_MS       (30UL * 1000UL)             // backoff on fetch failure / stale slot
#define POST_SLOT_GRACE_S  30                      // wait this long past :00/:15/:30/:45 before fetching

// ESP32-C3 SuperMini pinout. Avoid GPIO 2 (boot strap), GPIO 8 (onboard LED),
// GPIO 9 (BOOT button strap), GPIO 20/21 (UART0).
#define I2C_SDA_PIN    5
#define I2C_SCL_PIN    6

// Traffic-light LEDs (active-HIGH). Add a ~330Ω series resistor per LED.
#define LED_GREEN_PIN  3
#define LED_YELLOW_PIN 4
#define LED_RED_PIN    10

// Deadband around the week average (fraction). Inside this band → yellow,
// so a price hovering at the mean doesn't flicker between green/red.
#define LIGHT_DEADBAND 0.10f

// SSD1306 over hardware I2C. Wire pins are remapped in setup() via
// Wire.setPins(SDA, SCL) before oled.begin().
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE);

// Blocking stream wrapper. ArduinoJson reads byte-by-byte from the HTTP
// stream; over TLS the underlying client can momentarily drain to 0 bytes
// mid-body. The default Stream::read() returns -1 in that case, and the
// parser bails with IncompleteInput. This waits for more bytes (up to
// timeoutMs) before declaring EOF.
class BlockingStream : public Stream {
public:
  BlockingStream(Stream& s, uint32_t timeoutMs = 10000) : s_(s), to_(timeoutMs) {}
  int available() override { return s_.available(); }
  int read() override {
    uint32_t t = millis();
    while (!s_.available()) {
      if (millis() - t > to_) return -1;
      delay(1);
    }
    return s_.read();
  }
  int peek() override {
    uint32_t t = millis();
    while (!s_.available()) {
      if (millis() - t > to_) return -1;
      delay(1);
    }
    return s_.peek();
  }
  size_t write(uint8_t) override { return 0; }
private:
  Stream& s_;
  uint32_t to_;
};

enum LightState : uint8_t { LIGHT_OFF, LIGHT_GREEN, LIGHT_YELLOW, LIGHT_RED };

struct Snapshot {
  bool  valid = false;
  float currentPrice = NAN;
  float avgNext3h = NAN;
  float weekAverage = NAN;    // API's top-level `average` over the ~7d window it returns
  uint8_t slotsUsed = 0;
  char  windowFirst[8] = "";  // "HH:MM" of first slot in 3h window (kept for stale-check)
  char  windowLast[8]  = "";  // "HH:MM" of end of last slot in 3h window
  float graphPrices[SLOTS_GRAPH] = {0};  // today's prices, slot 0 = local midnight
  uint8_t graphCount = 0;                // how many of the 96 are populated
  uint8_t nowSlotIdx = 0xFF;             // 0..95 — index of the current local slot in graphPrices; 0xFF = unknown
  // Hourly summary used by the sparkline + min/max labels.
  float hourlyPrices[24] = {0};
  uint8_t hourlyCount = 0;
  float hourlyMin = NAN;
  float hourlyMax = NAN;
};
Snapshot snap;

LightState lightFor(const Snapshot& s) {
  if (!s.valid || isnan(s.avgNext3h) || isnan(s.weekAverage) || s.weekAverage <= 0) {
    return LIGHT_OFF;
  }
  float band = s.weekAverage * LIGHT_DEADBAND;
  if (s.avgNext3h < s.weekAverage - band) return LIGHT_GREEN;
  if (s.avgNext3h > s.weekAverage + band) return LIGHT_RED;
  return LIGHT_YELLOW;
}

void applyLight(LightState st) {
  digitalWrite(LED_GREEN_PIN,  st == LIGHT_GREEN  ? HIGH : LOW);
  digitalWrite(LED_YELLOW_PIN, st == LIGHT_YELLOW ? HIGH : LOW);
  digitalWrite(LED_RED_PIN,    st == LIGHT_RED    ? HIGH : LOW);
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf(" %s\n", WiFi.localIP().toString().c_str());
}

// Block up to timeoutMs for SNTP to set the system clock. Returns true if
// time is now valid (> 2024). Don't fail hard if it doesn't — the loop will
// just fall back to the retry timer until time eventually arrives.
bool waitForTime(uint32_t timeoutMs) {
  unsigned long t0 = millis();
  while (time(nullptr) < 1700000000L) {
    if (millis() - t0 > timeoutMs) return false;
    delay(200);
  }
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  Serial.printf("time sync: %04d-%02d-%02d %02d:%02d:%02d local\n",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                lt.tm_hour, lt.tm_min, lt.tm_sec);
  return true;
}

// Milliseconds from now until the next wall-clock quarter-hour boundary
// (HH:00, HH:15, HH:30, HH:45) plus POST_SLOT_GRACE_S. Returns 0 if time isn't
// synced yet — caller treats that as "fetch now."
unsigned long msUntilNextQuarter() {
  time_t now = time(nullptr);
  if (now < 1700000000L) return 0;
  struct tm lt; localtime_r(&now, &lt);
  int secIntoQuarter = (lt.tm_min % 15) * 60 + lt.tm_sec;
  int secsToBoundary = 15 * 60 - secIntoQuarter;
  return (unsigned long)(secsToBoundary + POST_SLOT_GRACE_S) * 1000UL;
}

// "HH:MM" of the current wall-clock 15-min slot (local). Empty if no time sync.
void expectedSlotHHMM(char* dst, size_t dstLen) {
  time_t now = time(nullptr);
  if (now < 1700000000L) { if (dstLen) dst[0] = '\0'; return; }
  struct tm lt; localtime_r(&now, &lt);
  snprintf(dst, dstLen, "%02d:%02d", lt.tm_hour, (lt.tm_min / 15) * 15);
}

// UTC ISO ("YYYY-MM-DDTHH:MM:00Z") of the current 15-min slot, computed from
// our own clock. Used to look up the right interval in the API response —
// the server's `current` field can lag the actual boundary by minutes (CDN
// or lazy refresh), but the underlying `intervals` data has all the slots.
void expectedSlotUtcIso(char* dst, size_t dstLen) {
  time_t now = time(nullptr);
  if (now < 1700000000L) { if (dstLen) dst[0] = '\0'; return; }
  time_t slot = now - (now % (15 * 60));  // 15-min slots align across UTC and any whole-hour TZ
  struct tm utc; gmtime_r(&slot, &utc);
  snprintf(dst, dstLen, "%04d-%02d-%02dT%02d:%02d:00Z",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min);
}

// UTC ISO of *local* midnight today (00:00:00 in the configured TZ). In CEST
// this is e.g. 22:00:00Z the previous day. Used to anchor the day-graph.
void localMidnightUtcIso(char* dst, size_t dstLen) {
  time_t now = time(nullptr);
  if (now < 1700000000L) { if (dstLen) dst[0] = '\0'; return; }
  struct tm lt; localtime_r(&now, &lt);
  lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
  lt.tm_isdst = -1;          // let the lib decide DST for this local midnight
  time_t mid = mktime(&lt);   // mktime: local broken-down → UTC time_t
  struct tm utc; gmtime_r(&mid, &utc);
  snprintf(dst, dstLen, "%04d-%02d-%02dT%02d:%02d:00Z",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min);
}

// Days between 1970-01-01 and (y, m, d), proleptic Gregorian. Howard Hinnant's
// `days_from_civil` — handles any date in [0000-03-01, ∞) correctly.
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468;
}

// Parse "YYYY-MM-DDTHH:MM:SS..." (UTC, the API uses Z suffix) and write the
// **local** HH:MM into dst. Timezone set once in setup() via tzset().
static void localHHMMFromUtcIso(char* dst, size_t dstLen, const char* iso) {
  if (!iso || strlen(iso) < 19) { if (dstLen) dst[0] = '\0'; return; }
  int y, mo, d, h, mi, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
    if (dstLen) dst[0] = '\0'; return;
  }
  time_t t = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400L
             + h * 3600 + mi * 60 + s;
  struct tm local;
  localtime_r(&t, &local);
  snprintf(dst, dstLen, "%02d:%02d", local.tm_hour, local.tm_min);
}

bool fetchPrices(Snapshot& out) {
  WiFiClientSecure client;
  client.setInsecure();  // hobby project: no cert pinning

  HTTPClient http;
  http.useHTTP10(true);  // disable chunked encoding so the stream is parseable straight
  http.setTimeout(15000);
  if (!http.begin(client, PRICE_API_URL)) { Serial.println("http.begin failed"); return false; }

  int code = http.GET();
  if (code != 200) { Serial.printf("HTTP %d\n", code); http.end(); return false; }

  // Filter: only keep the fields we need. Drops ~80% of the 92KB payload before DOM.
  JsonDocument filter;
  filter["average"]                    = true;
  filter["current"]["startDateTime"]   = true;
  filter["current"]["price"]           = true;
  filter["intervals"][0]["startDateTime"] = true;
  filter["intervals"][0]["endDateTime"]   = true;
  filter["intervals"][0]["price"]      = true;

  BlockingStream bs(http.getStream(), 10000);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, bs,
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("JSON: %s\n", err.c_str()); return false; }

  const char* curStart = doc["current"]["startDateTime"];
  float curPrice = doc["current"]["price"] | NAN;

  // Prefer our own clock to locate the slot — server's `current` is sometimes
  // stale by minutes (CDN/lazy refresh). Fall back to `current.startDateTime`
  // only if NTP hasn't synced yet.
  char wantIso[24];
  expectedSlotUtcIso(wantIso, sizeof(wantIso));
  const char* targetIso = (wantIso[0] != '\0') ? wantIso : curStart;
  if (!targetIso) { Serial.println("no slot target"); return false; }

  JsonArrayConst intervals = doc["intervals"].as<JsonArrayConst>();
  size_t startIdx = 0;
  bool found = false;
  for (size_t i = 0; i < intervals.size(); i++) {
    const char* s = intervals[i]["startDateTime"];
    if (s && strcmp(s, targetIso) == 0) { startIdx = i; found = true; break; }
  }
  if (!found) {
    Serial.printf("target slot %s not in intervals\n", targetIso);
    return false;
  }

  size_t endIdx = startIdx + SLOTS_NEEDED;
  if (endIdx > intervals.size()) endIdx = intervals.size();

  float sum = 0.f; uint8_t count = 0;
  for (size_t i = startIdx; i < endIdx; i++) {
    sum += intervals[i]["price"].as<float>();
    count++;
  }
  if (count == 0) return false;

  // Sparkline: full day, starting at local midnight today.
  out.graphCount = 0;
  out.nowSlotIdx = 0xFF;
  char midIso[24];
  localMidnightUtcIso(midIso, sizeof(midIso));
  if (midIso[0]) {
    size_t midIdx = 0;
    bool foundMid = false;
    for (size_t i = 0; i < intervals.size(); i++) {
      const char* s = intervals[i]["startDateTime"];
      if (s && strcmp(s, midIso) == 0) { midIdx = i; foundMid = true; break; }
    }
    if (foundMid) {
      for (size_t i = midIdx; i < intervals.size() && out.graphCount < SLOTS_GRAPH; i++) {
        out.graphPrices[out.graphCount++] = intervals[i]["price"].as<float>();
      }
    }
    // nowSlotIdx straight from local time: hours*4 + minutes/15.
    time_t tnow = time(nullptr);
    if (tnow >= 1700000000L) {
      struct tm lt; localtime_r(&tnow, &lt);
      out.nowSlotIdx = (uint8_t)(lt.tm_hour * 4 + lt.tm_min / 15);
    }

    // Hourly aggregation + min/max — used by sparkline and the side labels.
    out.hourlyCount = 0;
    out.hourlyMin = NAN; out.hourlyMax = NAN;
    float hAcc[24] = {0};
    uint8_t hCnt[24] = {0};
    for (uint8_t i = 0; i < out.graphCount; i++) {
      uint8_t hi = i / 4;
      if (hi >= 24) break;
      hAcc[hi] += out.graphPrices[i];
      hCnt[hi]++;
    }
    for (uint8_t hi = 0; hi < 24; hi++) {
      if (hCnt[hi]) {
        float v = hAcc[hi] / hCnt[hi];
        out.hourlyPrices[hi] = v;
        out.hourlyCount = hi + 1;
        if (isnan(out.hourlyMin) || v < out.hourlyMin) out.hourlyMin = v;
        if (isnan(out.hourlyMax) || v > out.hourlyMax) out.hourlyMax = v;
      }
    }
  }

  out.valid = true;
  // Pull `current` price from the slot we picked, not the API's `current`
  // field (which can be stale by minutes — same reason we don't trust its
  // pointer).
  out.currentPrice = intervals[startIdx]["price"] | curPrice;
  out.avgNext3h = sum / count;
  out.weekAverage = doc["average"] | NAN;
  out.slotsUsed = count;

  // Window labels: convert UTC ISO timestamps from the API → local HH:MM
  // (timezone configured once in setup() via tzset).
  localHHMMFromUtcIso(out.windowFirst, sizeof(out.windowFirst),
                      intervals[startIdx]["startDateTime"]);
  localHHMMFromUtcIso(out.windowLast,  sizeof(out.windowLast),
                      intervals[endIdx - 1]["endDateTime"]);

  Serial.printf("avg next %u slots = %.2f öre/kWh (%s→%s), 7d avg = %.2f\n",
                out.slotsUsed, out.avgNext3h, out.windowFirst, out.windowLast,
                out.weekAverage);
  return true;
}

// Sparkline: 24 hourly bars (today midnight→midnight) from snap.hourlyPrices.
// Past + current hour as filled bars; future hours as a step-outline (top edge
// + vertical connectors at boundaries). Caller should size w as a multiple
// of 24 (96 → 4px per hour).
void drawSparkline(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (snap.hourlyCount == 0 || isnan(snap.hourlyMin)) return;

  float mn = snap.hourlyMin, mx = snap.hourlyMax;
  float range = mx - mn;
  if (range < 1.0f) range = 1.0f;

  int16_t barW = w / 24;
  if (barW < 1) barW = 1;

  oled.drawHLine(x, y + h - 1, w);  // baseline

  auto topY = [&](uint8_t hi) -> int16_t {
    float norm = (snap.hourlyPrices[hi] - mn) / range;
    int16_t barH = (int16_t)(norm * (h - 2)) + 1;
    return y + (h - 1) - barH;
  };

  uint8_t hourCount = snap.hourlyCount;
  uint8_t nowHour = (snap.nowSlotIdx < SLOTS_GRAPH)
                    ? (uint8_t)(snap.nowSlotIdx / 4)
                    : (uint8_t)(hourCount - 1);
  if (nowHour >= hourCount) nowHour = hourCount - 1;

  // Past + current hour: filled bars.
  for (uint8_t hi = 0; hi <= nowHour; hi++) {
    int16_t cy = topY(hi);
    int16_t barH = (y + h - 1) - cy;
    if (barH > 0) oled.drawBox(x + hi * barW, cy, barW, barH);
    else          oled.drawHLine(x + hi * barW, cy, barW);
  }

  // Upcoming: top edge of each bar + vertical step at hour boundaries.
  for (uint8_t hi = nowHour; hi + 1 < hourCount; hi++) {
    int16_t yA = topY(hi);
    int16_t yB = topY(hi + 1);
    int16_t bx = x + (hi + 1) * barW;
    oled.drawHLine(bx, yB, barW);            // top edge of next hour
    int16_t yLo = (yA < yB) ? yA : yB;
    int16_t yHi = (yA < yB) ? yB : yA;
    if (yHi > yLo) oled.drawVLine(bx, yLo, yHi - yLo + 1);
  }
}

void renderOled() {
  oled.clearBuffer();

  if (!snap.valid) {
    oled.setFont(u8g2_font_logisoso24_tn);
    oled.drawStr(46, 38, "--");
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 9, "no data");
    oled.sendBuffer();
    return;
  }

  // Header (y 0..10): 7d avg on the left, current price on the right.
  oled.setFont(u8g2_font_6x10_tf);
  if (!isnan(snap.weekAverage)) {
    char wk[16];
    snprintf(wk, sizeof(wk), "7d %.0f", snap.weekAverage);
    oled.drawStr(0, 9, wk);
  }
  char nu[16];
  snprintf(nu, sizeof(nu), "nu %.0f", snap.currentPrice);
  int16_t nw = oled.getStrWidth(nu);
  oled.drawStr(128 - nw, 9, nu);

  // Big number (y 11..35): avg next 3h, 1 decimal, centered.
  char big[16];
  snprintf(big, sizeof(big), "%.1f", snap.avgNext3h);
  oled.setFont(u8g2_font_logisoso24_tn);
  int16_t w = oled.getStrWidth(big);
  oled.drawStr((128 - w) / 2, 35, big);

  // Sparkline (y 37..62): 96px wide, 26px tall, centered. Boundary
  // filled→outline = "now."
  drawSparkline(16, 37, 96, 26);

  // Tiny min/max labels in the left margin, aligned to the chart edges:
  // high near the top, low at the bottom — matches a y-axis convention.
  if (!isnan(snap.hourlyMin) && !isnan(snap.hourlyMax)) {
    oled.setFont(u8g2_font_4x6_tf);
    char hi[8], lo[8];
    snprintf(hi, sizeof(hi), "%.0f", snap.hourlyMax);
    snprintf(lo, sizeof(lo), "%.0f", snap.hourlyMin);
    oled.drawStr(0, 42, hi);   // baseline near top of sparkline (top of glyph at y=37)
    oled.drawStr(0, 62, lo);   // baseline at bottom of sparkline
  }

  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nprice_display starting");

  // Sweden: CET (UTC+1) / CEST (UTC+2), DST last-Sun-Mar → last-Sun-Oct.
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  pinMode(LED_GREEN_PIN,  OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_RED_PIN,    OUTPUT);
  applyLight(LIGHT_OFF);

  Wire.setPins(I2C_SDA_PIN, I2C_SCL_PIN);
  oled.begin();
  oled.setBusClock(400000);
  renderOled();  // shows "--" while we boot

  connectWifi();

  // SNTP. configTzTime sets TZ + starts the NTP background sync in one go,
  // so we don't need the earlier setenv/tzset (kept harmless for clarity).
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
  waitForTime(10000);
  // First fetch happens on the first loop() pass (nextFetchAtMs = 0).
}

void loop() {
  static unsigned long nextFetchAtMs = 0;  // millis() target for the next fetch
  static unsigned long lastOk = 0;
  unsigned long now = millis();

  // Signed compare so wrap-around is safe (~49 days uptime).
  if ((long)(now - nextFetchAtMs) < 0) { delay(100); return; }

  if (WiFi.status() != WL_CONNECTED) connectWifi();
  bool ok = fetchPrices(snap);
  if (ok) {
    lastOk = now;
    renderOled();
    applyLight(lightFor(snap));
  } else if (snap.valid) {
    Serial.printf("fetch failed, last ok %lus ago\n", (now - lastOk) / 1000);
  } else {
    renderOled();
  }

  // Stale detection: if the server's `current` slot doesn't match our
  // wall-clock quarter, the server hasn't rolled over yet (or our clock has
  // drifted ahead). Either way, retry soon rather than waiting a full quarter.
  bool stale = false;
  if (ok) {
    char expected[8];
    expectedSlotHHMM(expected, sizeof(expected));
    if (expected[0] && strcmp(expected, snap.windowFirst) != 0) {
      stale = true;
      Serial.printf("stale slot: got %s, expected %s\n", snap.windowFirst, expected);
    }
  }

  if (ok && !stale) {
    unsigned long wait = msUntilNextQuarter();
    if (wait == 0) wait = RETRY_MS;
    nextFetchAtMs = now + wait;
    Serial.printf("next fetch in %lus\n", wait / 1000);
  } else {
    nextFetchAtMs = now + RETRY_MS;
  }
  delay(100);
}
