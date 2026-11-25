#include <ArduinoJson.h>
#include <Wire.h>
#include <RTClib.h>
#include <BH1750.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <math.h>
#include "NodeLog.h"
#include "AutoLighting.h"
#include "Ina226Meter.h"
#include "RadioLink.h"
#include "SdOutbox.h"
#include "SensorHealth.h"

const char* DEVICE_ID = "LUXMAP-P001";
const int RELAY_PIN = 27;
const int RELAY_ON_LEVEL = HIGH, RELAY_OFF_LEVEL = LOW;
const unsigned long SENSOR_INTERVAL = 1000, SAMPLE_INTERVAL = 1000, RETRY_INTERVAL = 3000;
constexpr uint8_t INA226_ADDRESS = 0x40;
constexpr float INA226_SHUNT_OHMS = 0.002f; // R002 on this module.
// Continuous 1 Hz reads are selected for the user's capacitor observation.
// MEASURE QUIET can temporarily restore the RF quiet-window gate.
constexpr unsigned long INA_RADIO_QUIET_MS = 1500;
constexpr unsigned long INA_SETTLE_MS = 300;
constexpr unsigned long TELEMETRY_ACK_LISTEN_MS = 2500;
constexpr uint8_t RTC_ADDRESS = 0x68;
constexpr unsigned long SENSOR_REINIT_INTERVAL = 10000, SENSOR_HEALTH_INTERVAL = 30000;
Ina226Meter ina226(INA226_ADDRESS, INA226_SHUNT_OHMS);
RTC_DS3231 rtc;
BH1750 lightSensor;
RadioLink radio;
SdOutbox outbox;
Preferences controls;
bool inaReady = false, rtcReady = false, rtcTimeValid = false, luxReady = false, controlsReady = false;
bool luxSampleValid = false;
DateTime rtcSample(2000, 1, 1);
SensorFault rtcFault, inaFault, luxFault;
SensorIssue sdIssue = SensorIssue::None, loraIssue = SensorIssue::None;
bool sensorHealthChanged = true;
String pendingSensorHealth;
uint32_t sensorHealthSequence = 0;
unsigned long lastSensorHealthTransmit = 0, lastSensorReinit = 0;
bool lampIsOn = false, manualOn = false;
int dayStartMinute = AutoLighting::DayStartMinute;
int nightStartMinute = AutoLighting::NightStartMinute;
String autoType = "scheduled";
String mode = "AUTO", appliedCommandId = "", sessionId;
uint64_t appliedVersion = 0;
int64_t appliedExecutedAt = 0;
uint32_t sequenceNumber = 0;
float luxValue = 0, voltageValue = 0, currentValue = 0, powerValue = 0;
unsigned long lastSensor = 0, lastElectricalSample = 0, lastSample = 0, lastTransmit = 0;
unsigned long inaConfiguredAt = 0, relayChangedAt = 0, lastRamSaveAttempt = 0;
unsigned long lastTelemetryAck = 0, retryDelay = RETRY_INTERVAL;
unsigned long lastQueueCheck = 0;
String pendingPayload, volatilePayload, commandAck;
// Live is a fast copy of an already queued sample. Its ACK NEVER moves the FIFO cursor.
String latestLivePayload, livePendingPayload, lastLiveAckTimestamp;
bool preferLive = true;
uint32_t liveFramesQueued = 0, liveAcksReceived = 0;
// Reserve one real electrical measurement between data frames. Command ACKs
// bypass this gate; inbound commands are always polled.
constexpr uint8_t MEASUREMENTS_PER_DATA_FRAME = 1;
uint8_t measurementsSinceDataFrame = 0;
uint32_t telemetryFramesQueued = 0, telemetryAcksReceived = 0;
uint32_t commandPacketsReceived = 0, commandAcksQueued = 0, healthAcksReceived = 0;
bool radioTransmitPaused = false;
bool fastMeasurementBenchMode = true;

void formatRtcTime(const DateTime& time, char (&output)[20]) {
  snprintf(output, sizeof(output), "%04u-%02u-%02uT%02u:%02u:%02u",
    unsigned(time.year()), unsigned(time.month()), unsigned(time.day()),
    unsigned(time.hour()), unsigned(time.minute()), unsigned(time.second()));
}

uint8_t fromBcd(uint8_t value) {
  return 10 * (value >> 4) + (value & 0x0f);
}

