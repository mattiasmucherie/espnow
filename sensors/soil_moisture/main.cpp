#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "espnow_packet.h"

const int PIN_SENSOR_POWER = 4;   // GPIO4 — switches sensor VCC
const int PIN_ADC          = 0;   // GPIO0 — soil ADC

// Calibration (mV). Mirrors the ESPHome lambda (dry=2.622V, wet=0.916V).
const int DRY_MV = 2622;
const int WET_MV =  916;

const uint32_t WARMUP_MS       = 500;
const uint32_t SEND_TIMEOUT_MS = 200;
const uint64_t SLEEP_US        = 30ULL * 60ULL * 1000000ULL;  // 30 min

const char* SENSOR_ID    = "soil_moisture";
const char* SENSOR_NAME  = "Soil Moisture";
const char* SENSOR_MODEL = "ESP32-C3 Soil Moisture";

uint8_t bridgeMac[] = BRIDGE_MAC;

volatile bool sentDone = false;
volatile esp_now_send_status_t sentStatus = ESP_NOW_SEND_FAIL;

void onSent(const wifi_tx_info_t*, esp_now_send_status_t status) {
  sentStatus = status;
  sentDone = true;
}

void sleepNow() {
  Serial.println("Sleeping 30 min");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

int readMoisturePercent() {
  pinMode(PIN_SENSOR_POWER, OUTPUT);
  digitalWrite(PIN_SENSOR_POWER, HIGH);
  delay(WARMUP_MS);

  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts(PIN_ADC);
    delay(2);
  }
  digitalWrite(PIN_SENSOR_POWER, LOW);

  int mv = sum / 16;
  Serial.printf("ADC: %d mV\n", mv);

  long pct = ((long)DRY_MV - mv) * 100L / ((long)DRY_MV - WET_MV);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return (int)pct;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nSoil moisture sensor wake");

  int pct = readMoisturePercent();
  Serial.printf("Moisture: %d%%\n", pct);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  esp_wifi_set_protocol(WIFI_IF_STA,
    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    sleepNow();
  }
  esp_now_register_send_cb(onSent);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bridgeMac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add peer");
    sleepNow();
  }

  JsonDocument doc;
  doc["id"]    = SENSOR_ID;
  doc["name"]  = SENSOR_NAME;
  doc["model"] = SENSOR_MODEL;
  doc["v"]["moisture"]       = pct;
  doc["m"]["moisture"]["u"]  = "%";
  doc["m"]["moisture"]["i"]  = "mdi:water-percent";
  doc["m"]["moisture"]["dc"] = "humidity";

  uint8_t buf[ESPNOW_MAX_JSON];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  esp_err_t r = esp_now_send(bridgeMac, buf, n);
  Serial.printf("Sent (%u bytes) moisture=%d%% -> %s\n",
                (unsigned)n, pct, r == ESP_OK ? "queued" : "ERROR");

  unsigned long t0 = millis();
  while (!sentDone && millis() - t0 < SEND_TIMEOUT_MS) delay(5);
  Serial.printf("TX %s\n",
                sentStatus == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");

  sleepNow();
}

void loop() {}
