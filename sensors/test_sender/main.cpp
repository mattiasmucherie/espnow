#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "espnow_packet.h"

uint8_t bridgeMac[] = BRIDGE_MAC;

const char* SENSOR_ID    = "test_sensor";
const char* SENSOR_NAME  = "Test Sensor";
const char* SENSOR_MODEL = "ESP32-C3 ESP-NOW Sensor";

uint32_t counter = 0;

void onSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  Serial.print("Send: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP-NOW test sender starting...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);

  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  esp_now_register_send_cb(onSent);

  // Force channel AFTER esp_now_init — the C3 needs this order
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bridgeMac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add peer!");
    return;
  }

  uint8_t primary;
  wifi_second_chan_t secondary;
  esp_wifi_get_channel(&primary, &secondary);
  Serial.print("Sender on channel: ");
  Serial.println(primary);

  Serial.println("Ready");
}

void loop() {
  counter++;

  JsonDocument doc;
  doc["id"]    = SENSOR_ID;
  doc["name"]  = SENSOR_NAME;
  doc["model"] = SENSOR_MODEL;
  doc["v"]["counter"]      = counter;
  doc["m"]["counter"]["i"] = "mdi:counter";

  uint8_t buf[ESPNOW_MAX_JSON];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  esp_err_t result = esp_now_send(bridgeMac, buf, n);
  Serial.printf("Sent (%u bytes) counter=%lu -> %s\n",
                (unsigned)n, (unsigned long)counter,
                result == ESP_OK ? "queued" : "ERROR");

  delay(3000);
}
