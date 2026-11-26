#include <cassert>
#include <iostream>
#include <string>
#include "../../iot/espnode/TelemetryWire.h"
#include "../../iot/espnode/LiveAckCache.h"

int main() {
  const char* sample = R"({"kind":"telemetry","deviceId":"LUXMAP-P001","sessionId":"ba42d2ffde72fccd","sequenceNumber":4294967295,"timestamp":"2026-11-26T16:00:01","mode":"AUTO","autoType":"full_time","lampCommand":"ON","lux":12.345,"voltage":5.123,"current":93.75,"power":480.281,"receptionType":"backfill"})";
  ArduinoJson::JsonDocument data;
  assert(!deserializeJson(data, sample));
  TelemetryWire::encode(data);
  std::string compact;
  serializeJson(data, compact);
  assert(compact.size() < std::string(sample).size() * 0.7);
  ArduinoJson::JsonDocument decoded;
  assert(!deserializeJson(decoded, compact));
  assert(TelemetryWire::decode(decoded));
  assert(decoded["wireVersion"] == 2);
  decoded.remove("wireVersion");
  ArduinoJson::JsonDocument original;
  deserializeJson(original, sample);
  assert(decoded == original);
  deserializeJson(data, R"({"kind":"telemetryAck","deviceId":"LUXMAP-P001","sessionId":"ba42d2ffde72fccd","sequenceNumber":42,"wireVersion":2})");
  TelemetryWire::encode(data);
  assert(data["w"] == "a2");
  assert(TelemetryWire::decode(data));
  assert(data["kind"] == "telemetryAck" && data["sequenceNumber"] == 42);
  deserializeJson(data, sample);
  assert(TelemetryWire::decode(data) && data["wireVersion"].isNull());
  deserializeJson(data, R"({"w":"t2","d":"allowed","deviceId":"other"})");
  assert(!TelemetryWire::decode(data));
  deserializeJson(data, R"({"w":"t3"})");
  assert(!TelemetryWire::decode(data));
  LiveAckCache cache;
  assert(!cache.contains(0, 0));
  cache.add(1, 12);
  assert(cache.contains(1, 12) && !cache.contains(2, 12));
  for (unsigned i = 0; i < LiveAckCache::Capacity; ++i) cache.add(2, i);
  assert(!cache.contains(1, 12)); // Evicted ACKs must be retransmitted, not assumed stored.
  assert(cache.contains(2, 63));
  std::cout << "PASS: compact telemetry/ACK roundtrip, legacy, mixed identity rejection, bounded ACK cache; bytes=" << compact.size() << "\n";
}
