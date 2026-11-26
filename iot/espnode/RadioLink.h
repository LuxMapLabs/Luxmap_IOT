#pragma once
#include <Arduino.h>
#include "NodeLog.h"
#include "RadioStall.h"

// E32 transparent UART, 9600 8N1; M0/M1 must be tied LOW on both modules.
// Application framing: @JSON*CRC16\n. Short control frames fit in one E32 UART burst.
class RadioLink {
 public:
  static const size_t MAX_FRAME = 768;
  static constexpr size_t CONTROL_BURST_LIMIT = 256;
  static constexpr size_t DATA_BURST_LIMIT = 128;
  // E32-433T20D has a 512-byte UART buffer. A complete frame below this
  // conservative bound avoids artificial gaps between its JSON fragments.
  static constexpr size_t WHOLE_FRAME_LIMIT = 480;
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
    pinMode(AUX_PIN, INPUT_PULLUP);
    Serial2.setRxBufferSize(2048);
    Serial2.begin(9600, SERIAL_8N1, 16, 17);
  }
  bool busy() const { return outgoing.length() != 0; }
  bool auxHigh() const { return digitalRead(AUX_PIN) == HIGH; }
  bool quietFor(unsigned long duration) const {
    return quiet && idle() && millis() - quietSince >= duration;
  }
  uint32_t receiveErrorCount() const { return receiveErrors; }
  bool transmitBlocked() const { return stall.isBlocked(); }
  uint32_t stallCount() const { return stall.eventCount(); }
  uint32_t maximumAuxWaitMs() const { return stall.maximumWaitMs(); }
  bool sendJson(const String& json, bool control = false) {
    if (busy() || json.length() + 7 > MAX_FRAME) return false;
    quiet = false;
    outgoing = frame(json);
    burstSize = control && outgoing.length() <= CONTROL_BURST_LIMIT ? CONTROL_BURST_LIMIT :
      outgoing.length() <= WHOLE_FRAME_LIMIT ? WHOLE_FRAME_LIMIT : DATA_BURST_LIMIT;
    offset = 0;
    frameBytesWritten = 0;
    controlFrame = control;
    lastChunk = millis() - CHUNK_INTERVAL_MS;
    return true;
  }
  void poll() {
    if (!idle()) quiet = false;
    while (Serial2.available()) {
      quiet = false;
      char c = Serial2.read();
      ++uartProgress;
      if (c == '@') { incoming = "@"; receiving = true; }
      else if (receiving && c == '\n') {
        String payload;
        if (decode(incoming, payload) && callback) callback(payload);
        else { ++receiveErrors; NodeLog::debugln("LoRa: rejected bad CRC/frame"); }
        incoming = ""; receiving = false;
      } else if (receiving && c != '\r') {
        if (incoming.length() < MAX_FRAME) incoming += c;
        else { ++receiveErrors; incoming = ""; receiving = false; }
      }
      lastReceive = millis();
    }
    stall.observe(millis(), auxHigh(), uartProgress);
    if (receiving && millis() - lastReceive > RX_IDLE_TIMEOUT_MS) {
      incoming = ""; receiving = false;
      ++receiveErrors;
      NodeLog::debugln("LoRa: incomplete frame timed out");
    }
    if (idle() && !quiet) {
      quietSince = millis();
      quiet = true;
    }
    if (!busy() || !auxHigh()) return;
    if (offset >= outgoing.length()) { outgoing = ""; return; }
    if (millis() - lastChunk < CHUNK_INTERVAL_MS) return;
    if (offset == 0 && receiving) return;
    size_t count = outgoing.length() - offset;
    if (count > burstSize) count = burstSize;
    const size_t written = Serial2.write(reinterpret_cast<const uint8_t*>(outgoing.c_str() + offset), count);
    frameBytesWritten += written;
    uartProgress += written;
    offset += count; lastChunk = millis();
    if (offset == outgoing.length()) {
      NodeLog::debugf("LoRa UART %s frame: wrote %u/%u bytes, AUX=%s\n",
        controlFrame ? "control" : "telemetry", static_cast<unsigned>(frameBytesWritten),
        static_cast<unsigned>(outgoing.length()), auxHigh() ? "HIGH" : "LOW");
    }
  }
 private:
  static constexpr int AUX_PIN = 4;  // E32 AUX -> GPIO4
  bool idle() const { return auxHigh() && !busy() && !receiving && !Serial2.available(); }
  void (*callback)(const String&) = nullptr;
  String incoming, outgoing;
  bool receiving = false, controlFrame = false, quiet = false;
  uint32_t receiveErrors = 0;
  uint32_t uartProgress = 0;
  RadioStall stall;
  size_t offset = 0, burstSize = 48, frameBytesWritten = 0;
  unsigned long lastChunk = 0, lastReceive = 0, quietSince = 0;
};
