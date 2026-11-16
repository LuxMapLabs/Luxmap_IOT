#pragma once

namespace AutoLighting {
constexpr int DayStartMinute = 6 * 60;
constexpr int NightStartMinute = 18 * 60 + 1;
constexpr float ThresholdLux = 50.0f;

constexpr int RequiredSamples = 5;

constexpr bool isDay(int minuteOfDay, int dayStart, int nightStart) {
  return dayStart < nightStart
    ? minuteOfDay >= dayStart && minuteOfDay < nightStart
    : minuteOfDay >= dayStart || minuteOfDay < nightStart;
}

class ConsecutiveSamples {
 public:
  void reset() { count = 0; }
  int pendingSamples() const { return count; }

  bool update(bool valid, float lux, bool lampOn) {
    if (!valid || lux == ThresholdLux || (lux < ThresholdLux) == lampOn) {
      reset();
      return lampOn;
    }
    if (++count < RequiredSamples) return lampOn;
    reset();
    return !lampOn;
  }

 private:
  int count = 0;
};
}
