#pragma once
#include <SD.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Preferences.h>
#include "NodeLog.h"
#include "RadioLink.h"

// Append-only while samples are pending. Reclaim the log after the last durable ACK.
// Keep the same SD card for pending data; reformatting/removing it loses that data.
class SdOutbox {
 public:
  bool ready = false;
  bool begin(const String& bootId) {
    prefs.end();
    SD.end();
    constexpr int sck = 18, miso = 19, mosi = 23, cs = 5;
    constexpr uint32_t frequency = 1000000;
    NodeLog::debugf("MicroSD SPI: SCK=%d MISO=%d MOSI=%d CS=%d, %lu Hz\n",
      sck, miso, mosi, cs, static_cast<unsigned long>(frequency));
    SPI.begin(sck, miso, mosi, cs);
    if (!SD.begin(cs, SPI, frequency)) {
      NodeLog::debugln("MicroSD init failed: SD.begin could not initialize the card or mount its filesystem");
      return false;
    }
    const auto cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      NodeLog::debugln("MicroSD init failed: no card reported after mount");
      return false;
    }
    NodeLog::debugf("MicroSD mounted: type=%u, size=%llu MiB\n",
      static_cast<unsigned>(cardType), static_cast<unsigned long long>(SD.cardSize() / (1024 * 1024)));
    if (!prefs.begin("luxmap-outbox", false)) {
      NodeLog::debugln("MicroSD outbox init failed: cannot open ESP32 NVS cursor");
      return false;
    }
    if (!SD.exists("/queue.id")) {
      File meta = SD.open("/queue.id", FILE_WRITE);
      if (!meta) {
        NodeLog::debugln("MicroSD open failed: /queue.id for write");
        return false;
      }
      if (meta.print(bootId) != bootId.length()) {
        NodeLog::debugln("MicroSD write failed: /queue.id");
        return false;
      }
      meta.flush(); meta.close();
    }
    File meta = SD.open("/queue.id");
    if (!meta) {
      NodeLog::debugln("MicroSD open failed: /queue.id for read");
      return false;
    }
    String queueId = meta.readString(); meta.close();
    queueId.trim();
    if (queueId.isEmpty()) {
      NodeLog::debugln("MicroSD read failed: /queue.id is empty");
      return false;
    }
    if (prefs.getString("queueId", "") != queueId) {
      // Reset the cursor BEFORE changing the queue ID; interruption causes replay, not skipping.
      if (prefs.putULong64("cursor", 0) != sizeof(uint64_t) ||
          prefs.putString("queueId", queueId) != queueId.length()) {
        NodeLog::debugln("MicroSD outbox init failed: cannot save ESP32 NVS cursor/queue ID");
        return false;
      }
    }
    cursor = prefs.getULong64("cursor", 0);
    File log = SD.open("/telemetry.log", FILE_APPEND);
    if (!log) {
      NodeLog::debugln("MicroSD open failed: /telemetry.log for append during init");
      return false;
    }
    if (cursor > log.size()) {
      NodeLog::debugf("MicroSD cursor invalid: cursor=%llu, log=%llu bytes; no automatic reset\n",
        static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(log.size()));
      return false;
    }
    log.close();
    ready = true;
    readFailed = writeFailed = false;
    reclaimAcknowledgedLog();
    return true;
  }
  bool append(const String& payload) {
    if (!ready) return false;
    writeFailed = true;
    String line = String("\n") + RadioLink::frame(payload);
    File file = SD.open("/telemetry.log", FILE_APPEND);
    if (!file) {
      NodeLog::debugln("MicroSD open failed: /telemetry.log for append");
      return false;
    }
    if (uint64_t(file.size()) + line.length() > 64ULL * 1024 * 1024) {
      NodeLog::debugln("MicroSD append stopped: /telemetry.log reached 64 MiB limit");
      return false;
    }
    const size_t written = file.print(line);
    file.flush(); file.close();
    if (written == line.length()) { writeFailed = false; return true; }
    NodeLog::debugf("MicroSD write failed: /telemetry.log wrote %u of %u bytes\n",
      static_cast<unsigned>(written), static_cast<unsigned>(line.length()));
    return false;
  }
  bool peek(String& payload) {
    if (!cached.isEmpty()) { payload = cached; return true; }
    if (!ready) return false;
    File file = SD.open("/telemetry.log", FILE_READ);
    readFailed = !file || !file.seek(cursor);
    if (readFailed) return false;
    while (file.available()) {
      String line;
      bool overflow = false;
      while (file.available()) {
        const int byteRead = file.read();
        if (byteRead < 0) { readFailed = true; return false; }
        const char c = static_cast<char>(byteRead);
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
      if (line.length()) {
        readFailed = true;
        NodeLog::debugln("SD outbox: corrupt/incomplete record skipped; inspect log");
      }
    }
    file.close(); return false;
  }

  // Read ahead without moving the durable cursor. Individual ACKs reclaim FIFO heads.
  size_t readBatch(String* payloads, size_t capacity) {
    if (!ready || !capacity) return 0;
    File file = SD.open("/telemetry.log", FILE_READ);
    if (!file || !file.seek(cursor)) { readFailed = true; return 0; }
    size_t count = 0, scanned = 0;
    String line, firstSession;
    bool overflow = false;
    while (file.available() && count < capacity && scanned++ < 4096) {
      int value = file.read();
      if (value < 0) { readFailed = true; break; }
      if (value != '\n') {
        if (line.length() < RadioLink::MAX_FRAME) line += char(value);
        else overflow = true;
        continue;
      }
      String decoded;
      JsonDocument data;
      if (!overflow && RadioLink::decode(line, decoded) && !deserializeJson(data, decoded)) {
        const String session = data["sessionId"] | "";
        if (count && session != firstSession) break;
        firstSession = session;
        payloads[count++] = decoded;
      }
      line = ""; overflow = false;
    }
    file.close();
    return count;
  }

  // Independent, bounded read-only scan. Never calls peek(), acknowledge(), or writes NVS.
  struct QueueInspection {
    uint32_t samples = 0, invalid = 0;
    uint64_t cursor = 0, bytes = 0;
    bool active = false, complete = false, readError = false;
    String firstTimestamp, lastTimestamp;
  } inspection;
  bool startInspection() {
    if (!ready || inspection.active) return false;
    inspectionFile = SD.open("/telemetry.log", FILE_READ);
    if (!inspectionFile) return false;
    inspection = QueueInspection{};
    inspection.cursor = cursor;
    inspection.bytes = inspectionFile.size();
    if (cursor > inspection.bytes || !inspectionFile.seek(cursor)) {
      inspectionFile.close(); return false;
    }
    inspectionLine = ""; inspectionOverflow = false;
    inspection.active = true;
    return true;
  }
  void inspectStep() {
    if (!inspection.active) return;
    // Cooperatively scan at most 512 bytes / 5 ms, allowing LoRa ACK handling between steps.
    const unsigned long started = millis();
    for (unsigned count = 0; count < 512 && millis() - started < 5; ++count) {
      if (inspectionFile.position() >= inspection.bytes) {
        if (inspectionLine.length() || inspectionOverflow) ++inspection.invalid;
        inspectionFile.close(); inspection.active = false; inspection.complete = true;
        return;
      }
      const int value = inspectionFile.read();
      if (value < 0) {
        inspection.readError = true; inspectionFile.close();
        inspection.active = false; inspection.complete = true; return;
      }
      if (value != '\n') {
        if (inspectionLine.length() < RadioLink::MAX_FRAME) inspectionLine += char(value);
        else inspectionOverflow = true;
        continue;
      }
      if (inspectionLine.length() || inspectionOverflow) {
        String decoded;
        JsonDocument data;
        if (!inspectionOverflow && RadioLink::decode(inspectionLine, decoded) &&
            !deserializeJson(data, decoded) && String(data["kind"] | "") == "telemetry") {
          const String stamp = data["timestamp"] | "unavailable";
          if (!inspection.samples) inspection.firstTimestamp = stamp;
          inspection.lastTimestamp = stamp;
          ++inspection.samples;
        } else ++inspection.invalid;
      }
      inspectionLine = ""; inspectionOverflow = false;
    }
  }

  bool hasReadError() const { return readFailed; }
  bool hasWriteError() const { return writeFailed; }
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
      NodeLog::debugln("MicroSD cleanup deferred: cannot reset NVS cursor");
      return;
    }
    cursor = 0;
    log = SD.open("/telemetry.log", FILE_WRITE);
    if (!log) {
      NodeLog::debugln("MicroSD cleanup failed: cannot truncate /telemetry.log; acknowledged samples may replay");
      return;
    }
    log.flush();
    log.close();
    log = SD.open("/telemetry.log", FILE_READ);
    if (log && log.size() == 0) NodeLog::debugln("MicroSD cleared: all queued samples acknowledged");
    else NodeLog::debugln("MicroSD cleanup unverified: /telemetry.log is not empty or cannot be read");
  }

  File inspectionFile;
  String inspectionLine;
  bool inspectionOverflow = false;
  Preferences prefs;
  bool readFailed = false, writeFailed = false;
  uint64_t cursor = 0, endOfCached = 0;
  String cached;
};
