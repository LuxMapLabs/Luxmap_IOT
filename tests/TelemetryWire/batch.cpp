#include <cassert>
#include <iostream>
#include <string>
#include "../../iot/espnode/TelemetryBatch.h"

int main() {
  JsonDocument batch;
  TelemetryBatch::begin(batch, "LUXMAP-P001", "164698c933a6d962");
  JsonDocument originals;
  JsonArray original = originals.to<JsonArray>();
  for (uint32_t i = 0; i < 4; ++i) {
    JsonDocument sample;
    sample["kind"] = "telemetry"; sample["deviceId"] = "LUXMAP-P001";
    sample["sessionId"] = "164698c933a6d962"; sample["sequenceNumber"] = 100 + i;
    sample["timestamp"] = std::string("2026-11-26T15:00:0") + char('1' + i);
    sample["mode"] = i ? "AUTO" : "MANUAL";
    sample["lampCommand"] = i % 2 ? "OFF" : "ON";
    if (i) sample["autoType"] = i == 1 ? "scheduled" : "full_time";
    sample["receptionType"] = i == 3 ? "backfill" : "live";
    sample["lux"] = 15.125 + i; sample["voltage"] = 5.101 + i * 0.01;
    sample["current"] = 93.75 + i * 1.25; sample["power"] = 478.219 + i;
    assert(TelemetryBatch::append(batch, sample));
    original.add(sample.as<JsonObjectConst>());
  }
  std::string wire;
  serializeJson(batch, wire);
  assert(wire.size() <= TelemetryBatch::MaxPayload);
  JsonDocument received;
  assert(!deserializeJson(received, wire));
  JsonDocument samples;
  assert(TelemetryBatch::expand(received, samples));
  std::string fullWire; serializeJson(originals, fullWire);
  JsonDocument expected; deserializeJson(expected, fullWire);
  if (samples != expected) { serializeJson(samples, std::cerr); std::cerr << "\n" << fullWire << "\n"; }
  assert(samples.size() == 4 && samples == expected);
  JsonDocument fifth;
  fifth.set(original[0]);
  assert(!TelemetryBatch::append(batch, fifth));
  received["b"][3][0] = 100; // A duplicate sequence is rejected atomically.
  assert(!TelemetryBatch::expand(received, samples) && samples.isNull());
  deserializeJson(received, wire);
  received["b"][2][3] = nullptr;
  assert(!TelemetryBatch::expand(received, samples) && samples.isNull());
  deserializeJson(received, wire);
  received["b"][0][2] = 4; // full_time without AUTO is invalid.
  assert(!TelemetryBatch::expand(received, samples));
  JsonDocument ack;
  deserializeJson(ack, R"({"w":"k3","d":"LUXMAP-P001","s":"164698c933a6d962","q":[100,102]})");
  assert(TelemetryBatch::validAck(ack)); // Partial and out-of-order receipts are valid.
  assert(ack["q"].size() == 2); // The receiver must retain 101 and 103.
  JsonDocument receipt;
  receipt["deviceId"] = "LUXMAP-P001"; receipt["sessionId"] = "164698c933a6d962";
  receipt["sequenceNumber"] = 100;
  assert(TelemetryBatch::matchesReceipt(fifth, receipt));
  receipt["sequenceNumber"] = 101;
  assert(!TelemetryBatch::matchesReceipt(fifth, receipt));
  receipt["sequenceNumber"] = 100; receipt["sessionId"] = "old-session";
  assert(!TelemetryBatch::matchesReceipt(fifth, receipt));
  receipt["sessionId"] = "164698c933a6d962"; receipt["deviceId"] = "other-device";
  assert(!TelemetryBatch::matchesReceipt(fifth, receipt));
  ack["q"][0] = "100";
  assert(!TelemetryBatch::validAck(ack));
  deserializeJson(ack, R"({"w":"k3","d":"LUXMAP-P001","s":"164698c933a6d962","q":[]})");
  assert(!TelemetryBatch::validAck(ack));
  // Oversized records do not leave a partially appended row.
  TelemetryBatch::begin(batch, "LUXMAP-P001", "164698c933a6d962");
  fifth["timestamp"] = std::string(500, 'x');
  assert(!TelemetryBatch::append(batch, fifth) && batch["b"].size() == 0);
  fifth.set(original[0]); fifth["sessionId"] = "another-session";
  assert(!TelemetryBatch::append(batch, fifth));
  fifth.set(original[0]);
  for (const char* key : {"lux", "voltage", "current", "power"}) fifth[key] = 0;
  assert(TelemetryBatch::append(batch, fifth)); // OFF and voltage-loss zeroes are real samples.
  assert(TelemetryBatch::expand(batch, samples));
  assert(samples[0]["voltage"] == 0 && samples[0]["current"] == 0);
  std::cout << "PASS: 4 raw samples roundtrip (" << wire.size()
    << " bytes), mode/time/value preservation, atomic rejection, partial ACK and frame size\n";
}
