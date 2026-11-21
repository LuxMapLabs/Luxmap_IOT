#include <ArduinoJson.h>
#include <Wire.h>
#include <RTClib.h>
#include <BH1750.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <math.h>
#include "AutoLighting.h"
#include "Ina226Meter.h"
#include "RadioLink.h"
#include "SdOutbox.h"

const char* DEVICE_ID = "LUXMAP-P001";
const int RELAY_PIN = 27;
const int RELAY_ON_LEVEL = HIGH, RELAY_OFF_LEVEL = LOW;
const unsigned long SENSOR_INTERVAL = 1000, SAMPLE_INTERVAL = 10000, RETRY_INTERVAL = 10000;
constexpr uint8_t INA226_ADDRESS = 0x40;
constexpr float INA226_SHUNT_OHMS = 0.002f; // R002 on this module.
Ina226Meter ina226(INA226_ADDRESS, INA226_SHUNT_OHMS);
RTC_DS3231 rtc;
BH1750 lightSensor;
RadioLink radio;
SdOutbox outbox;
Preferences controls;
bool inaReady = false, rtcReady = false, rtcTimeValid = false, luxReady = false, controlsReady = false;
bool electricalSampleValid = false, luxSampleValid = false;
bool lampIsOn = false, manualOn = false;
int dayStartMinute = AutoLighting::DayStartMinute;
int nightStartMinute = AutoLighting::NightStartMinute;
String autoType = "scheduled";
String mode = "AUTO", appliedCommandId = "", sessionId;
uint64_t appliedVersion = 0;
uint32_t sequenceNumber = 0;
float luxValue = 0, voltageValue = 0, currentValue = 0, powerValue = 0;
unsigned long lastSensor = 0, lastSample = 0, lastTransmit = 0;
unsigned long retryDelay = RETRY_INTERVAL;
unsigned long lastQueueCheck = 0;
String pendingPayload, volatilePayload, commandAck;
char sampledAt[20] = "";

void formatRtcTime(const DateTime& time, char (&output)[20]) {
  snprintf(output, sizeof(output), "%04u-%02u-%02uT%02u:%02u:%02u",
    unsigned(time.year()), unsigned(time.month()), unsigned(time.day()),
    unsigned(time.hour()), unsigned(time.minute()), unsigned(time.second()));
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
          const DateTime readBack = rtc.now();
          rtcTimeValid = !rtc.lostPower() && readBack.isValid() &&
            readBack.unixtime() >= requested.unixtime() && readBack.unixtime() - requested.unixtime() <= 2;
          Serial.printf("DS3231 %s: %s +07:00\n", rtcTimeValid ? "set" : "set failed", canonical);
        } else Serial.println("DS3231: invalid date");
      } else Serial.println("DS3231: invalid date");
    }
    length = 0;
    overflow = false;
  }
}

void setLamp(bool on) {
  const int level = on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL;
  digitalWrite(RELAY_PIN, level);
  if (on != lampIsOn) {
    // The electrical sample belongs to the previous relay state.
    electricalSampleValid = false;
    Serial.printf("Lamp %s, GPIO%d=%s\n", on ? "ON" : "OFF", RELAY_PIN, level == HIGH ? "HIGH" : "LOW");
  }
  lampIsOn = on;
}
AutoLighting::ConsecutiveSamples autoSamples;