SensorIssue readRtcClock(DateTime& time) {
  if (!rtcReady) return SensorIssue::RtcUnavailable;
  // Read the clock and oscillator status in one checked I2C transaction.
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(uint8_t(0));
  uint8_t registers[16];
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(RTC_ADDRESS, sizeof(registers)) != sizeof(registers))
    return SensorIssue::RtcReadFailed;
  if (Wire.readBytes(registers, sizeof(registers)) != sizeof(registers)) return SensorIssue::RtcReadFailed;
  if ((registers[15] & 0x80) || (registers[5] & 0x80)) return SensorIssue::RtcTimeInvalid;
  const bool twelveHour = registers[2] & 0x40;
  const uint8_t hour = registers[2] & (twelveHour ? 0x1f : 0x3f);
  const uint8_t fields[] = { uint8_t(registers[0] & 0x7f), uint8_t(registers[1] & 0x7f),
    hour, uint8_t(registers[4] & 0x3f), uint8_t(registers[5] & 0x1f), registers[6] };
  for (uint8_t field : fields)
    if ((field & 0x0f) > 9 || (field >> 4) > 9) return SensorIssue::RtcTimeInvalid;
  const uint8_t decodedHour = fromBcd(hour);
  if (twelveHour && (decodedHour < 1 || decodedHour > 12)) return SensorIssue::RtcTimeInvalid;
  time = DateTime(2000 + fromBcd(fields[5]), fromBcd(fields[4]), fromBcd(fields[3]),
    twelveHour ? decodedHour % 12 + ((registers[2] & 0x20) ? 12 : 0) : decodedHour,
    fromBcd(fields[1]), fromBcd(fields[0]));
  return time.isValid() ? SensorIssue::None : SensorIssue::RtcTimeInvalid;
}

void handleRtcSerial() {
  static char line[32];
  static size_t length = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (length < sizeof(line) - 1) line[length++] = c;
      else overflow = true;
      continue;
    }
    line[length] = '\0';
    // Volatile bench control: reboot always restores normal LoRa transmission.
    if (!overflow && strcmp(line, "RADIO PAUSE") == 0) radioTransmitPaused = true;
    if (!overflow && strcmp(line, "RADIO RESUME") == 0) radioTransmitPaused = false;
    if (!overflow && strcmp(line, "MEASURE FAST") == 0) fastMeasurementBenchMode = true;
    if (!overflow && strcmp(line, "MEASURE QUIET") == 0) fastMeasurementBenchMode = false;
    if (!overflow && strcmp(line, "MEASURE GET") == 0) {
      Serial.printf("MEASURE mode=%s wait_ms=%lu\n", fastMeasurementBenchMode ? "FAST" : "QUIET",
        fastMeasurementBenchMode ? 0UL : INA_RADIO_QUIET_MS);
    }
    // Explicit maintenance query only; normal Serial output stays electrical.
    if (!overflow && strcmp(line, "LINK STATS") == 0) {
      Serial.printf("LINK telemetry_tx=%lu telemetry_ack=%lu command_rx=%lu command_ack_tx=%lu health_ack=%lu rx_errors=%lu pending=%s AUX=%s\n",
        (unsigned long)telemetryFramesQueued, (unsigned long)telemetryAcksReceived,
        (unsigned long)commandPacketsReceived, (unsigned long)commandAcksQueued,
        (unsigned long)healthAcksReceived, (unsigned long)radio.receiveErrorCount(),
        pendingPayload.isEmpty() ? "no" : "yes", radio.auxHigh() ? "HIGH" : "LOW");
    }

    if (!overflow && strcmp(line, "LIVE STATS") == 0) {
      Serial.printf("LIVE tx=%lu ack=%lu last_ack_measured_local=%s pending=%s next=%s\n",
        (unsigned long)liveFramesQueued, (unsigned long)liveAcksReceived,
        lastLiveAckTimestamp.isEmpty() ? "none" : lastLiveAckTimestamp.c_str(),
        livePendingPayload.isEmpty() ? "no" : "yes", preferLive ? "live" : "backlog");
    }
    if (!overflow && strcmp(line, "QUEUE GET") == 0) {
      if (outbox.startInspection()) Serial.println("QUEUE GET scanning=read_only snapshot=at_request");
      else Serial.println("QUEUE GET status=unavailable_or_busy");
    }

    // Read-only maintenance query: never adjusts the clock or touches the SD queue.
    if (!overflow && strcmp(line, "RTC GET") == 0) {
      DateTime observed(2000, 1, 1);
      const auto issue = readRtcClock(observed);
      if (issue == SensorIssue::None) {
        char value[20];
        formatRtcTime(observed, value);
        Serial.printf("RTC GET time=%s+07:00 status=valid\n", value);
      } else {
        Serial.printf("RTC GET time=unavailable status=%s\n", sensorIssueCode(issue));
      }
    }
    if (!overflow && length == 27 && strncmp(line, "RTC SET ", 8) == 0 && rtcReady) {
      unsigned year, month, day, hour, minute, second;
      if (sscanf(line + 8, "%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour, &minute, &second) == 6 &&
          year >= 2000 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
          hour <= 23 && minute <= 59 && second <= 59) {
        const DateTime requested(year, month, day, hour, minute, second);
        char canonical[20];
        formatRtcTime(requested, canonical);
        if (requested.isValid() && strcmp(canonical, line + 8) == 0) {
          rtc.adjust(requested);
          rtcTimeValid = readRtcClock(rtcSample) == SensorIssue::None &&
            rtcSample.unixtime() >= requested.unixtime() && rtcSample.unixtime() - requested.unixtime() <= 2;
          NodeLog::debugf("DS3231 %s: %s +07:00\n", rtcTimeValid ? "set" : "set failed", canonical);
        } else NodeLog::debugln("DS3231: invalid date");
      } else NodeLog::debugln("DS3231: invalid date");
    }
    length = 0;
    overflow = false;
  }
}

