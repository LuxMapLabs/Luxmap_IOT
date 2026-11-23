#pragma once

#include <Arduino.h>

enum class SensorIssue : uint8_t {
  None, RtcUnavailable, RtcReadFailed, RtcTimeInvalid, InaUnavailable, InaReadFailed,
  InaShuntOutOfRange, LuxUnavailable, LuxReadFailed, SdUnavailable, SdReadFailed, SdWriteFailed,
  LoraTxBlocked, LoraRxFrameError
};

inline const char* sensorIssueCode(SensorIssue issue) {
  static const char* const codes[] = {
    "", "ds3231_unavailable", "ds3231_read_failed", "ds3231_time_invalid",
    "ina226_unavailable", "ina226_read_failed", "ina226_shunt_out_of_range",
    "bh1750_unavailable", "bh1750_read_failed", "microsd_unavailable",
    "microsd_read_failed", "microsd_write_failed", "lora_tx_blocked", "lora_rx_frame_error"
  };
  return codes[static_cast<uint8_t>(issue)];
}

struct SensorFault {
  SensorIssue active = SensorIssue::None;
  bool ready = false;

  bool observe(SensorIssue issue) {
    if (issue != candidate) {
      candidate = issue;
      consecutive = 0;
    }
    if (consecutive < 3) ++consecutive;
    if (consecutive != 3 || (ready && active == candidate)) return false;
    active = candidate;
    ready = true;
    return true;
  }

private:
  SensorIssue candidate = SensorIssue::None;
  uint8_t consecutive = 0;
};
