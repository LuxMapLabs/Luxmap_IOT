#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "NetworkConfig.h"
#include "RadioLink.h"
#include "src/PubSubClient/PubSubClient.h"

WiFiClient mqttNetwork;
PubSubClient mqtt(mqttNetwork);
RadioLink radio;
String activeBroker, mqttHost;
unsigned long activeConfigVersion = 0;
uint16_t brokerPort = 1883;
String desiredCommand, desiredCommandId, acknowledgedCommandId, telemetryAck, nodeCommandAck;
uint64_t desiredVersion = 0;
unsigned long lastMqttAttempt = 0, lastWiFiAttempt = 0, lastCommandTransmit = 0, lastCommandAckPublish = 0;
const unsigned long COMMAND_RETRY_MIN_MS = 3500;
const unsigned long COMMAND_RETRY_JITTER_MS = 1500;
unsigned long commandRetryDelay = COMMAND_RETRY_MIN_MS;
const unsigned long HEARTBEAT_INTERVAL_MS = 10000;
unsigned long lastHeartbeat = 0;

String topic(const char* kind) { return String("luxmap/") + nodeId + "/" + kind; }
void onMqttMessage(char* incomingTopic, byte* payload, unsigned int length) {
  JsonDocument data;
  if (deserializeJson(data, payload, length) || String(data["deviceId"] | "") != nodeId) return;
  if (topic("telemetryAck") == incomingTopic && String(data["kind"] | "") == "telemetryAck") {
    telemetryAck = ""; serializeJson(data, telemetryAck);
  } else if (topic("command") == incomingTopic && String(data["kind"] | "") == "command") {
    String id = data["commandId"] | "";
    uint64_t version = data["version"] | uint64_t(0);
    if (id.isEmpty() || (!desiredCommand.isEmpty() && version < desiredVersion)) return;
    if (desiredCommandId != id) {
      nodeCommandAck = ""; acknowledgedCommandId = "";
      desiredCommandId = id; desiredVersion = version;
      desiredCommand = ""; serializeJson(data, desiredCommand);
      lastCommandTransmit = millis() - 15000;
      Serial.printf("MQTT command queued for LoRa node id=%s\n", id.c_str());
    }
  }
}
void onRadioPacket(const String& payload) {
  JsonDocument data;
  if (deserializeJson(data, payload) || String(data["deviceId"] | "") != nodeId) return;
  String kind = data["kind"] | "";
  if (kind == "telemetry") {
    // No local ACK. Only the backend's durable-storage ACK is relayed to the node.
    if (mqtt.connected() && mqtt.publish(topic("telemetry").c_str(), payload.c_str(), false))
      Serial.println("LoRa telemetry forwarded to MQTT; awaiting server ACK");
    else Serial.println("MQTT unavailable: node retains SD record and will retry");
  } else if (kind == "commandAck" && String(data["commandId"] | "") == desiredCommandId) {
    acknowledgedCommandId = desiredCommandId;
    nodeCommandAck = payload; lastCommandAckPublish = millis() - 5000;
    Serial.printf("Node confirmed command over LoRa id=%s\n", acknowledgedCommandId.c_str());
  }
}
void maintainMqtt() {
  if (networkApplyPending || WiFi.status() != WL_CONNECTED || configuredBroker.isEmpty() ||
      !validMqttId(gatewayId) || !validMqttId(nodeId) || mqttUsername != gatewayId || !validMqttSecret(mqttSecret)) {
    if (mqtt.connected()) mqtt.disconnect();
    mqttNetwork.stop(); return;
  }
  if (activeBroker != configuredBroker || activeConfigVersion != configVersion) {
    mqtt.disconnect(); mqttNetwork.stop(); activeBroker = configuredBroker;
    activeConfigVersion = configVersion;
    String address = activeBroker.substring(7); int colon = address.indexOf(':');
    mqttHost = colon < 0 ? address : address.substring(0, colon);
    brokerPort = colon < 0 ? 1883 : address.substring(colon + 1).toInt();
    mqtt.setServer(mqttHost.c_str(), brokerPort); lastMqttAttempt = millis() - 5000;
    // A different broker is a different control source; accept its retained state.
    desiredCommand = ""; desiredCommandId = ""; acknowledgedCommandId = ""; desiredVersion = 0;
    nodeCommandAck = ""; telemetryAck = "";
  }
  if (!mqtt.connected()) {
    if (millis() - lastMqttAttempt < 5000) return;
    lastMqttAttempt = millis();
    if (!mqttNetwork.connect(mqttHost.c_str(), brokerPort, 1000) ||
        !mqtt.connect(gatewayId.c_str(), mqttUsername.c_str(), mqttSecret.c_str())) {
      mqttNetwork.stop(); Serial.printf("MQTT failed state=%d\n", mqtt.state()); return;
    }
    if (!mqtt.subscribe(topic("command").c_str(), 1) || !mqtt.subscribe(topic("telemetryAck").c_str(), 1)) {
      mqtt.disconnect(); return;
    }
    Serial.println("Gateway MQTT connected: command + telemetryAck subscribed");
    lastCommandAckPublish = millis() - 5000;
    lastHeartbeat = millis() - HEARTBEAT_INTERVAL_MS;
  }
  if (!mqtt.loop()) return;
  const unsigned long now = millis();
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    const String heartbeatTopic = String("luxmap/gateways/") + gatewayId + "/heartbeat";
    if (mqtt.publish(heartbeatTopic.c_str(), "1", false)) lastHeartbeat = now;
  }
}
void setup() {
  Serial.begin(115200);
  radio.begin(onRadioPacket);
  mqtt.setCallback(onMqttMessage); mqtt.setKeepAlive(15); mqtt.setSocketTimeout(1);
  if (!mqtt.setBufferSize(1536)) Serial.println("ERROR: MQTT buffer allocation failed");
  startNetworkConfiguration();
  Serial.printf("LuxMap LoRa gateway %s, node %s\n", gatewayId.c_str(), nodeId.c_str());
}
void loop() {
  // Service UART before network work; retries are bounded, no reconnect while-loop.
  radio.poll(); handleNetworkConfiguration();
  if (!networkApplyPending && WiFi.status() != WL_CONNECTED && !configuredSsid.isEmpty() &&
      millis() - lastWiFiAttempt >= 10000) {
    lastWiFiAttempt = millis(); WiFi.disconnect();
    WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
  }
  maintainMqtt();
  if (mqtt.connected() && !nodeCommandAck.isEmpty() && millis() - lastCommandAckPublish >= 5000) {
    mqtt.publish(topic("commandAck").c_str(), nodeCommandAck.c_str(), false);
    lastCommandAckPublish = millis();
  }
  if (!radio.busy() && !desiredCommand.isEmpty() && desiredCommandId != acknowledgedCommandId &&
             millis() - lastCommandTransmit >= commandRetryDelay) {
    if (radio.sendJson(desiredCommand, true)) {
      lastCommandTransmit = millis();
      commandRetryDelay = COMMAND_RETRY_MIN_MS + esp_random() % COMMAND_RETRY_JITTER_MS;
      Serial.printf("LoRa command scheduled id=%s\n", desiredCommandId.c_str());
    }
  } else if (!radio.busy() && !telemetryAck.isEmpty()) {
    if (radio.sendJson(telemetryAck)) telemetryAck = "";
  }
  delay(2);
}
