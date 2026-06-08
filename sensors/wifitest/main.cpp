// Throwaway antenna/RF diagnostic. Connects to WiFi, reports RSSI + channel,
// and does one HTTP GET to prove end-to-end internet. RSSI at close range is
// the real antenna-health signal: healthy ~ -40..-55 dBm, detuned ~ -75..-90.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nwifitest: connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("FAILED to associate within 20s (weak/broken antenna or wrong creds)");
    return;
  }

  Serial.printf("Connected: IP=%s  channel=%d  RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());

  HTTPClient http;
  http.begin("http://example.com/");
  int code = http.GET();
  Serial.printf("HTTP GET example.com -> %d  (internet %s)\n",
                code, code == 200 ? "OK" : "FAIL");
  http.end();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("RSSI: %d dBm  (ch %d)\n", WiFi.RSSI(), WiFi.channel());
  } else {
    Serial.println("disconnected");
  }
  delay(2000);
}
