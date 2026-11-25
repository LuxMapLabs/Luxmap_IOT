#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <utility>
#include <vector>
#include "MqttRootCAs.h"
#include <ArduinoJson.h>
#include <esp_system.h>
#include "NetworkConfig.h"
#include "RadioLink.h"
#include "src/PubSubClient/PubSubClient.h"

WiFiClientSecure mqttNetwork;
PubSubClient mqtt(mqttNetwork);
RadioLink radio;
String activeBroker, mqttHost;
unsigned long activeConfigVersion = 0;
uint16_t brokerPort = 8883;
// Shared with MqttBroker.MaxNodesPerGateway. MQTT buffer must fit 32 IDs of 64 bytes.
constexpr size_t MAX_NODES = 32;
constexpr unsigned long COMMAND_RETRY_MIN_MS = 2100;
constexpr unsigned long COMMAND_RETRY_JITTER_MS = 900;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;
constexpr unsigned long NODE_SYNC_INTERVAL_MS = 10000;

struct NodeState {
  String id;
  String desiredCommand, desiredCommandId, acknowledgedCommandId, telemetryAck, sensorHealthAck, commandAck;
  String desiredMode, desiredAutoType;
  uint64_t desiredVersion = 0;
  unsigned long lastCommandTransmit = 0, lastCommandAckPublish = 0;
  unsigned long commandRetryDelay = COMMAND_RETRY_MIN_MS;
  unsigned long commandQueuedAt = 0;
  uint64_t expiresAtUnixSeconds = 0;
  bool subscribed = false;
};

std::vector<NodeState> nodes;
std::vector<String> pendingNodeIds;
bool nodeListPending = false;
size_t radioCursor = 0;
size_t ackCursor = 0;
unsigned int commandsSinceTelemetryAck = 0;
unsigned long lastMqttAttempt = 0, lastHeartbeat = 0, lastNodeRequest = 0;
bool heartbeatSent = false, nodeListReceived = false;

String topic(const String& id, const char* kind) { return String("luxmap/") + id + "/" + kind; }
String gatewayTopic(const char* kind) { return String("luxmap/gateways/") + serverId + "/" + kind; }

void logTelemetry(const char* stage, const JsonDocument& data) {
  const char* deviceId = data["deviceId"] | "";
  const char* sessionId = data["sessionId"] | "";
  const String kind = data["kind"] | "";
  Serial.printf("%s %s device=%s session=%s seq=%lu\n", kind.startsWith("sensorHealth") ? "Sensor health" : "Telemetry", stage,
    validMqttId(deviceId) ? deviceId : "-", validMqttId(sessionId) ? sessionId : "-",
    data["sequenceNumber"] | 0UL);
}

NodeState* findNode(const String& id);
NodeState* findNode(const String& id) {
  for (auto& node : nodes)
    if (node.id == id) return &node;
  return nullptr;
}

void receiveNodeList(const JsonDocument& data) {
  if (String(data["serverId"] | serverId.c_str()) != serverId) return;
  JsonArrayConst ids = data["deviceIds"].as<JsonArrayConst>();
  if (ids.isNull() || ids.size() > MAX_NODES) return;
  std::vector<String> next;
  next.reserve(ids.size());
  for (JsonVariantConst value : ids) {
    const String id = value | "";
    if (!validMqttId(id)) return;
    for (const auto& existing : next)
      if (existing == id) return;
    next.push_back(id);
  }
  pendingNodeIds = std::move(next);
  nodeListPending = true;
}

void subscribeNodes() {
  if (nodeListPending) {
    for (const auto& node : nodes) {
      bool retained = false;
      for (const auto& id : pendingNodeIds)
        if (node.id == id) retained = true;
      if (!retained && node.subscribed &&
          (!mqtt.unsubscribe(topic(node.id, "command").c_str()) ||
           !mqtt.unsubscribe(topic(node.id, "telemetryAck").c_str()) ||
           !mqtt.unsubscribe(topic(node.id, "sensorHealthAck").c_str()))) {
        mqtt.disconnect();
        return;
      }
    }
    std::vector<NodeState> next;
    next.reserve(pendingNodeIds.size());
    for (const auto& id : pendingNodeIds) {
      if (auto* node = findNode(id)) {
        next.push_back(std::move(*node));
      } else {
        next.emplace_back();
        next.back().id = id;
      }
    }
    if (!nodeListReceived || next.size() != nodes.size())
      Serial.printf("MQTT node list synced: %u node(s)\n", static_cast<unsigned>(next.size()));
    nodes = std::move(next);
    nodeListReceived = true;
    nodeListPending = false;
  }
  for (auto& node : nodes) {
    if (node.subscribed) continue;
    if (!mqtt.subscribe(topic(node.id, "command").c_str(), 1) ||
        !mqtt.subscribe(topic(node.id, "telemetryAck").c_str(), 1) ||
        !mqtt.subscribe(topic(node.id, "sensorHealthAck").c_str(), 1)) {
      mqtt.disconnect();
      return;
    }
    node.subscribed = true;
  }
}

