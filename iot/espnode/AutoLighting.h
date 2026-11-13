#pragma once

namespace AutoLighting {
constexpr int DayStartMinute = 6 * 60;
constexpr int NightStartMinute = 18 * 60 + 1;
constexpr float ThresholdLux = 50.0f;

constexpr bool desiredState(int minuteOfDay, bool hasLuxSample, float lux, bool currentState) {
  if (minuteOfDay < DayStartMinute || minuteOfDay >= NightStartMinute) return true;
  if (!hasLuxSample || lux == ThresholdLux) return currentState;
  return lux < ThresholdLux;
}
}
