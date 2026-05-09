#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <set>
#include <string>
#include "secrets.h"
#include "espnow_packet.h"

const char* BRIDGE_ID   = "espnow_bridge";
const char* BRIDGE_NAME = "ESP-NOW Bridge";

const char* TOPIC_AVAILABILITY = "espnow/bridge/availability";
const char* TOPIC_UPTIME       = "espnow/bridge/uptime";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Single-slot inbox written by ESP-NOW callback, drained in loop().
volatile bool packetReady = false;
volatile size_t packetLen = 0;
uint8_t packetBuf[ESPNOW_MAX_JSON + 1];
volatile uint32_t droppedPackets = 0;

// First-sight sets: drives idempotent HA discovery republish per boot.
std::set<std::string> seenDevices;
std::set<std::string> seenMetrics;  // key = "id|metric"

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (packetReady) { droppedPackets++; return; }
  size_t n = len > ESPNOW_MAX_JSON ? ESPNOW_MAX_JSON : (size_t)len;
  memcpy((void*)packetBuf, data, n);
  packetBuf[n] = '\0';
  packetLen = n;
  packetReady = true;
}

void connectWifi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Bridge MAC: ");
  Serial.println(WiFi.macAddress());

  uint8_t primary;
  wifi_second_chan_t secondary;
  esp_wifi_get_channel(&primary, &secondary);
  Serial.print("WiFi channel: ");
  Serial.println(primary);
}

void publishBridgeDiscovery() {
  char bridgeDevice[256];
  snprintf(bridgeDevice, sizeof(bridgeDevice),
    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
    "\"manufacturer\":\"DIY\",\"model\":\"ESP32 ESP-NOW Bridge\"}",
    BRIDGE_ID, BRIDGE_NAME);

  {
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic),
      "homeassistant/binary_sensor/%s/connectivity/config", BRIDGE_ID);
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Connectivity\",\"unique_id\":\"%s_connectivity\","
      "\"state_topic\":\"%s\",\"payload_on\":\"online\",\"payload_off\":\"offline\","
      "\"device_class\":\"connectivity\",%s}",
      BRIDGE_ID, TOPIC_AVAILABILITY, bridgeDevice);
    mqtt.publish(topic, payload, true);
  }

  {
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic),
      "homeassistant/sensor/%s/uptime/config", BRIDGE_ID);
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Uptime\",\"unique_id\":\"%s_uptime\","
      "\"state_topic\":\"%s\",\"unit_of_measurement\":\"s\","
      "\"icon\":\"mdi:timer-outline\",\"availability_topic\":\"%s\",%s}",
      BRIDGE_ID, TOPIC_UPTIME, TOPIC_AVAILABILITY, bridgeDevice);
    mqtt.publish(topic, payload, true);
  }

  Serial.println("Bridge discovery published");
}

void publishSensorDiscovery(const char* id, const char* name, const char* model,
                            const char* metric, const char* unit,
                            const char* icon, const char* deviceClass) {
  char deviceJson[320];
  snprintf(deviceJson, sizeof(deviceJson),
    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
    "\"manufacturer\":\"DIY\",\"model\":\"%s\",\"via_device\":\"%s\"}",
    id, name, model, BRIDGE_ID);

  char extra[160] = "";
  size_t off = 0;
  if (unit && *unit) {
    off += snprintf(extra + off, sizeof(extra) - off,
                    ",\"unit_of_measurement\":\"%s\"", unit);
  }
  if (icon && *icon) {
    off += snprintf(extra + off, sizeof(extra) - off,
                    ",\"icon\":\"%s\"", icon);
  }
  if (deviceClass && *deviceClass) {
    off += snprintf(extra + off, sizeof(extra) - off,
                    ",\"device_class\":\"%s\"", deviceClass);
  }

  char topic[160], payload[768];
  snprintf(topic, sizeof(topic),
    "homeassistant/sensor/%s/%s/config", id, metric);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
    "\"state_topic\":\"espnow/%s/%s\","
    "\"availability_topic\":\"%s\"%s,%s}",
    metric, id, metric, id, metric, TOPIC_AVAILABILITY, extra, deviceJson);

  mqtt.publish(topic, payload, true);
  Serial.printf("Discovery: %s/%s\n", id, metric);
}

void connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT... ");
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                     TOPIC_AVAILABILITY, 0, true, "offline")) {
      Serial.println("connected!");
      mqtt.publish(TOPIC_AVAILABILITY, "online", true);
      publishBridgeDiscovery();
    } else {
      Serial.print("rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  esp_now_register_recv_cb(onEspNowReceive);
  Serial.println("ESP-NOW receiver ready");
}

void handlePacket() {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)packetBuf, packetLen);
  packetReady = false;

  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return;
  }

  const char* id = doc["id"];
  if (!id || !*id) {
    Serial.println("packet missing id");
    return;
  }

  const char* name  = doc["name"]  | id;
  const char* model = doc["model"] | "ESP-NOW Sensor";

  if (seenDevices.insert(id).second) {
    Serial.printf("New device: %s\n", id);
  }

  JsonObjectConst v = doc["v"].as<JsonObjectConst>();
  JsonObjectConst m = doc["m"].as<JsonObjectConst>();

  if (v.isNull() || v.size() == 0) {
    Serial.println("packet has no values");
    return;
  }

  for (JsonPairConst kv : v) {
    const char* metric = kv.key().c_str();

    std::string metricKey = std::string(id) + "|" + metric;
    if (seenMetrics.insert(metricKey).second) {
      const char* unit = "";
      const char* icon = "";
      const char* dc   = "";
      if (!m.isNull()) {
        JsonObjectConst mm = m[metric].as<JsonObjectConst>();
        if (!mm.isNull()) {
          unit = mm["u"]  | "";
          icon = mm["i"]  | "";
          dc   = mm["dc"] | "";
        }
      }
      publishSensorDiscovery(id, name, model, metric, unit, icon, dc);
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "espnow/%s/%s", id, metric);

    char valBuf[32];
    serializeJson(kv.value(), valBuf, sizeof(valBuf));
    mqtt.publish(topic, valBuf);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP-NOW MQTT Bridge starting...");
  connectWifi();
  WiFi.setSleep(false);
  esp_wifi_set_protocol(WIFI_IF_STA,
    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  connectMqtt();
  setupEspNow();
}

void loop() {
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  if (packetReady) handlePacket();

  static unsigned long lastUptime = 0;
  if (millis() - lastUptime > 10000) {
    lastUptime = millis();
    char msg[16];
    snprintf(msg, sizeof(msg), "%lu", millis() / 1000);
    mqtt.publish(TOPIC_UPTIME, msg);

    if (droppedPackets > 0) {
      Serial.printf("Dropped packets (since last log): %lu\n",
                    (unsigned long)droppedPackets);
      droppedPackets = 0;
    }
  }
}