void onMqttMessage(char* incomingTopic, byte* payload, unsigned int length) {
  JsonDocument data;
  if (deserializeJson(data, payload, length)) return;
  if (gatewayTopic("nodes") == incomingTopic) {
    receiveNodeList(data);
    return;
  }
  auto* node = findNode(data["deviceId"] | "");
  if (!node) return;
  const String kind = data["kind"] | "";
  if ((kind == "telemetryAck" || kind == "sensorHealthAck") && topic(node->id, kind.c_str()) == incomingTopic) {
    logTelemetry("MQTT ACK received", data);
    String& ack = kind == "sensorHealthAck" ? node->sensorHealthAck : node->telemetryAck;
    ack = "";
    serializeJson(data, ack);
  } else if (kind == "command" && topic(node->id, "command") == incomingTopic) {
    const String id = data["commandId"] | "";
    const String requestedMode = data["mode"] | "";
    const String requestedType = data["autoType"] | "scheduled";
    if (requestedMode == "AUTO") {
      if (requestedType != "scheduled" && requestedType != "full_time") return;
      if (requestedType == "full_time" && !data["autoSchedule"].isNull()) return;
      data["autoType"] = requestedType;
    } else if (requestedMode == "MANUAL") {
      if (!data["autoType"].isNull()) return;
      data.remove("autoType");
    } else return;
    const uint64_t version = data["version"] | uint64_t(0);
    if (id.isEmpty() || (!node->desiredCommand.isEmpty() && version < node->desiredVersion)) return;
    if (node->desiredCommandId != id) {
      node->commandAck = "";
      node->acknowledgedCommandId = "";
      node->desiredCommandId = id;
      node->desiredMode = requestedMode;
      node->desiredAutoType = requestedMode == "AUTO" ? requestedType : "";
      node->desiredVersion = version;
      node->expiresAtUnixSeconds = data["expiresAtUnixSeconds"] | uint64_t(0);
      if (data["autoSchedule"].isNull()) data.remove("autoSchedule");
      if (data["expiresAtUnixSeconds"].isNull()) data.remove("expiresAtUnixSeconds");
      if (data["command"].isNull()) data.remove("command");
      node->desiredCommand = "";
      serializeJson(data, node->desiredCommand);
      node->commandQueuedAt = millis();
      node->lastCommandTransmit = node->commandQueuedAt - node->commandRetryDelay;
      Serial.printf("MQTT command queued for %s id=%s\n", node->id.c_str(), id.c_str());
    }
  }
}

void onRadioPacket(const String& payload) {
  JsonDocument data;
  if (deserializeJson(data, payload)) {
    Serial.println("LoRa frame rejected: invalid JSON");
    return;
  }
  const String kind = data["kind"] | "";
  const bool report = kind == "telemetry" || kind == "sensorHealth";
  // Preserve the node's explicit live/backfill marker; never infer it from network delay.
  if (kind == "telemetry" && !data["receptionType"].isNull()) {
    const String reception = data["receptionType"].as<String>();
    if (reception != "live" && reception != "backfill") {
      Serial.println("LoRa telemetry rejected: invalid receptionType");
      return;
    }
  }
  if (report) logTelemetry("LoRa received", data);
  auto* node = findNode(data["deviceId"] | "");
  if (!node) {
    if (report) logTelemetry("blocked: device not in gateway list", data);
    return;
  }
  if (report) {
    // Only the bridge's durable-storage ACK is relayed to the node.
    if (!mqtt.connected())
      logTelemetry("blocked: MQTT disconnected", data);
    else if (!nodeListReceived)
      logTelemetry("blocked: MQTT node list not synced", data);
    else if (mqtt.publish(topic(node->id, kind.c_str()).c_str(), payload.c_str(), false))
      logTelemetry("MQTT published", data);
    else
      logTelemetry("MQTT publish failed", data);
  } else if (kind == "commandAck" && String(data["commandId"] | "") != node->desiredCommandId) {
    // An older command may have executed before its deadline. Bridge validates its durable ledger.
    if (mqtt.connected()) mqtt.publish(topic(node->id, "commandAck").c_str(), payload.c_str(), false);
  } else if (kind == "commandAck" && !node->desiredCommandId.isEmpty() &&
             String(data["commandId"] | "") == node->desiredCommandId) {
    if (String(data["mode"] | "") != node->desiredMode ||
        (node->desiredMode == "AUTO" && String(data["autoType"] | "scheduled") != node->desiredAutoType)) return;
    node->acknowledgedCommandId = node->desiredCommandId;
    node->commandAck = payload;
    node->lastCommandAckPublish = millis() - 5000;
    Serial.printf("LoRa command confirmed by %s id=%s after %lu ms\n",
      node->id.c_str(), node->acknowledgedCommandId.c_str(), millis() - node->commandQueuedAt);
  }
}