void controlLamp(bool freshSample) {
  if (mode == "MANUAL") { autoSamples.reset(); setLamp(manualOn); return; }
  const bool fullTime = autoType == "full_time";
  int minutes = 0;
  if (!fullTime) {
    if (!rtcTimeValid) { autoSamples.reset(); return; }
    const DateTime now = rtc.now();
    if (!now.isValid()) { autoSamples.reset(); return; }
    minutes = now.hour() * 60 + now.minute();
  }
  if (!AutoLighting::useSensor(fullTime, rtcTimeValid, minutes, dayStartMinute, nightStartMinute)) {
    autoSamples.reset();
    setLamp(true);
  } else if (freshSample) {
    const bool nextState = autoSamples.update(luxSampleValid, luxValue, lampIsOn);
    const int streak = nextState != lampIsOn ? AutoLighting::RequiredSamples : autoSamples.pendingSamples();
    if (streak > 0)
      Serial.printf("AUTO lux=%.2f target=%s samples=%d/%d\n",
        luxValue, lampIsOn ? "OFF" : "ON", streak, AutoLighting::RequiredSamples);
    setLamp(nextState);
  }
}
void readSensors() {
  sampledAt[0] = '\0';
  luxSampleValid = false;
  if (luxReady) {
    const float value = lightSensor.readLightLevel();
    luxSampleValid = isfinite(value) && value >= 0;
    if (luxSampleValid) luxValue = value;
  }
  electricalSampleValid = inaReady && ina226.read(voltageValue, currentValue, powerValue);
  if (!electricalSampleValid) {
    Serial.println("INA226: no valid electrical sample (check I2C and shunt range)");
    return;
  }
  if (rtcTimeValid) {
    const DateTime measuredAt = rtc.now();
    if (measuredAt.isValid()) formatRtcTime(measuredAt, sampledAt);
  }
  Serial.printf("lux=%.2f V=%.3f mA=%.3f mW=%.3f mode=%s lamp=%s\n",
    luxValue, voltageValue, currentValue, powerValue, mode.c_str(), lampIsOn ? "ON" : "OFF");
}
void captureTelemetry() {
  if (!electricalSampleValid || !luxSampleValid || !isfinite(voltageValue) ||
      !isfinite(currentValue) || !isfinite(powerValue) || !volatilePayload.isEmpty() || sequenceNumber == UINT32_MAX) {
    Serial.println("Sample skipped: sensor unavailable or unsaved RAM record waiting"); return;
  }
  JsonDocument data;
  data["kind"] = "telemetry"; data["deviceId"] = DEVICE_ID;
  data["sessionId"] = sessionId; data["sequenceNumber"] = ++sequenceNumber;
  data["mode"] = mode; data["lampCommand"] = lampIsOn ? "ON" : "OFF";
  if (mode == "AUTO") data["autoType"] = autoType;
  data["lux"] = luxValue; data["voltage"] = voltageValue;
  data["current"] = currentValue; data["power"] = powerValue;
  data["appliedCommandId"] = appliedCommandId;
  if (sampledAt[0]) data["timestamp"] = sampledAt;
  String payload; serializeJson(data, payload);
  if (!outbox.append(payload)) {
    volatilePayload = payload;
    Serial.println("ERROR: SD append failed/unavailable; one RAM record retained (lost on power off)");
  } else Serial.printf("MicroSD saved seq=%lu timestamp=%s\n", static_cast<unsigned long>(sequenceNumber),
    sampledAt[0] ? sampledAt : "unavailable");
}
void onRadioPacket(const String& payload) {
  JsonDocument data;
  if (deserializeJson(data, payload) || String(data["deviceId"] | "") != DEVICE_ID) return;
  String kind = data["kind"] | "";
  if (kind == "telemetryAck" && !pendingPayload.isEmpty()) {
    JsonDocument pending; if (deserializeJson(pending, pendingPayload)) return;
    if (String(data["sessionId"] | "") != String(pending["sessionId"] | "") ||
        data["sequenceNumber"].as<uint32_t>() != pending["sequenceNumber"].as<uint32_t>()) return;
    if (pendingPayload == volatilePayload) volatilePayload = "";
    else if (!outbox.acknowledge()) { Serial.println("ERROR: ACK cursor not saved; will replay"); return; }
    Serial.printf("Server ACK received; record synchronized session=%s seq=%lu\n",
      data["sessionId"] | "-", data["sequenceNumber"] | 0UL);
    pendingPayload = ""; lastTransmit = millis() - 15000;
  } else if (kind == "command") {
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
          Serial.println("LoRa command skipped: RTC unavailable");
          return;
        }
        const DateTime now = rtc.now();
        // DS3231 stores Vietnam local time; bridge expiry uses UTC Unix seconds.
        const int64_t expiresAt = data["expiresAtUnixSeconds"] | int64_t(0);
        if (!now.isValid() || expiresAt <= int64_t(now.unixtime()) - 7 * 3600) {
          Serial.println("LoRa command skipped: expired or RTC unavailable");
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
      if (!controlsReady || controls.putString("command", saved) != saved.length()) {
        Serial.println("ERROR: cannot persist command; no command ACK"); return;
      }
      dayStartMinute = data["autoSchedule"]["dayStartMinute"] | AutoLighting::DayStartMinute;
      nightStartMinute = data["autoSchedule"]["nightStartMinute"] | AutoLighting::NightStartMinute;
      autoType = newAutoType;
      mode = newMode; manualOn = command == "ON"; appliedCommandId = id; appliedVersion = version;
      autoSamples.reset();
      controlLamp(false);
      Serial.printf("LoRa command applied: %s %s\n", mode.c_str(), command.c_str());
    }
    JsonDocument ack;
    ack["kind"] = "commandAck"; ack["deviceId"] = DEVICE_ID; ack["commandId"] = appliedCommandId;
    ack["mode"] = mode; ack["lampCommand"] = lampIsOn ? "ON" : "OFF";
    if (mode == "AUTO") ack["autoType"] = autoType;
    commandAck = ""; serializeJson(ack, commandAck);
  }
}
void setup() {
  Serial.begin(115200);
  Serial.println("Node firmware: telemetry TX diagnostics enabled");
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
      if (restored == "AUTO" || restored == "MANUAL") {
        autoType = saved["autoType"] | "scheduled";
        if (autoType != "scheduled" && autoType != "full_time") autoType = "scheduled";
        mode = restored; manualOn = String(saved["command"] | "") == "ON";
        appliedCommandId = saved["commandId"] | ""; appliedVersion = saved["version"] | uint64_t(0);
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
    const DateTime now = rtc.now();
    rtcTimeValid = !rtc.lostPower() && now.isValid();
    if (rtcTimeValid) {
      char localTime[20];
      formatRtcTime(now, localTime);
      Serial.printf("DS3231 retained: %s +07:00\n", localTime);
    } else Serial.println("DS3231 time invalid; use RTC SET yyyy-MM-ddTHH:mm:ss");
  } else Serial.println("DS3231 unavailable on I2C");
  inaReady = ina226.begin();
  Serial.printf("INA226 %s, I2C=0x%02X, shunt=%.3f ohm\n",
    inaReady ? "ready" : "not found/configuration failed", INA226_ADDRESS, INA226_SHUNT_OHMS);
  luxReady = lightSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.println(outbox.begin(sessionId) ? "MicroSD outbox ready" : "ERROR: MicroSD unavailable");
  radio.begin(onRadioPacket); delay(200); readSensors(); controlLamp(true); captureTelemetry();
  lastSensor = lastSample = millis(); lastTransmit = millis() - RETRY_INTERVAL;
  Serial.printf("LuxMap node %s session=%s\n", DEVICE_ID, sessionId.c_str());
}

