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
  // A normal telemetry frame fits below the E32 UART buffer bound in one write.
  RadioLink telemetrySender;
  telemetrySender.begin(capture);
  const String longTelemetry(std::string(330, 'x'));
  Serial2.tx.clear();
  assert(telemetrySender.sendJson(longTelemetry));
  telemetrySender.poll();
  assert(Serial2.tx.size() == RadioLink::frame(longTelemetry).length());
  assert(!telemetrySender.quietFor(1500));
  for (int i = 0; telemetrySender.busy() && i < 30; ++i) {
    testTime += RadioLink::CHUNK_INTERVAL_MS;
    telemetrySender.poll();
  }
  assert(!telemetrySender.busy());
  assert(RadioLink::decode(String(Serial2.tx), decoded));
  assert(decoded.value == longTelemetry.value);
  // Larger frames fall back to bounded chunks; never overflow the module buffer.
  const String largerTelemetry(std::string(600, 'y'));
  Serial2.tx.clear();
  assert(telemetrySender.sendJson(largerTelemetry));
  telemetrySender.poll();
  assert(Serial2.tx.size() == RadioLink::DATA_BURST_LIMIT);
  for (int i = 0; telemetrySender.busy() && i < 30; ++i) {
    testTime += RadioLink::CHUNK_INTERVAL_MS;
    telemetrySender.poll();
  }
  assert(!telemetrySender.busy());
  assert(RadioLink::decode(String(Serial2.tx), decoded));
  assert(decoded.value == largerTelemetry.value);
  assert(!sender.sendJson(String(std::string(RadioLink::MAX_FRAME, 'x'))));

  constexpr unsigned long quietDuration = 500;
  testTime += quietDuration;
  assert(receiver.quietFor(quietDuration));
  auxState = LOW;
  assert(!receiver.quietFor(quietDuration));
  receiver.poll();
  testTime += quietDuration;
  auxState = HIGH;
  receiver.poll();
  assert(!receiver.quietFor(quietDuration));
  testTime += quietDuration;
  assert(receiver.quietFor(quietDuration));

  Serial2.rx.push_back('@');
  assert(!receiver.quietFor(quietDuration));
  receiver.poll();
  testTime += quietDuration;
  receiver.poll();
  assert(!receiver.quietFor(quietDuration));
  for (char c : encoded.value.substr(1)) Serial2.rx.push_back(c);
  receiver.poll();
  assert(receivedCount == 3 && !receiver.quietFor(quietDuration));
  testTime += quietDuration;
  assert(receiver.quietFor(quietDuration));

  assert(receiver.sendJson(json, true) && !receiver.quietFor(quietDuration));
  receiver.poll();
  testTime += RadioLink::CHUNK_INTERVAL_MS;
  receiver.poll();
  receiver.poll();
  assert(!receiver.busy() && !receiver.quietFor(quietDuration));
  testTime += quietDuration;
  assert(receiver.quietFor(quietDuration));
  std::cout << "PASS: RadioLink framing, AUX gating, TX/RX and quiet windows\n";
}