void maintainMqtt() {
  const char* blocked = nullptr;
  if (networkApplyPending) blocked = "applying network settings";
  else if (configuredSsid.isEmpty()) blocked = "Wi-Fi SSID missing";
  else if (WiFi.status() != WL_CONNECTED) blocked = "Wi-Fi not connected";
  else if (!validBrokerAddress(configuredBroker)) blocked = "broker URL must use mqtts://";
  else if (!validMqttId(serverId) || !validMqttSecret(mqttSecret)) blocked = "invalid server ID or secret format";
  if (blocked) {
    if (mqtt.connected()) mqtt.disconnect();
    mqttNetwork.stop();
    static unsigned long lastBlockedLog = 0;
    if (lastBlockedLog == 0 || millis() - lastBlockedLog >= 10000) {
      Serial.printf("MQTT blocked: %s (Wi-Fi status=%d)\n", blocked, WiFi.status());
      lastBlockedLog = millis();
    }
    return;
  }
  if (activeBroker != configuredBroker || activeConfigVersion != configVersion) {
    mqtt.disconnect(); mqttNetwork.stop(); activeBroker = configuredBroker;
    activeConfigVersion = configVersion;
    String address = activeBroker.substring(8); int colon = address.indexOf(':');
    mqttHost = colon < 0 ? address : address.substring(0, colon);
    brokerPort = colon < 0 ? 8883 : address.substring(colon + 1).toInt();
    mqtt.setServer(mqttHost.c_str(), brokerPort); lastMqttAttempt = millis() - 5000;
    // A different broker is a different control source; accept its retained state.
    nodes.clear();
    nodeListPending = false;
    nodeListReceived = false;
  }
  if (!mqtt.connected()) {
    if (millis() - lastMqttAttempt < 5000) return;
    lastMqttAttempt = millis();
    if (time(nullptr) < 1704067200) {
      static bool ntpStarted = false;
      if (!ntpStarted) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        ntpStarted = true;
      }
      Serial.println("MQTT TLS waiting for NTP time");
      return;
    }
    if (!mqttNetwork.connect(mqttHost.c_str(), brokerPort, 3000)) {
      char reason[128] = {};
      const int code = mqttNetwork.lastError(reason, sizeof(reason));
      Serial.printf("MQTT TLS failed host=%s port=%u error=%d %s\n",
        mqttHost.c_str(), static_cast<unsigned int>(brokerPort), code, reason);
      mqttNetwork.stop(); return;
    }
    if (!mqtt.connect(serverId.c_str(), serverId.c_str(), mqttSecret.c_str())) {
      mqttNetwork.stop(); Serial.printf("MQTT failed state=%d\n", mqtt.state()); return;
    }
    nodeListReceived = false;
    nodeListPending = false;
    for (auto& node : nodes) {
      node.subscribed = false;
      node.lastCommandAckPublish = millis() - 5000;
    }
    if (!mqtt.subscribe(gatewayTopic("nodes").c_str(), 1)) {
      mqtt.disconnect();
      return;
    }
    Serial.println("Gateway MQTT connected via TLS; requesting node list");
    lastNodeRequest = millis() - NODE_SYNC_INTERVAL_MS;
    lastHeartbeat = millis() - HEARTBEAT_INTERVAL_MS;
    heartbeatSent = false;
  }
  if (!mqtt.loop()) return;
  if (nodeListPending || nodeListReceived) subscribeNodes();
  if (!mqtt.connected()) return;
  const unsigned long now = millis();
  const unsigned long syncInterval = nodeListReceived ? NODE_SYNC_INTERVAL_MS : 2000;
  if (now - lastNodeRequest >= syncInterval) {
    mqtt.publish(gatewayTopic("nodes/request").c_str(), "1", false);
    lastNodeRequest = now;
  }
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    const String heartbeatTopic = gatewayTopic("heartbeat");
    if (mqtt.publish(heartbeatTopic.c_str(), "1", false)) {
      lastHeartbeat = now;
      if (!heartbeatSent) Serial.printf("MQTT heartbeat sent: %s\n", serverId.c_str());
      heartbeatSent = true;
    }
  }
}
void setup() {
  Serial.begin(115200);
  Serial.println("Gateway firmware: telemetry diagnostics enabled");
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("Wi-Fi disconnected reason=%u\n", static_cast<unsigned int>(info.wifi_sta_disconnected.reason));
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.println("Wi-Fi connected; station has an IP");
  });
  radio.begin(onRadioPacket);
  mqttNetwork.setCACert(MqttRootCAs);
  mqttNetwork.setHandshakeTimeout(5);
  mqtt.setCallback(onMqttMessage); mqtt.setKeepAlive(15); mqtt.setSocketTimeout(1);
  if (!mqtt.setBufferSize(2560)) Serial.println("ERROR: MQTT buffer allocation failed");
  startNetworkConfiguration();
  Serial.printf("LuxMap LoRa gateway %s firmware=gateway-auto-type-v2\n", serverId.c_str());
}

