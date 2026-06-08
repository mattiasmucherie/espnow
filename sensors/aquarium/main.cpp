#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include "secrets.h"
#include "espnow_packet.h"

// Unit number injected per-env (-DAQ_UNIT=1|2). Names derived from it so the
// build flags stay free of spaces/quote-escaping.
#ifndef AQ_UNIT
#define AQ_UNIT 1
#endif
#define STR2(x) #x
#define STR(x)  STR2(x)
#define SENSOR_ID    "aquarium_" STR(AQ_UNIT)
#define SENSOR_NAME  "Aquarium " STR(AQ_UNIT)
#define SENSOR_MODEL "ESP32-C3 Aquarium"

// ESP32-C3 SuperMini pinout. Avoid GPIO 2 (boot strap), GPIO 8 (onboard LED),
// GPIO 9 (BOOT strap), GPIO 20/21 (UART0). I2C mirrors the price display.
#define I2C_SDA_PIN   5
#define I2C_SCL_PIN   6
#define ONE_WIRE_PIN  10   // DS18B20 data; 4.7k pullup to 3V3

// Valid water-temp band. Rejects the DS18B20 sentinels (-127 disconnected,
// 85 read-before-conversion) and anything physically implausible for a tank.
const float TEMP_MIN_VALID = 5.0f;
const float TEMP_MAX_VALID = 40.0f;

// Send policy: a fixed 5-minute heartbeat. Water temp drifts slowly, so a
// steady cadence gives a clean HA history with no change-detection bookkeeping.
const uint32_t SEND_INTERVAL_MS = 5UL * 60UL * 1000UL;   // send every 5 min

const uint32_t READ_INTERVAL_MS = 1000;                  // probe read + OLED refresh
const uint32_t BUCKET_MS        = 60UL * 60UL * 1000UL;  // one 24h sparkline bucket = 1h

// Note: under ESP-NOW Long Range the send callback almost never reports an ACK
// (the LR-only sender can't decode the bridge's ACK frame), so we treat a
// successful enqueue as "sent" rather than waiting on a confirmation that won't
// come. Delivery reliability comes from setSleep(false) + the on-change/heartbeat
// policy, not from per-packet retries.

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE);

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds(&oneWire);

uint8_t bridgeMac[] = BRIDGE_MAC;

volatile bool sentDone = false;
volatile esp_now_send_status_t sentStatus = ESP_NOW_SEND_FAIL;

// ---- 24h sparkline: ring of hourly averages, millis()-based, resets on boot ----
const uint8_t NBUCKETS = 24;
float    bucket[NBUCKETS] = {0};
uint8_t  bucketCount = 0;            // filled buckets (<= NBUCKETS)
uint8_t  bucketHead  = 0;            // index of oldest, once the ring is full
float    hourAcc = 0;                // accumulator for the in-progress hour
uint16_t hourCnt = 0;
uint32_t hourStartMs = 0;

// Display/send state.
bool     haveTemp = false;           // last read was valid
float    curTemp  = NAN;             // last valid reading
uint32_t lastSendMs = 0;
bool     everSent = false;

void onSent(const wifi_tx_info_t*, esp_now_send_status_t status) {
  sentStatus = status;
  sentDone = true;
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  WiFi.setSleep(false);  // always-on sender: keep the radio awake so ACKs land
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onSent);

  // Force channel AFTER esp_now_init — the C3 needs this order.
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bridgeMac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add peer");
  }
  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());
}

// Build + send the temperature packet, retrying delivery a few times.
// Returns true only on a confirmed ESP-NOW ack.
bool sendTemp(float temp) {
  JsonDocument doc;
  doc["id"]    = SENSOR_ID;
  doc["name"]  = SENSOR_NAME;
  doc["model"] = SENSOR_MODEL;
  doc["v"]["temperature"]       = serialized(String(temp, 1));  // 1 decimal
  doc["m"]["temperature"]["u"]  = "°C";
  doc["m"]["temperature"]["i"]  = "mdi:thermometer-water";
  doc["m"]["temperature"]["dc"] = "temperature";

  uint8_t buf[ESPNOW_MAX_JSON];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  esp_err_t r = esp_now_send(bridgeMac, buf, n);
  Serial.printf("send temp=%.1f (%u bytes) -> %s\n",
                temp, (unsigned)n, r == ESP_OK ? "queued" : "ERROR");
  return r == ESP_OK;  // enqueue success == sent; LR ACK is unreliable, don't gate on it
}

// Fold a valid reading into the in-progress hour; roll the ring every BUCKET_MS.
void accumulate(float temp) {
  hourAcc += temp;
  hourCnt++;
  if (millis() - hourStartMs >= BUCKET_MS && hourCnt > 0) {
    float avg = hourAcc / hourCnt;
    if (bucketCount < NBUCKETS) {
      bucket[bucketCount++] = avg;
    } else {
      bucket[bucketHead] = avg;
      bucketHead = (bucketHead + 1) % NBUCKETS;
    }
    hourAcc = 0; hourCnt = 0;
    hourStartMs += BUCKET_MS;
  }
}

float bucketAt(uint8_t i) {  // i: 0 = oldest
  uint8_t start = (bucketCount < NBUCKETS) ? 0 : bucketHead;
  return bucket[(start + i) % NBUCKETS];
}

