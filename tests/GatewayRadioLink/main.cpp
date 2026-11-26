#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL: " << #condition << " at line " << __LINE__ << "\n"; return 1; } } while (false)
#include <iostream>
#include "../../iot/espserver/RadioLink.h"

static int received = 0;
void capture(const String&) { ++received; }

int main() {
  RadioLink radio;
  radio.begin(capture);
  // AUTO schedule + expiry make real command frames exceed the old 256-byte shortcut.
  const String command(std::string(330, 'c'));
  CHECK(radio.sendJson(command, true));
  radio.poll();
  CHECK(Serial2.tx.size() == RadioLink::frame(command).length());
  // Respect real AUX busy, then leave an additional window for RF completion and the ACK.
  auxState = LOW;
  testTime += 5000;
  radio.poll();
  CHECK(radio.busy());
  auxState = HIGH;
  radio.poll();
  CHECK(!radio.busy());
  CHECK(!radio.sendJson(command, true)); // Retry must leave room for the node's ACK.
  testTime += 1499;
  radio.poll();
  CHECK(!radio.sendJson(command, true));

  // Receive callbacks still run during the post-TX listen interval.
  const String ack = RadioLink::frame("{\"kind\":\"commandAck\"}");
  for (char c : ack.value) Serial2.rx.push_back(c);
  radio.poll();
  CHECK(received == 1 && radio.rxByteCount() == ack.length());
  testTime += 100;
  radio.poll();
  // Storage ACK is terminal: no artificial 1.5 s reply wait after it.
  CHECK(radio.sendJson("{\"kind\":\"telemetryAck\"}"));
  radio.poll();
  auxState = LOW;
  radio.poll();
  CHECK(radio.busy());
  auxState = HIGH;
  radio.poll();
  CHECK(!radio.busy());
  CHECK(radio.sendJson(command, true));
  radio.poll();
  testTime += 300;
  radio.poll();
  CHECK(!radio.busy());
  testTime += 1500;
  // Never start a new transmission in the middle of an incoming data frame.
  Serial2.rx.push_back('@');
  radio.poll();
  CHECK(!radio.sendJson(command, true));
  testTime += RadioLink::RX_IDLE_TIMEOUT_MS + 100;
  radio.poll();
  CHECK(radio.sendJson(command, true));
  std::cout << "PASS: gateway command frame, AUX completion, ACK listen window and RX protection\n";
}
