#pragma once

#include <Wire.h>

class Ina226Meter {
public:
  Ina226Meter(uint8_t address, float shuntOhms) : address_(address), shuntOhms_(shuntOhms) {}

  bool begin() {
    uint16_t manufacturer, die;
    if (!readRegister(0xFE, manufacturer) || manufacturer != 0x5449 ||
        !readRegister(0xFF, die) || (die & 0xFFF0) != 0x2260) return false;

    // 16 averages, 1.1 ms per conversion, shunt and bus continuously.
    Wire.beginTransmission(address_);
    Wire.write(uint8_t(0x00));
    Wire.write(uint8_t(0x45));
    Wire.write(uint8_t(0x27));
    return Wire.endTransmission() == 0;
  }

  bool read(float& voltageV, float& currentMa, float& powerMw) {
    uint16_t shuntRaw, busRaw;
    if (!readRegister(0x01, shuntRaw) || !readRegister(0x02, busRaw) ||
        shuntRaw == 0x7FFF || shuntRaw == 0x8000) return false;

    const float shuntV = static_cast<int16_t>(shuntRaw) * 0.0000025f;
    const float busV = busRaw * 0.00125f;
    voltageV = busV + shuntV; // Keep the old source-side reading; VBUS is wired to IN-.
    currentMa = shuntV / shuntOhms_ * 1000.0f;
    powerMw = busV * currentMa; // Power delivered to the load.
    return true;
  }

private:
  const uint8_t address_;
  const float shuntOhms_;

  bool readRegister(uint8_t reg, uint16_t& value) {
    Wire.beginTransmission(address_);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address_, uint8_t(2)) != 2) return false;
    const uint16_t high = Wire.read();
    value = (high << 8) | uint16_t(Wire.read());
    return true;
  }
};
