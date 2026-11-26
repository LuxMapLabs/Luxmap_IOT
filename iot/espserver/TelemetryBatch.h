#pragma once
#include <ArduinoJson.h>
#include <cmath>

namespace TelemetryBatch {
constexpr size_t Capacity = 4;
constexpr size_t MaxPayload = 473; // Existing 480-byte complete-frame path, including CRC framing.
inline bool matchesReceipt(const JsonDocument& sample, const JsonDocument& ack) {
  return sample["sequenceNumber"].is<uint32_t>() && ack["sequenceNumber"].is<uint32_t>() &&
    sample["deviceId"].is<const char*>() && sample["sessionId"].is<const char*>() &&
    strcmp(sample["deviceId"] | "", ack["deviceId"] | "") == 0 &&
    strcmp(sample["sessionId"] | "", ack["sessionId"] | "") == 0 &&
    sample["sequenceNumber"].as<uint32_t>() == ack["sequenceNumber"].as<uint32_t>();
}
inline bool number(JsonVariantConst value) {
  return !value.isNull() && value.is<double>() && std::isfinite(value.as<double>());
}
inline void begin(JsonDocument& batch, const char* device, const char* session) {
  batch.clear(); batch["w"] = "b3"; batch["d"] = device; batch["s"] = session;
  batch["b"].to<JsonArray>();
}
// All samples share identity but retain their own sequence, clock and readings.
inline bool append(JsonDocument& batch, const JsonDocument& sample) {
  JsonArray rows = batch["b"].as<JsonArray>();
  if (rows.size() >= Capacity || !sample["sequenceNumber"].is<uint32_t>() ||
      !sample["timestamp"].is<const char*>() ||
      strcmp(sample["deviceId"] | "", batch["d"] | "") ||
      strcmp(sample["sessionId"] | "", batch["s"] | "")) return false;
  const char* mode = sample["mode"] | "";
  const char* lamp = sample["lampCommand"] | "";
  const char* reception = sample["receptionType"] | "";
  const char* type = sample["autoType"] | "scheduled";
  if ((strcmp(mode, "AUTO") && strcmp(mode, "MANUAL")) ||
      (strcmp(lamp, "ON") && strcmp(lamp, "OFF")) ||
      (strcmp(reception, "live") && strcmp(reception, "backfill")) ||
      (!strcmp(mode, "AUTO") && strcmp(type, "scheduled") && strcmp(type, "full_time"))) return false;
  for (const char* key : {"lux", "voltage", "current", "power"})
    if (!number(sample[key])) return false;
  uint8_t flags = !strcmp(lamp, "ON") ? 1 : 0;
  if (!strcmp(mode, "AUTO")) { flags |= 2; if (!strcmp(type, "full_time")) flags |= 4; }
  if (!strcmp(reception, "backfill")) flags |= 8;
  JsonArray row = rows.add<JsonArray>();
  row.add(sample["sequenceNumber"]); row.add(sample["timestamp"]); row.add(flags);
  for (const char* key : {"lux", "voltage", "current", "power"}) row.add(sample[key]);
  if (measureJson(batch) > MaxPayload) { rows.remove(rows.size() - 1); return false; }
  return true;
}
// Validate the entire packet before publishing any row to MQTT.
inline bool expand(const JsonDocument& batch, JsonDocument& samples) {
  samples.clear();
  JsonArrayConst rows = batch["b"].as<JsonArrayConst>();
  if (strcmp(batch["w"] | "", "b3") || !batch["d"].is<const char*>() ||
      !batch["s"].is<const char*>() || rows.isNull() || !rows.size() ||
      rows.size() > Capacity || measureJson(batch) > MaxPayload) return false;
  uint32_t sequences[Capacity] = {};
  size_t index = 0;
  for (JsonArrayConst row : rows) {
    if (row.isNull() || row.size() != 7 || !row[0].is<uint32_t>() ||
        !row[1].is<const char*>() || !row[2].is<uint8_t>()) return false;
    const uint8_t flags = row[2];
    if (flags > 15 || ((flags & 4) && !(flags & 2))) return false;
    for (size_t j = 3; j < 7; ++j) if (!number(row[j])) return false;
    sequences[index] = row[0];
    for (size_t j = 0; j < index; ++j) if (sequences[j] == sequences[index]) return false;
    ++index;
  }
  JsonArray result = samples.to<JsonArray>();
  for (JsonArrayConst row : rows) {
    const uint8_t flags = row[2];
    JsonObject sample = result.add<JsonObject>();
    sample["kind"] = "telemetry"; sample["deviceId"] = batch["d"];
    sample["sessionId"] = batch["s"]; sample["sequenceNumber"] = row[0];
    sample["timestamp"] = row[1]; sample["mode"] = flags & 2 ? "AUTO" : "MANUAL";
    sample["lampCommand"] = flags & 1 ? "ON" : "OFF";
    if (flags & 2) sample["autoType"] = flags & 4 ? "full_time" : "scheduled";
    sample["receptionType"] = flags & 8 ? "backfill" : "live";
    sample["lux"] = row[3]; sample["voltage"] = row[4];
    sample["current"] = row[5]; sample["power"] = row[6];
  }
  return true;
}
inline bool validAck(const JsonDocument& ack) {
  JsonArrayConst ids = ack["q"].as<JsonArrayConst>();
  if (strcmp(ack["w"] | "", "k3") || !ack["d"].is<const char*>() ||
      !ack["s"].is<const char*>() || ids.isNull() || !ids.size() || ids.size() > Capacity) return false;
  for (JsonVariantConst id : ids) if (!id.is<uint32_t>()) return false;
  return true;
}
}
