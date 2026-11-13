#include <cassert>
#include <iostream>
#include "../../iot/espnode/RadioLink.h"

std::string received;
int receivedCount = 0;
void capture(const String& payload) { received = payload.value; ++receivedCount; }
int main() {
  assert(RadioLink::crc("123456789") == 0x29B1); // CRC-16/CCITT-FALSE reference vector.
  const String json("{\"kind\":\"telemetry\",\"deviceId\":\"LUXMAP-P001\",\"sequenceNumber\":1}");
  const String encoded = RadioLink::frame(json);
  String decoded;
  assert(RadioLink::decode(encoded, decoded) && decoded.value == json.value);
  String broken = encoded; broken.value[5] ^= 1;
  assert(!RadioLink::decode(broken, decoded));
  assert(!RadioLink::decode("@{}*ZZZZ\n", decoded));
  RadioLink sender; sender.begin(capture);
  auxState = LOW;
  assert(sender.sendJson(json)); sender.poll(); assert(Serial2.tx.empty());
  assert(!sender.sendJson(json)); // One bounded TX buffer, no silent overwrite.
  auxState = HIGH;
  for (int i = 0; sender.busy() && i < 30; ++i) { testTime += 300; sender.poll(); }
  assert(!sender.busy() && Serial2.tx == encoded.value);
  RadioLink receiver; receiver.begin(capture);
  for (char c : encoded.value) { Serial2.rx.push_back(c); receiver.poll(); }
  assert(received == json.value && receivedCount == 1);
  for (char c : broken.value) Serial2.rx.push_back(c);
  receiver.poll(); assert(receivedCount == 1);
  // New start delimiter resynchronizes an interrupted frame.
  for (char c : std::string("@partial") + encoded.value) Serial2.rx.push_back(c);
  receiver.poll(); assert(receivedCount == 2);
  assert(!sender.sendJson(String(std::string(RadioLink::MAX_FRAME, 'x'))));
  std::cout << "PASS: actual RadioLink CRC, corruption rejection, AUX gating, chunked send, fragmented receive, resync and buffer bounds\n";
}
