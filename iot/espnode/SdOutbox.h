#pragma once
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include "RadioLink.h"

// Append-only while samples are pending. Reclaim the log after the last durable ACK.
// Keep the same SD card for pending data; reformatting/removing it loses that data.
class SdOutbox {
 public:
  bool ready = false;
  bool begin(const String& bootId) {
    constexpr int sck = 18, miso = 19, mosi = 23, cs = 5;
    constexpr uint32_t frequency = 1000000;
    Serial.printf("MicroSD SPI: SCK=%d MISO=%d MOSI=%d CS=%d, %lu Hz\n",
      sck, miso, mosi, cs, static_cast<unsigned long>(frequency));
    SPI.begin(sck, miso, mosi, cs);
    if (!SD.begin(cs, SPI, frequency)) {
      Serial.println("MicroSD init failed: SD.begin could not initialize the card or mount its filesystem");
      return false;
    }
    const auto cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("MicroSD init failed: no card reported after mount");
      return false;
    }
    Serial.printf("MicroSD mounted: type=%u, size=%llu MiB\n",
      static_cast<unsigned>(cardType), static_cast<unsigned long long>(SD.cardSize() / (1024 * 1024)));
    if (!prefs.begin("luxmap-outbox", false)) {
      Serial.println("MicroSD outbox init failed: cannot open ESP32 NVS cursor");
      return false;
    }
    if (!SD.exists("/queue.id")) {
      File meta = SD.open("/queue.id", FILE_WRITE);
      if (!meta) {
        Serial.println("MicroSD open failed: /queue.id for write");
        return false;
      }
      if (meta.print(bootId) != bootId.length()) {
        Serial.println("MicroSD write failed: /queue.id");
        return false;
      }
      meta.flush(); meta.close();
    }
    File meta = SD.open("/queue.id");
    if (!meta) {
      Serial.println("MicroSD open failed: /queue.id for read");
      return false;
    }
    String queueId = meta.readString(); meta.close();
    queueId.trim();
    if (queueId.isEmpty()) {
      Serial.println("MicroSD read failed: /queue.id is empty");
      return false;
    }
    if (prefs.getString("queueId", "") != queueId) {
      // Reset the cursor BEFORE changing the queue ID; interruption causes replay, not skipping.
      if (prefs.putULong64("cursor", 0) != sizeof(uint64_t) ||
          prefs.putString("queueId", queueId) != queueId.length()) {
        Serial.println("MicroSD outbox init failed: cannot save ESP32 NVS cursor/queue ID");
        return false;
      }
    }
    cursor = prefs.getULong64("cursor", 0);
    File log = SD.open("/telemetry.log", FILE_APPEND);
    if (!log) {
      Serial.println("MicroSD open failed: /telemetry.log for append during init");
      return false;
    }
    if (cursor > log.size()) {
      Serial.printf("MicroSD cursor invalid: cursor=%llu, log=%llu bytes; no automatic reset\n",
        static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(log.size()));
      return false;
    }
    log.close();
    ready = true;
    reclaimAcknowledgedLog();
    return true;
  }
  bool append(const String& payload) {
    if (!ready) return false;
    String line = String("\n") + RadioLink::frame(payload);
    File file = SD.open("/telemetry.log", FILE_APPEND);
    if (!file) {
      Serial.println("MicroSD open failed: /telemetry.log for append");
      return false;
    }
    if (uint64_t(file.size()) + line.length() > 64ULL * 1024 * 1024) {
      Serial.println("MicroSD append stopped: /telemetry.log reached 64 MiB limit");
      return false;
    }
    const size_t written = file.print(line);
    file.flush(); file.close();
    if (written == line.length()) return true;
    Serial.printf("MicroSD write failed: /telemetry.log wrote %u of %u bytes\n",
      static_cast<unsigned>(written), static_cast<unsigned>(line.length()));
    return false;
  }
  bool peek(String& payload) {
    if (!cached.isEmpty()) { payload = cached; return true; }
    if (!ready) return false;
    File file = SD.open("/telemetry.log", FILE_READ);
    if (!file || !file.seek(cursor)) return false;
    while (file.available()) {
      String line;
      bool overflow = false;
      while (file.available()) {
        char c = file.read();
        if (c == '\n') break;
        if (line.length() < RadioLink::MAX_FRAME) line += c;
        else overflow = true;
      }
      endOfCached = file.position();
      String decoded;
      if (!overflow && RadioLink::decode(line, decoded)) {
        cached = decoded;
        payload = cached; file.close(); return true;
      }
      if (line.length()) Serial.println("SD outbox: corrupt/incomplete record skipped; inspect log");
    }
    file.close(); return false;
  }
  bool acknowledge() {
    if (cached.isEmpty() || prefs.putULong64("cursor", endOfCached) != sizeof(uint64_t)) return false;
    cursor = endOfCached;
    cached = "";
    reclaimAcknowledgedLog();
    return true;
  }
 private:
  void reclaimAcknowledgedLog() {
    if (cursor == 0) return;
    File log = SD.open("/telemetry.log", FILE_READ);
    if (!log) return;
    const bool fullyAcknowledged = cursor == log.size();
    log.close();
    if (!fullyAcknowledged) return;

    // Commit zero before truncating. A reboot between them can replay acknowledged
    // samples, but cannot leave a cursor past the end of a newly emptied file.
    if (prefs.putULong64("cursor", 0) != sizeof(uint64_t)) {
      Serial.println("MicroSD cleanup deferred: cannot reset NVS cursor");
      return;
    }
    cursor = 0;
    log = SD.open("/telemetry.log", FILE_WRITE);
    if (!log) {
      Serial.println("MicroSD cleanup failed: cannot truncate /telemetry.log; acknowledged samples may replay");
      return;
    }
    log.flush();
    log.close();
    log = SD.open("/telemetry.log", FILE_READ);
    if (log && log.size() == 0) Serial.println("MicroSD cleared: all queued samples acknowledged");
    else Serial.println("MicroSD cleanup unverified: /telemetry.log is not empty or cannot be read");
  }

  Preferences prefs;
  uint64_t cursor = 0, endOfCached = 0;
  String cached;
};