void setLamp(bool on) {
  const int level = on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL;
  digitalWrite(RELAY_PIN, level);
  if (on != lampIsOn) {
    relayChangedAt = millis();
    NodeLog::debugf("Lamp %s, GPIO%d=%s\n", on ? "ON" : "OFF", RELAY_PIN, level == HIGH ? "HIGH" : "LOW");
  }
  lampIsOn = on;
}
AutoLighting::ConsecutiveSamples autoSamples;

void controlLamp(bool freshSample) {
  if (mode == "MANUAL") { autoSamples.reset(); setLamp(manualOn); return; }
  const bool fullTime = autoType == "full_time";
  if (!fullTime && !rtcTimeValid) { autoSamples.reset(); return; }
  const int minutes = rtcSample.hour() * 60 + rtcSample.minute();
  if (!AutoLighting::useSensor(fullTime, rtcTimeValid, minutes, dayStartMinute, nightStartMinute)) {
    autoSamples.reset();
    setLamp(true);
  } else if (freshSample) {
    const bool nextState = autoSamples.update(luxSampleValid, luxValue, lampIsOn);
    const int streak = nextState != lampIsOn ? AutoLighting::RequiredSamples : autoSamples.pendingSamples();
    if (streak > 0)
      NodeLog::debugf("AUTO lux=%.2f target=%s samples=%d/%d\n",
        luxValue, lampIsOn ? "OFF" : "ON", streak, AutoLighting::RequiredSamples);
    setLamp(nextState);
  }
}
void readEnvironment() {
  const unsigned long now = millis();
  if (now - lastSensorReinit >= SENSOR_REINIT_INTERVAL) {
    lastSensorReinit = now;
    if (!rtcReady) rtcReady = rtc.begin();
    if (!inaReady || inaFault.active == SensorIssue::InaReadFailed) {
      inaReady = ina226.begin() || inaReady;
      inaConfiguredAt = millis();
    }
    if (!luxReady || luxFault.active == SensorIssue::LuxReadFailed)
      luxReady = lightSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE) || luxReady;
    if (!outbox.ready) outbox.begin(sessionId);
  }
  const auto rtcIssue = readRtcClock(rtcSample);
  rtcTimeValid = rtcIssue == SensorIssue::None;
  sensorHealthChanged |= rtcFault.observe(rtcIssue);
  luxSampleValid = false;
  if (!luxReady) sensorHealthChanged |= luxFault.observe(SensorIssue::LuxUnavailable);
  else if (lightSensor.measurementReady(true)) {
    const float value = lightSensor.readLightLevel();
    luxSampleValid = isfinite(value) && value >= 0;
    sensorHealthChanged |= luxFault.observe(luxSampleValid ? SensorIssue::None : SensorIssue::LuxReadFailed);
    if (luxSampleValid) luxValue = value;
  }
}
void captureTelemetry() {
  if (!luxSampleValid || !isfinite(voltageValue) ||
      !isfinite(currentValue) || !isfinite(powerValue) || !volatilePayload.isEmpty() || sequenceNumber == UINT32_MAX) {
    NodeLog::debugln("Sample skipped: sensor unavailable or unsaved RAM record waiting"); return;
  }
  JsonDocument data;
  data["kind"] = "telemetry"; data["deviceId"] = DEVICE_ID;
  data["sessionId"] = sessionId; data["sequenceNumber"] = ++sequenceNumber;
  data["mode"] = mode; data["lampCommand"] = lampIsOn ? "ON" : "OFF";
  if (mode == "AUTO") data["autoType"] = autoType;
  data["lux"] = luxValue; data["voltage"] = voltageValue;
  data["current"] = currentValue; data["power"] = powerValue;
  data["appliedCommandId"] = appliedCommandId;
  data["captureUptime"] = millis();
  char sampledAt[20] = "";
  DateTime measuredTime(2000, 1, 1);
  if (readRtcClock(measuredTime) == SensorIssue::None) {
    formatRtcTime(measuredTime, sampledAt);
    data["timestamp"] = sampledAt;
  }
  String payload; serializeJson(data, payload);
  if (!outbox.append(payload)) {
    volatilePayload = payload;
    NodeLog::debugln("ERROR: SD append failed/unavailable; one RAM record retained (lost on power off)");
  } else NodeLog::debugf("MicroSD saved seq=%lu timestamp=%s\n", static_cast<unsigned long>(sequenceNumber),
    sampledAt[0] ? sampledAt : "unavailable");
  latestLivePayload = payload;
}