void publishCommandAck() {
  if (!mqtt.connected() || !nodeListReceived) return;
  for (size_t offset = 0; offset < nodes.size(); ++offset) {
    const size_t index = (ackCursor + offset) % nodes.size();
    auto& node = nodes[index];
    if (node.commandAck.isEmpty() || millis() - node.lastCommandAckPublish < 5000) continue;
    mqtt.publish(topic(node.id, "commandAck").c_str(), node.commandAck.c_str(), false);
    node.lastCommandAckPublish = millis();
    ackCursor = (index + 1) % nodes.size();
    return;
  }
}

bool sendTelemetryAck() {
  for (size_t offset = 0; offset < nodes.size(); ++offset) {
    const size_t index = (radioCursor + offset) % nodes.size();
    auto& node = nodes[index];
    const bool healthAckWaiting = !node.sensorHealthAck.isEmpty();
    String& ack = healthAckWaiting ? node.sensorHealthAck : node.telemetryAck;
    if (ack.isEmpty()) continue;
    if (!radio.sendJson(ack)) return false;
    Serial.printf("%s LoRa ACK queued device=%s\n", healthAckWaiting ? "Sensor health" : "Telemetry", node.id.c_str());
    ack = "";
    radioCursor = (index + 1) % nodes.size();
    commandsSinceTelemetryAck = 0;
    return true;
  }
  return false;
}

void sendPendingRadio() {
  if (radio.busy() || (commandsSinceTelemetryAck >= 2 && sendTelemetryAck())) return;
  const auto now = millis();
  for (size_t offset = 0; offset < nodes.size(); ++offset) {
    const size_t index = (radioCursor + offset) % nodes.size();
    auto& node = nodes[index];
    if (node.desiredCommand.isEmpty() || node.desiredCommandId == node.acknowledgedCommandId ||
        now - node.lastCommandTransmit < node.commandRetryDelay) continue;
    if (node.expiresAtUnixSeconds && static_cast<uint64_t>(time(nullptr)) >= node.expiresAtUnixSeconds) {
      node.desiredCommand = ""; // Keep the ID so an execution receipt may still arrive late.
      continue;
    }
    if (radio.sendJson(node.desiredCommand, true)) {
      if (commandsSinceTelemetryAck < 2) ++commandsSinceTelemetryAck;
      radioCursor = (index + 1) % nodes.size();
      node.lastCommandTransmit = millis();
      node.commandRetryDelay = COMMAND_RETRY_MIN_MS + esp_random() % COMMAND_RETRY_JITTER_MS;
      Serial.printf("LoRa command scheduled for %s id=%s after %lu ms\n",
        node.id.c_str(), node.desiredCommandId.c_str(), node.lastCommandTransmit - node.commandQueuedAt);
    }
    return;
  }
  sendTelemetryAck();
}

void reportRadioReceive() {
  static unsigned long previousReport = 0;
  static uint32_t previousBytes = 0;
  const auto now = millis();
  if (now - previousReport < 10000) return;
  const uint32_t bytes = radio.rxByteCount();
  Serial.printf("LoRa UART RX: +%lu bytes in %lu ms; MQTT state=%d; node list=%s (%u)\n",
    static_cast<unsigned long>(bytes - previousBytes), now - previousReport,
    mqtt.state(), nodeListReceived ? "synced" : "pending", static_cast<unsigned>(nodes.size()));
  previousReport = now;
  previousBytes = bytes;
}

void loop() {
  // Service UART before network work; retries are bounded, no reconnect while-loop.
  radio.poll();
  handleNetworkConfiguration();
  maintainMqtt();
  publishCommandAck();
  sendPendingRadio();
  reportRadioReceive();
  delay(2);
}