void logTelemetryQueued(const String& payload) {
  JsonDocument data;
  deserializeJson(data, payload);
  Serial.printf("Telemetry queued for LoRa session=%s seq=%lu source=%s AUX=%s\n",
    data["sessionId"] | "-",
    data["sequenceNumber"] | 0UL, payload == volatilePayload ? "RAM" : "SD",
    radio.auxHigh() ? "HIGH" : "LOW");
}

void loop() {
  radio.poll();
  if (!radio.busy() && !commandAck.isEmpty() && radio.sendJson(commandAck, true)) {
    commandAck = "";
    radio.poll();
  }
  handleRtcSerial();
  unsigned long now = millis();
  if (now - lastSensor >= SENSOR_INTERVAL) { lastSensor = now; readSensors(); controlLamp(true); }
  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    if (!volatilePayload.isEmpty() && pendingPayload != volatilePayload && outbox.append(volatilePayload)) {
      volatilePayload = "";
      Serial.println("MicroSD recovered; RAM record saved");
    }
    captureTelemetry();
  }
  if (!radio.busy() && commandAck.isEmpty() && now - lastTransmit >= retryDelay) {
    if (pendingPayload.isEmpty() && now - lastQueueCheck >= 1000) {
      lastQueueCheck = now;
      if (!outbox.peek(pendingPayload)) pendingPayload = volatilePayload;
    }
    if (!pendingPayload.isEmpty() && radio.sendJson(pendingPayload)) {
      logTelemetryQueued(pendingPayload);
      lastTransmit = now; retryDelay = RETRY_INTERVAL + esp_random() % 3000;
    }
  }
  static unsigned long lastRadioReport = 0;
  if (now - lastRadioReport >= 5000) {
    lastRadioReport = now;
    Serial.printf("LoRa TX status AUX=%s busy=%s telemetry_pending_bytes=%u\n",
      radio.auxHigh() ? "HIGH" : "LOW", radio.busy() ? "yes" : "no",
      static_cast<unsigned>(pendingPayload.length()));
  }
  delay(2);
}
