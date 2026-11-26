#pragma once
#include <ArduinoJson.h>

// Only the radio representation changes. SD and MQTT keep the public JSON fields.
namespace TelemetryWire {
struct Field { const char* full; const char* shortName; };
constexpr Field fields[] = {
  {"deviceId", "d"}, {"sessionId", "s"}, {"sequenceNumber", "q"},
  {"timestamp", "t"}, {"mode", "m"}, {"lampCommand", "c"},
  {"lux", "l"}, {"voltage", "v"}, {"current", "i"}, {"power", "p"},
  {"receptionType", "r"}, {"autoType", "u"}
};
inline void encode(JsonDocument& data) {
  const char* kind = data["kind"] | "";
  if (strcmp(kind, "telemetry") && strcmp(kind, "telemetryAck")) return;
  data["w"] = strcmp(kind, "telemetry") == 0 ? "t2" : "a2";
  data.remove("kind");
  data.remove("wireVersion");
  for (const auto& field : fields) {
    if (!data[field.full].isNull()) data[field.shortName] = data[field.full];
    data.remove(field.full);
  }
}
inline bool decode(JsonDocument& data) {
  if (data["w"].isNull()) return true; // Legacy peer.
  const char* wire = data["w"] | "";
  if (strcmp(wire, "t2") && strcmp(wire, "a2")) return false;
  // Reject mixed encodings instead of allowing one identity to override another.
  if (!data["kind"].isNull()) return false;
  for (const auto& field : fields)
    if (!data[field.full].isNull()) return false;
  data["kind"] = strcmp(wire, "t2") == 0 ? "telemetry" : "telemetryAck";
  data.remove("w");
  for (const auto& field : fields) {
    if (!data[field.shortName].isNull()) data[field.full] = data[field.shortName];
    data.remove(field.shortName);
  }
  data["wireVersion"] = 2;
  return true;
}
}
