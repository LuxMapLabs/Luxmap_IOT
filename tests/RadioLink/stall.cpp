#include <cassert>
#include <iostream>
#include "../../iot/espnode/RadioLink.h"

int main() {
  RadioStall monitor;
  monitor.observe(0, false, 0);
  monitor.observe(3000, false, 0);
  assert(!monitor.isBlocked()); // A legitimate batch can exceed the old 3-second timer.
  monitor.observe(9999, false, 0);
  assert(!monitor.isBlocked());
  monitor.observe(10000, false, 0);
  assert(monitor.isBlocked() && monitor.eventCount() == 1);
  monitor.observe(12000, false, 0);
  assert(monitor.eventCount() == 1);
  monitor.observe(12001, true, 0);
  assert(!monitor.isBlocked()); // Recovery is immediate, no stale 30-second latch.
  monitor.observe(13000, false, 0);
  monitor.observe(21000, false, 1); // Actual UART progress restarts the wait.
  monitor.observe(30999, false, 1);
  assert(!monitor.isBlocked());
  monitor.observe(31000, false, 1);
  assert(monitor.isBlocked() && monitor.eventCount() == 2);
  monitor.observe(31001, false, 2);
  assert(!monitor.isBlocked());
  RadioStall rollover;
  rollover.observe(UINT32_MAX - 4999, false, 0);
  rollover.observe(4999, false, 0);
  assert(!rollover.isBlocked());
  rollover.observe(5000, false, 0);
  assert(rollover.isBlocked());
  // Observe AUX even when no software frame is queued: the idle gate otherwise
  // prevents a stuck module from ever entering radio.busy().
  RadioLink radio;
  radio.begin(nullptr);
  auxState = LOW; testTime = 0; radio.poll();
  testTime = 9999; radio.poll();
  assert(!radio.transmitBlocked());
  testTime = 10000; radio.poll();
  assert(radio.transmitBlocked() && !radio.busy());
  auxState = HIGH; radio.poll();
  assert(!radio.transmitBlocked());
  std::cout << "PASS: 10s no-progress AUX stall, normal batch, immediate recovery, progress and rollover\n";
}