void sampleElectrical(unsigned long now) {
  const bool forceMeasure = (now - lastElectricalSample >= 3000);

  if (now - lastElectricalSample < SENSOR_INTERVAL || now - inaConfiguredAt < INA_SETTLE_MS ||
      now - relayChangedAt < INA_SETTLE_MS ||
      (!fastMeasurementBenchMode && !forceMeasure && !radio.quietFor(INA_RADIO_QUIET_MS))) return;

  const auto result = inaReady ? ina226.read(voltageValue, currentValue, powerValue) : Ina226Meter::ReadResult::I2cFailure;
  if (!fastMeasurementBenchMode && !forceMeasure && !radio.quietFor(INA_RADIO_QUIET_MS)) return;
  lastElectricalSample = now;
  if (measurementsSinceDataFrame < MEASUREMENTS_PER_DATA_FRAME) ++measurementsSinceDataFrame;
  const bool valid = result == Ina226Meter::ReadResult::Ok;
  sensorHealthChanged |= inaFault.observe(!inaReady ? SensorIssue::InaUnavailable :
    valid ? SensorIssue::None : result == Ina226Meter::ReadResult::ShuntOutOfRange
      ? SensorIssue::InaShuntOutOfRange : SensorIssue::InaReadFailed);
  if (valid) {
    Serial.printf("lux=%.2f V=%.3f mA=%.3f mW=%.3f mode=%s lamp=%s\n",
      luxValue, voltageValue, currentValue, powerValue, mode.c_str(), lampIsOn ? "ON" : "OFF");
  } else NodeLog::debugln("INA226: no valid electrical sample (check I2C and shunt range)");

  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    if (valid) captureTelemetry();
  }
}
bool sendSensorHealth(unsigned long now) {
  if (!rtcFault.ready || !inaFault.ready || !luxFault.ready ||
      (sensorHealthSequence != 0 && now - lastSensorHealthTransmit < 1000)) return false;
  const auto elapsed = now - lastSensorHealthTransmit;
  const bool newReport = sensorHealthChanged || (pendingSensorHealth.isEmpty() && elapsed >= SENSOR_HEALTH_INTERVAL);
  if (newReport) {
    JsonDocument report;
    report["kind"] = "sensorHealth"; report["deviceId"] = DEVICE_ID;
    report["sessionId"] = sessionId; report["sequenceNumber"] = ++sensorHealthSequence;
    report["firmwareVersion"] = "fast-v12-1hz-replay";
    JsonArray codes = report["codes"].to<JsonArray>();
    for (const auto* fault : { &rtcFault, &inaFault, &luxFault })
      if (fault->active != SensorIssue::None) codes.add(sensorIssueCode(fault->active));
    for (const auto issue : { sdIssue, loraIssue })
      if (issue != SensorIssue::None) codes.add(sensorIssueCode(issue));
    pendingSensorHealth = "";
    serializeJson(report, pendingSensorHealth);
    sensorHealthChanged = false;
  }
  if (!pendingSensorHealth.isEmpty() && (newReport || elapsed >= RETRY_INTERVAL) && radio.sendJson(pendingSensorHealth)) {
    lastSensorHealthTransmit = now;
    NodeLog::debugf("Sensor health queued seq=%lu\n", static_cast<unsigned long>(sensorHealthSequence));
    return true;
  }
  return false;
}