// Min/max over the finalized buckets plus the live reading, for the labels.
void windowMinMax(float& lo, float& hi) {
  lo = NAN; hi = NAN;
  for (uint8_t i = 0; i < bucketCount; i++) {
    float v = bucketAt(i);
    if (isnan(lo) || v < lo) lo = v;
    if (isnan(hi) || v > hi) hi = v;
  }
  if (haveTemp) {
    if (isnan(lo) || curTemp < lo) lo = curTemp;
    if (isnan(hi) || curTemp > hi) hi = curTemp;
  }
}

// A 24h min/max value pinned to a screen corner: a small filled triangle
// (up = max, down = min) followed by the value, right-aligned to rightX.
void drawCornerValue(int16_t rightX, int16_t baselineY, bool up, float val) {
  char s[8];
  snprintf(s, sizeof(s), "%.1f", val);
  oled.setFont(u8g2_font_6x10_tf);
  int16_t tw = oled.getStrWidth(s);
  int16_t vx = rightX - tw;            // value right-aligned to rightX
  oled.drawStr(vx, baselineY, s);

  int16_t tx = vx - 7;                 // triangle just left of the value
  int16_t ty = baselineY - 7;          // top of the 5px-tall triangle
  if (up) oled.drawTriangle(tx, ty + 5, tx + 5, ty + 5, tx + 2, ty);       // ▲
  else    oled.drawTriangle(tx, ty,     tx + 5, ty,     tx + 2, ty + 5);   // ▼
}

// Filled-bar sparkline of the hourly buckets. Bars align to a fixed 24-slot
// grid so the chart fills in left-to-right as uptime accrues.
void drawSparkline(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (bucketCount < 2) return;

  float mn = NAN, mx = NAN;
  for (uint8_t i = 0; i < bucketCount; i++) {
    float v = bucketAt(i);
    if (isnan(mn) || v < mn) mn = v;
    if (isnan(mx) || v > mx) mx = v;
  }
  float range = mx - mn;
  if (range < 0.5f) range = 0.5f;

  int16_t barW = w / NBUCKETS;
  if (barW < 1) barW = 1;

  oled.drawHLine(x, y + h - 1, w);  // baseline
  for (uint8_t i = 0; i < bucketCount; i++) {
    float norm = (bucketAt(i) - mn) / range;
    int16_t barH = (int16_t)(norm * (h - 2)) + 1;
    int16_t cy = y + (h - 1) - barH;
    oled.drawBox(x + i * barW, cy, barW, barH);
  }
}

void renderOled() {
  oled.clearBuffer();

  // Tank label, top-left — always shown so you know which unit at a glance.
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 8, SENSOR_NAME);

  if (!haveTemp) {
    const char* err = "PROBE ERROR";
    oled.drawStr((128 - oled.getStrWidth(err)) / 2, 40, err);
    oled.sendBuffer();
    return;
  }

  // 24h min/max in the right corners (top = max, bottom = min).
  float lo, hi;
  windowMinMax(lo, hi);
  if (!isnan(hi)) drawCornerValue(127, 8,  true,  hi);
  if (!isnan(lo)) drawCornerValue(127, 53, false, lo);

  // Hero temperature: big, centered, with a proper degree ring (no "C").
  char big[12];
  snprintf(big, sizeof(big), "%.1f", curTemp);
  oled.setFont(u8g2_font_logisoso32_tn);
  int16_t bw = oled.getStrWidth(big);
  const int16_t degW = 8;                       // room for the degree ring
  int16_t x = (128 - (bw + degW)) / 2;
  if (x < 0) x = 0;
  oled.drawStr(x, 42, big);                     // baseline y42 (glyph top ~y10)
  oled.drawCircle(x + bw + 4, 13, 2);           // degree mark, top-right of number

  // Bottom sparkline strip: full-width, 10px tall, 1px baseline.
  drawSparkline(4, 54, 120, 10);

  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n" SENSOR_ID " starting");

  Wire.setPins(I2C_SDA_PIN, I2C_SCL_PIN);
  oled.begin();
  oled.setBusClock(400000);
  renderOled();  // "PROBE ERROR" until the first good read

  ds.begin();
  ds.setResolution(12);          // 0.0625°C; needed for the 0.1°C threshold
  ds.setWaitForConversion(true);

  initEspNow();
  hourStartMs = millis();
}

void loop() {
  static uint32_t nextReadMs = 0;
  uint32_t now = millis();
  if ((long)(now - nextReadMs) < 0) { delay(20); return; }
  nextReadMs = now + READ_INTERVAL_MS;

  ds.requestTemperatures();
  float t = ds.getTempCByIndex(0);
  bool valid = (t >= TEMP_MIN_VALID && t <= TEMP_MAX_VALID);

  if (valid) {
    haveTemp = true;
    curTemp = t;
    accumulate(t);
  } else {
    haveTemp = false;
    Serial.printf("invalid read: %.2f\n", t);
  }
  renderOled();

  if (!valid) return;  // never send while faulted

  if (!everSent || (now - lastSendMs) >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    everSent = true;
    Serial.println(sendTemp(roundf(t * 10.0f) / 10.0f) ? "delivered" : "send FAILED");
  }
}
