#pragma once
#include <Arduino.h>

namespace NodeLog {
// Keep diagnostics available for troubleshooting, but print only measurements
// on the node's Serial Monitor by default.
constexpr bool DebugEnabled = false;

template <typename... Args>
inline void debugf(const char* format, Args... args) {
  if constexpr (DebugEnabled) Serial.printf(format, args...);
}

template <typename... Args>
inline void debugln(const Args&... args) {
  if constexpr (DebugEnabled) Serial.println(args...);
}
}  // namespace NodeLog