void updateHardwareHealth(unsigned long now) {
  const auto storage = !outbox.ready ? SensorIssue::SdUnavailable :
    outbox.hasWriteError() ? SensorIssue::SdWriteFailed :
    outbox.hasReadError() ? SensorIssue::SdReadFailed : SensorIssue::None;

  static bool waitingForAux = false, txBlocked = false, rxFault = false;
  static unsigned long auxWaitStarted = 0, lastTxBlocked = 0, lastRxError = 0;
  static uint32_t previousRxErrors = 0, recentRxErrors = 0;
  if (radio.busy() && !radio.auxHigh()) {
    if (!waitingForAux) auxWaitStarted = now;
    waitingForAux = true;
    if (now - auxWaitStarted >= RETRY_INTERVAL) {
      txBlocked = true;
      lastTxBlocked = now;
    }
  } else waitingForAux = false;
  if (txBlocked && now - lastTxBlocked >= SENSOR_HEALTH_INTERVAL) txBlocked = false;

  if (now - lastRxError >= SENSOR_HEALTH_INTERVAL) {
    recentRxErrors = 0;
    rxFault = false;
  }
  const uint32_t rxErrors = radio.receiveErrorCount();
  if (rxErrors != previousRxErrors) {
    recentRxErrors += rxErrors - previousRxErrors;
    previousRxErrors = rxErrors;
    lastRxError = now;
    rxFault = recentRxErrors >= 3;
  }
  const auto link = txBlocked ? SensorIssue::LoraTxBlocked : rxFault ? SensorIssue::LoraRxFrameError : SensorIssue::None;
  sensorHealthChanged |= storage != sdIssue || link != loraIssue;
  sdIssue = storage;
  loraIssue = link;
}

