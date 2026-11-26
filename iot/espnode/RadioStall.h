#pragma once
#include <stdint.h>

// AUX LOW is normal while transmitting/receiving. Report a stall only after
// ten uninterrupted seconds LOW without UART progress; never fake AUX HIGH.
class RadioStall {
 public:
  static constexpr uint32_t TimeoutMs = 10000;
  void observe(uint32_t now, bool auxHigh, uint32_t progress) {
    if (auxHigh || progress != lastProgress) {
      watching = false;
      blocked = false;
    }
    lastProgress = progress;
    if (auxHigh) return;
    if (!watching) { watching = true; since = now; }
    const uint32_t elapsed = now - since;
    if (elapsed > longestWait) longestWait = elapsed;
    if (!blocked && elapsed >= TimeoutMs) { blocked = true; ++events; }
  }
  bool isBlocked() const { return blocked; }
  uint32_t eventCount() const { return events; }
  uint32_t maximumWaitMs() const { return longestWait; }
 private:
  bool watching = false, blocked = false;
  uint32_t since = 0, lastProgress = 0, events = 0, longestWait = 0;
};
