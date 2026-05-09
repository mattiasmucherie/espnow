#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include "secrets.h"
#include "espnow_packet.h"

const char* BRIDGE_ID   = "espnow_bridge";
const char* BRIDGE_NAME = "ESP-NOW Bridge";

const char* TEST_SENSOR_ID   = "test_sensor";
const char* TEST_SENSOR_NAME = "Test Sensor";

const char* TOPIC_AVAILABILITY = "espnow/bridge/availability";
const char* TOPIC_UPTIME       = "espnow/bridge/uptime";
const char* TOPIC_TEST_COUNTER = "espnow/test_sensor/counter";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

volatile bool packetReady = false;
test_payload_t lastPacket;

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  Serial.print("ESP-NOW packet from ");
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
    info->src_addr[0], info->src_addr[1], info->src_addr[2],
    info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  Serial.print(" len=");
  Serial.println(len);

  if (len == sizeof(test_payload_t)) {
    memcpy((void*)&lastPacket, data, sizeof(test_payload_t));
    packetReady = true;
  }
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

void publishDiscoveryConfigs() {
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

  char testDevice[256];
  snprintf(testDevice, sizeof(testDevice),
    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
    "\"manufacturer\":\"DIY\",\"model\":\"ESP32-C3 ESP-NOW Sensor\","
    "\"via_device\":\"%s\"}",
    TEST_SENSOR_ID, TEST_SENSOR_NAME, BRIDGE_ID);

  {
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic),
      "homeassistant/sensor/%s/counter/config", TEST_SENSOR_ID);
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Counter\",\"unique_id\":\"%s_counter\","
      "\"state_topic\":\"%s\",\"icon\":\"mdi:counter\","
      "\"availability_topic\":\"%s\",%s}",
      TEST_SENSOR_ID, TOPIC_TEST_COUNTER, TOPIC_AVAILABILITY, testDevice);
    mqtt.publish(topic, payload, true);
  }

  Serial.println("Discovery configs published");
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
      publishDiscoveryConfigs();
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

  if (packetReady) {
    packetReady = false;
    char msg[16];
    snprintf(msg, sizeof(msg), "%lu", (unsigned long)lastPacket.counter);
    mqtt.publish(TOPIC_TEST_COUNTER, msg);
    Serial.print("Received counter=");
    Serial.println(lastPacket.counter);
  }

  static unsigned long lastUptime = 0;
  if (millis() - lastUptime > 10000) {
    lastUptime = millis();
    char msg[16];
    snprintf(msg, sizeof(msg), "%lu", millis() / 1000);
    mqtt.publish(TOPIC_UPTIME, msg);
  }
}