void onRadioPacket(const String& payload) {
  JsonDocument data;
  if (deserializeJson(data, payload) || String(data["deviceId"] | "") != DEVICE_ID) return;
  String kind = data["kind"] | "";
  if (kind == "sensorHealthAck") {
    if (String(data["sessionId"] | "") == sessionId && data["sequenceNumber"].as<uint32_t>() == sensorHealthSequence) {
      pendingSensorHealth = "";
      ++healthAcksReceived;
      NodeLog::debugf("Sensor health acknowledged seq=%lu\n", static_cast<unsigned long>(sensorHealthSequence));
    }
  } else if (kind == "telemetryAck") {
    auto matches = [&](const String& value) {
      JsonDocument queued;
      return !value.isEmpty() && !deserializeJson(queued, value) &&
        String(data["sessionId"] | "") == String(queued["sessionId"] | "") &&
        data["sequenceNumber"].as<uint32_t>() == queued["sequenceNumber"].as<uint32_t>();
    };
    const bool fifoAck = matches(pendingPayload);
    const bool liveAck = matches(livePendingPayload);
    if (!fifoAck && !liveAck) return;
    if (fifoAck) {
      if (pendingPayload == volatilePayload) volatilePayload = "";
      else if (!outbox.acknowledge()) return; // Keep retrying if durable cursor save fails.
      if (latestLivePayload == pendingPayload) latestLivePayload = "";
      pendingPayload = "";
    }
    if (liveAck) {
      JsonDocument reading;
      deserializeJson(reading, livePendingPayload);
      lastLiveAckTimestamp = String(reading["timestamp"] | "unavailable");
      ++liveAcksReceived;
      if (latestLivePayload == livePendingPayload) latestLivePayload = "";
      livePendingPayload = "";
      // The SD copy stays intact until it reaches the FIFO head and is ACKed there.
    }
    ++telemetryAcksReceived;
    lastTelemetryAck = millis();
    retryDelay = 1000;
    lastTransmit = lastTelemetryAck - retryDelay;
  } else if (kind == "command") {
    ++commandPacketsReceived;
    String newMode = data["mode"] | "", command = data["command"] | "";
    String id = data["commandId"] | "";
    const String newAutoType = data["autoType"] | "scheduled";
    if ((newMode == "AUTO" && newAutoType != "scheduled" && newAutoType != "full_time") ||
        (newMode == "MANUAL" && !data["autoType"].isNull())) return;
    uint64_t version = data["version"] | uint64_t(0);
    if (id.isEmpty() || (newMode != "AUTO" && newMode != "MANUAL") ||
        (newMode == "MANUAL" && command != "ON" && command != "OFF")) return;
    if (version < appliedVersion || (version == appliedVersion && !appliedCommandId.isEmpty() && id != appliedCommandId)) return;
    if (id != appliedCommandId) {
      if (!data["expiresAtUnixSeconds"].isNull()) {
        if (!rtcTimeValid) {
          NodeLog::debugln("LoRa command skipped: RTC unavailable");
          return;
        }
      DateTime now(2000, 1, 1);
        // DS3231 stores Vietnam local time; bridge expiry uses UTC Unix seconds.
        const int64_t expiresAt = data["expiresAtUnixSeconds"] | int64_t(0);
      if (readRtcClock(now) != SensorIssue::None || expiresAt <= int64_t(now.unixtime()) - 7 * 3600) {
          NodeLog::debugln("LoRa command skipped: expired or RTC unavailable");
          return;
        }
      }
      JsonVariantConst schedule = data["autoSchedule"].as<JsonVariantConst>();
      int day = schedule["dayStartMinute"] | -1;
      int night = schedule["nightStartMinute"] | -1;
      if (newAutoType == "full_time" && !schedule.isNull()) return;
      if (!schedule.isNull() && (day < 0 || day >= 1440 || night < 0 || night >= 1440 || day == night)) return;
      if (schedule.isNull() && newAutoType != "full_time") {
        data["autoSchedule"]["dayStartMinute"] = dayStartMinute;
        data["autoSchedule"]["nightStartMinute"] = nightStartMinute;
      }
      if (newMode == "AUTO") data["autoType"] = newAutoType;
      else data.remove("autoType");
      String saved; serializeJson(data, saved);
      const String previousSaved = controls.getString("command", "");
      if (!controlsReady || controls.putString("command", saved) != saved.length()) {
        NodeLog::debugln("ERROR: cannot persist command; no command ACK"); return;
      }
      DateTime executionClock(2000, 1, 1);
      if (readRtcClock(executionClock) != SensorIssue::None) {
        controls.putString("command", previousSaved);
        return;
      }
      const int64_t executedAt = int64_t(executionClock.unixtime()) - 7 * 3600;
      if (!data["expiresAtUnixSeconds"].isNull() && executedAt >= (data["expiresAtUnixSeconds"] | int64_t(0))) {
        controls.putString("command", previousSaved);
        return;
      }
      dayStartMinute = data["autoSchedule"]["dayStartMinute"] | AutoLighting::DayStartMinute;
      nightStartMinute = data["autoSchedule"]["nightStartMinute"] | AutoLighting::NightStartMinute;
      autoType = newAutoType;
      mode = newMode; manualOn = command == "ON"; appliedCommandId = id; appliedVersion = version;
      autoSamples.reset();
      controlLamp(false);
      appliedExecutedAt = executedAt;
      // Save the execution receipt only after changing the output; retries reuse this timestamp.
      JsonDocument receipt;
      receipt["commandId"] = appliedCommandId;
      receipt["executedAt"] = appliedExecutedAt;
      String receiptPayload; serializeJson(receipt, receiptPayload);
      controls.putString("receipt", receiptPayload);
      NodeLog::debugf("LoRa command applied: %s %s\n", mode.c_str(), command.c_str());
    }
    JsonDocument ack;
    ack["kind"] = "commandAck"; ack["deviceId"] = DEVICE_ID; ack["commandId"] = appliedCommandId;
    ack["mode"] = mode; ack["lampCommand"] = lampIsOn ? "ON" : "OFF";
    if (mode == "AUTO") ack["autoType"] = autoType;
    if (appliedExecutedAt > 0) ack["executedAtUnixSeconds"] = appliedExecutedAt;
    commandAck = ""; serializeJson(ack, commandAck);
  }
}
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  NodeLog::debugln("Node firmware: telemetry TX diagnostics enabled");
  // Set the output latch before enabling the driver so the relay starts off.
  gpio_set_level(static_cast<gpio_num_t>(RELAY_PIN), RELAY_OFF_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  char id[24]; snprintf(id, sizeof(id), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  sessionId = id;
  controlsReady = controls.begin("luxmap-control", false);
  if (controlsReady) {
    JsonDocument saved;
    if (!deserializeJson(saved, controls.getString("command", ""))) {
      String restored = saved["mode"] | "AUTO";
      JsonDocument receipt;
      deserializeJson(receipt, controls.getString("receipt", ""));
      // Do not execute a command saved just before a power cut but never applied.
      const bool confirmed = saved["expiresAtUnixSeconds"].isNull() ||
        String(receipt["commandId"] | "") == String(saved["commandId"] | "");
      if (confirmed && (restored == "AUTO" || restored == "MANUAL")) {
        autoType = saved["autoType"] | "scheduled";
        if (autoType != "scheduled" && autoType != "full_time") autoType = "scheduled";
        mode = restored; manualOn = String(saved["command"] | "") == "ON";
        appliedCommandId = saved["commandId"] | ""; appliedVersion = saved["version"] | uint64_t(0);
        if (String(receipt["commandId"] | "") == appliedCommandId)
          appliedExecutedAt = receipt["executedAt"] | int64_t(0);
        dayStartMinute = saved["autoSchedule"]["dayStartMinute"] | AutoLighting::DayStartMinute;
        nightStartMinute = saved["autoSchedule"]["nightStartMinute"] | AutoLighting::NightStartMinute;
      }
    }
  }
  Wire.begin(21, 22); Wire.setClock(100000);
  for (int attempt = 0; attempt < 5 && !rtcReady; ++attempt) {
    rtcReady = rtc.begin(); if (!rtcReady) delay(100);
  }
  if (rtcReady) {
    rtcTimeValid = readRtcClock(rtcSample) == SensorIssue::None;
    if (rtcTimeValid) {
      char localTime[20];
      formatRtcTime(rtcSample, localTime);
      NodeLog::debugf("DS3231 retained: %s +07:00\n", localTime);
    } else NodeLog::debugln("DS3231 time invalid; use RTC SET yyyy-MM-ddTHH:mm:ss");
  } else NodeLog::debugln("DS3231 unavailable on I2C");
  inaReady = ina226.begin();
  inaConfiguredAt = millis();
  NodeLog::debugf("INA226 %s, I2C=0x%02X, shunt=%.3f ohm\n",
    inaReady ? "ready" : "not found/configuration failed", INA226_ADDRESS, INA226_SHUNT_OHMS);
  NodeLog::debugln("INA226 sampling: quiet windows, 16 averages, R002");
  luxReady = lightSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  const bool storageReady = outbox.begin(sessionId);
  NodeLog::debugln(storageReady ? "MicroSD outbox ready" : "ERROR: MicroSD unavailable");
  radio.begin(onRadioPacket);
  const unsigned long now = millis();
  lastSensor = lastElectricalSample = now - SENSOR_INTERVAL;
  lastSample = now - SAMPLE_INTERVAL;
  lastTransmit = now - RETRY_INTERVAL;
  NodeLog::debugf("LuxMap node %s session=%s\n", DEVICE_ID, sessionId.c_str());
}

void logTelemetryQueued(const String& payload) {
  if (!NodeLog::DebugEnabled) return;
  JsonDocument data;
  deserializeJson(data, payload);
  NodeLog::debugf("Telemetry queued for LoRa session=%s seq=%lu source=%s AUX=%s\n",
    data["sessionId"] | "-",
    data["sequenceNumber"] | 0UL, payload == volatilePayload ? "RAM" : "SD",
    radio.auxHigh() ? "HIGH" : "LOW");
}

String telemetryForRadio(const String& storedPayload) {
  JsonDocument data;
  if (deserializeJson(data, storedPayload)) return storedPayload;
  // The command ACK carries commandId separately. This optional duplicate field
  // is unused by telemetry ingest; keep the original SD record unchanged.
  data.remove("appliedCommandId");
  const bool fresh = String(data["sessionId"] | "") == sessionId &&
    !data["captureUptime"].isNull() && millis() - data["captureUptime"].as<unsigned long>() <= 2500;
  data["receptionType"] = fresh ? "live" : "backfill";
  data.remove("captureUptime");
  // Wire precision remains finer than the meter's resolution and matches Serial.
  for (const char* name : { "lux", "voltage", "current", "power" }) {
    if (data[name].isNull()) continue;
    const double value = data[name].as<double>();
    if (isfinite(value)) data[name] = round(value * 1000.0) / 1000.0;
  }
  String payload;
  serializeJson(data, payload);
  return payload;
}

void loop() {
  radio.poll();
  if (!radioTransmitPaused && !radio.busy() && !commandAck.isEmpty() && radio.sendJson(commandAck, true)) {
    ++commandAcksQueued;
    commandAck = "";
    radio.poll();
  }
  handleRtcSerial();

  outbox.inspectStep();
  if (outbox.inspection.complete) {
    const auto& queue = outbox.inspection;
    Serial.printf("QUEUE GET status=%s sd_pending=%lu invalid_records=%lu cursor=%llu snapshot_bytes=%llu first_measured_local=%s last_measured_local=%s timezone=+07:00\n",
      queue.readError ? "read_error" : "ok", (unsigned long)queue.samples,
      (unsigned long)queue.invalid, (unsigned long long)queue.cursor, (unsigned long long)queue.bytes,
      queue.firstTimestamp.isEmpty() ? "none" : queue.firstTimestamp.c_str(),
      queue.lastTimestamp.isEmpty() ? "none" : queue.lastTimestamp.c_str());
    JsonDocument pending;
    deserializeJson(pending, pendingPayload);
    Serial.printf("QUEUE GET inflight=%s source=%s session=%s seq=%lu measured_local=%s ram_pending=%s\n",
      pendingPayload.isEmpty() ? "no" : "yes",
      pendingPayload.isEmpty() ? "none" : pendingPayload == volatilePayload ? "RAM" : "SD",
      pending["sessionId"] | "none", pending["sequenceNumber"] | 0UL,
      pending["timestamp"] | "none", volatilePayload.isEmpty() ? "no" : "yes");
    outbox.inspection.complete = false;
  }

  unsigned long now = millis();
  if (now - lastSensor >= SENSOR_INTERVAL) { lastSensor = now; readEnvironment(); controlLamp(true); }
  now = millis();
  sampleElectrical(now);
  if (now - lastRamSaveAttempt >= SAMPLE_INTERVAL) {
    lastRamSaveAttempt = now;
    if (!volatilePayload.isEmpty() && pendingPayload != volatilePayload && outbox.append(volatilePayload)) {
      volatilePayload = "";
      NodeLog::debugln("MicroSD recovered; RAM record saved");
    }
  }
  updateHardwareHealth(now);
  // Keep data frames paced without making electrical reads wait for radio ACKs.
  const bool canStartDataFrame = !radioTransmitPaused && measurementsSinceDataFrame >= MEASUREMENTS_PER_DATA_FRAME &&
    now - lastElectricalSample < SENSOR_INTERVAL && commandAck.isEmpty();
  // Give the gateway time to return the telemetry ACK on the half-duplex radio.
  if (canStartDataFrame && !radio.busy() &&
      (pendingPayload.isEmpty() || radio.quietFor(TELEMETRY_ACK_LISTEN_MS)) && sendSensorHealth(now))
    measurementsSinceDataFrame = 0;
  if (canStartDataFrame && measurementsSinceDataFrame >= MEASUREMENTS_PER_DATA_FRAME &&
      !radio.busy() && now - lastTransmit >= retryDelay) {
    if (pendingPayload.isEmpty() && now - lastQueueCheck >= 1000) {
      lastQueueCheck = now;
      if (!outbox.peek(pendingPayload)) pendingPayload = volatilePayload;
    }
    const bool sendLive = !latestLivePayload.isEmpty() && (preferLive || pendingPayload.isEmpty());
    const String& selected = sendLive ? latestLivePayload : pendingPayload;
    if (!selected.isEmpty() && radio.sendJson(telemetryForRadio(selected))) {
      if (sendLive) {
        livePendingPayload = selected;
        ++liveFramesQueued;
      }
      preferLive = !sendLive;
      ++telemetryFramesQueued;
      measurementsSinceDataFrame = 0;
      logTelemetryQueued(selected);
      lastTransmit = now; retryDelay = RETRY_INTERVAL + esp_random() % 250;
    }
  }
  static unsigned long lastRadioReport = 0;
  if (NodeLog::DebugEnabled && now - lastRadioReport >= 5000) {
    lastRadioReport = now;
    NodeLog::debugf("LoRa TX status AUX=%s busy=%s telemetry_pending_bytes=%u\n",
      radio.auxHigh() ? "HIGH" : "LOW", radio.busy() ? "yes" : "no",
      static_cast<unsigned>(pendingPayload.length()));
  }
  delay(2);
}
