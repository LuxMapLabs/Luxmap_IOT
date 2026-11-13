#pragma once
#include "src/PubSubClient/PubSubClient.h"

WiFiClient mqttNetwork;
PubSubClient mqtt(mqttNetwork);
String activeBroker;
String mqttHost;
String commandTopic;
String telemetryTopic;
unsigned long lastMqttAttempt = 0;
bool telemetryDue = false;

void setLamp(bool turnOn);
void runAutomaticControl();

void onMqttCommand(char* topic, byte* payload, unsigned int length) {
  if (commandTopic != topic) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("Invalid MQTT command JSON");
    return;
  }
  const char* mode = doc["mode"] | "";
  const char* command = doc["command"] | "";
  if (strcmp(mode, "AUTO") == 0) {
    controlMode = MODE_AUTO;
    runAutomaticControl();
  } else if (strcmp(mode, "MANUAL") == 0 &&
             (strcmp(command, "ON") == 0 || strcmp(command, "OFF") == 0)) {
    controlMode = MODE_MANUAL;
    manualLampCommand = strcmp(command, "ON") == 0;
    setLamp(manualLampCommand);
  } else {
    Serial.println("Rejected invalid MQTT mode/command");
    return;
  }
  telemetryDue = true;
  Serial.printf("MQTT command: %s %s\n", mode, command);
}

void setupMqtt() {
  commandTopic = String("luxmap/") + DEVICE_ID + "/command";
  telemetryTopic = String("luxmap/") + DEVICE_ID + "/telemetry";
  mqtt.setCallback(onMqttCommand);
  if (!mqtt.setBufferSize(1024)) Serial.println("ERROR: MQTT buffer allocation failed");
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(1);
}

void maintainMqtt() {
  if (networkApplyPending || WiFi.status() != WL_CONNECTED || configuredBroker.isEmpty()) {
    if (mqtt.connected()) mqtt.disconnect();
    mqttNetwork.stop();
    return;
  }
  if (activeBroker != configuredBroker) {
    mqtt.disconnect();
    mqttNetwork.stop();
    activeBroker = configuredBroker;
    String authority = activeBroker.substring(7);
    int colon = authority.indexOf(':');
    mqttHost = colon < 0 ? authority : authority.substring(0, colon);
    uint16_t port = colon < 0 ? 1883 : authority.substring(colon + 1).toInt();
    mqtt.setServer(mqttHost.c_str(), port);
    lastMqttAttempt = millis() - 5000;
  }
  if (!mqtt.connected()) {
    if (millis() - lastMqttAttempt < 5000) return;
    lastMqttAttempt = millis();
    String authority = activeBroker.substring(7);
    int colon = authority.indexOf(':');
    uint16_t port = colon < 0 ? 1883 : authority.substring(colon + 1).toInt();
    // Bound TCP connection attempts; never wait in a reconnect loop.
    if (!mqttNetwork.connect(mqttHost.c_str(), port, 1000) || !mqtt.connect(DEVICE_ID)) {
      mqttNetwork.stop();
      Serial.printf("MQTT connection failed, state=%d; retry in 5s\n", mqtt.state());
      return;
    }
    if (!mqtt.subscribe(commandTopic.c_str(), 1)) {
      mqtt.disconnect();
      Serial.println("MQTT subscription failed");
      return;
    }
    Serial.println("MQTT connected, subscribed to command");
    telemetryDue = true;
  }
  mqtt.loop();
}
