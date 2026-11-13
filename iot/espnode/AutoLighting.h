#pragma once

namespace AutoLighting {
constexpr int DayStartMinute = 6 * 60;
constexpr int NightStartMinute = 18 * 60 + 1;
constexpr float ThresholdLux = 50.0f;

constexpr bool desiredState(int minuteOfDay, bool hasLuxSample, float lux, bool currentState,
    int dayStart = DayStartMinute, int nightStart = NightStartMinute) {
  const bool inDay = dayStart < nightStart
    ? minuteOfDay >= dayStart && minuteOfDay < nightStart
    : minuteOfDay >= dayStart || minuteOfDay < nightStart;
  if (!inDay) return true;
  if (!hasLuxSample || lux == ThresholdLux) return currentState;
  return lux < ThresholdLux;
}
}
