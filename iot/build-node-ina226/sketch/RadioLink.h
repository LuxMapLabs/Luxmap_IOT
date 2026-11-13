#line 1 "E:\\Ki9\\DoAn\\iot\\code iot\\iot\\espnode\\RadioLink.h"
#pragma once
#include <Arduino.h>

// E32 transparent UART, 9600 8N1; M0/M1 must be tied LOW on both modules.
// Application framing: @JSON*CRC16\n. Short control frames fit in one E32 UART burst.
class RadioLink {
 public:
  static const size_t MAX_FRAME = 768;
  static constexpr size_t CONTROL_BURST_LIMIT = 256;
  static constexpr unsigned long CHUNK_INTERVAL_MS = 300;
  static constexpr unsigned long RX_IDLE_TIMEOUT_MS = 2500;
  static uint16_t crc(const String& data) {
    uint16_t value = 0xFFFF;
    for (unsigned int i = 0; i < data.length(); ++i) {
      value ^= uint16_t(uint8_t(data[i])) << 8;
      for (int bit = 0; bit < 8; ++bit)
        value = value & 0x8000 ? (value << 1) ^ 0x1021 : value << 1;
    }
    return value;
  }
  static String frame(const String& json) {
    char suffix[8]; snprintf(suffix, sizeof(suffix), "*%04X\n", crc(json));
    return String("@") + json + suffix;
  }
  static bool decode(String line, String& json) {
    line.trim();
    int star = line.lastIndexOf('*');
    if (!line.startsWith("@") || star < 2 || line.length() != size_t(star + 5)) return false;
    String checksum = line.substring(star + 1);
    for (int i = 0; i < 4; ++i) if (!isxdigit(checksum[i])) return false;
    json = line.substring(1, star);
    return crc(json) == strtoul(checksum.c_str(), nullptr, 16);
  }
  void begin(void (*handler)(const String&)) {
    callback = handler;
    pinMode(4, INPUT_PULLUP);  // E32 AUX -> GPIO4
    Serial2.setRxBufferSize(2048);
    Serial2.begin(9600, SERIAL_8N1, 16, 17);
  }
  bool busy() const { return outgoing.length() != 0; }
  bool sendJson(const String& json, bool control = false) {
    if (busy() || json.length() + 7 > MAX_FRAME) return false;
    outgoing = frame(json);
    burstSize = control && outgoing.length() <= CONTROL_BURST_LIMIT ? CONTROL_BURST_LIMIT : 48;
    offset = 0;
    lastChunk = millis() - CHUNK_INTERVAL_MS;
    return true;
  }
  void poll() {
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c == '@') { incoming = "@"; receiving = true; }
      else if (receiving && c == '\n') {
        String payload;
        if (decode(incoming, payload) && callback) callback(payload);
        else Serial.println("LoRa: rejected bad CRC/frame");
        incoming = ""; receiving = false;
      } else if (receiving && c != '\r') {
        if (incoming.length() < MAX_FRAME) incoming += c;
        else { incoming = ""; receiving = false; }
      }
      lastReceive = millis();
    }
    if (receiving && millis() - lastReceive > RX_IDLE_TIMEOUT_MS) {
      incoming = ""; receiving = false;
      Serial.println("LoRa: incomplete frame timed out");
    }
    if (!busy() || digitalRead(4) == LOW || millis() - lastChunk < CHUNK_INTERVAL_MS) return;
    if (offset >= outgoing.length()) { outgoing = ""; return; }
    if (offset == 0 && receiving) return;
    size_t count = outgoing.length() - offset;
    if (count > burstSize) count = burstSize;
    Serial2.write(reinterpret_cast<const uint8_t*>(outgoing.c_str() + offset), count);
    offset += count; lastChunk = millis();
  }
 private:
  void (*callback)(const String&) = nullptr;
  String incoming, outgoing;
  bool receiving = false;
  size_t offset = 0, burstSize = 48;
  unsigned long lastChunk = 0, lastReceive = 0;
};
