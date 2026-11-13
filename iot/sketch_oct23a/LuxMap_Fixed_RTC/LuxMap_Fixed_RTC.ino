
#include <WiFi.h>

#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <RTClib.h>
#include <BH1750.h>
#include "NetworkConfig.h"

// ==================== 1. WIFI ====================

const char* DEVICE_ID = "LUXMAP-P001";

// ==================== 2. PIN ====================

#define SDA_PIN 21
#define SCL_PIN 22
#define RELAY_PIN 27

// Relay đã thử bật/tắt được với mức HIGH
const int RELAY_ON_LEVEL = HIGH;
const int RELAY_OFF_LEVEL = LOW;

// ==================== 3. SENSORS ====================

Adafruit_INA219 ina219;
RTC_DS3231 rtc;
BH1750 lightSensor;

bool ina219Ready = false;
bool rtcReady = false;
bool bh1750Ready = false;

// ==================== 4. CONTROL ====================

enum ControlMode {
  MODE_AUTO,
  MODE_MANUAL
};

ControlMode controlMode = MODE_AUTO;

bool manualLampCommand = false;
bool lampIsOn = false;

float luxValue = 0.0;
float voltageValue = 0.0;
float currentValue = 0.0;
float powerValue = 0.0;

// Ngưỡng điều khiển ban ngày
const float LUX_ON_THRESHOLD = 50.0;
const float LUX_OFF_THRESHOLD = 100.0;

// ==================== 5. TIMERS ====================

unsigned long lastSensorTime = 0;

unsigned long lastTelemetryTime = 0;
unsigned long lastWiFiTime = 0;

const unsigned long SENSOR_INTERVAL = 1000;

const unsigned long TELEMETRY_INTERVAL = 10000;
const unsigned long WIFI_INTERVAL = 10000;

#include "MqttTransport.h"

// ==================== 6. WIFI ====================

void connectWiFi() {
  startNetworkConfiguration();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED || configuredSsid.isEmpty() || networkApplyPending) {
    return;
  }

  if (millis() - lastWiFiTime >= WIFI_INTERVAL) {
    lastWiFiTime = millis();

    Serial.println("Reconnecting WiFi...");
    WiFi.disconnect();
    WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
  }
}

// ==================== 7. RELAY ====================

void setLamp(bool turnOn) {
  int relayLevel = turnOn
      ? RELAY_ON_LEVEL
      : RELAY_OFF_LEVEL;

  // Luôn ghi mức GPIO yêu cầu
  digitalWrite(RELAY_PIN, relayLevel);

  if (lampIsOn != turnOn) {
    lampIsOn = turnOn;

    Serial.println("----------------------------");
    Serial.print("GPIO27 = ");
    Serial.println(relayLevel == HIGH ? "HIGH" : "LOW");

    Serial.print("Lamp command = ");
    Serial.println(lampIsOn ? "ON" : "OFF");

    Serial.println("----------------------------");
  }
}

// ==================== 8. READ SENSORS ====================

void readSensors() {
  // Đọc BH1750
  if (bh1750Ready) {
    float newLux = lightSensor.readLightLevel();

    if (newLux >= 0) {
      luxValue = newLux;
    }
  }

  // Đọc INA219
  if (ina219Ready) {
    float busVoltage = ina219.getBusVoltage_V();
    float shuntVoltage = ina219.getShuntVoltage_mV();

    currentValue = ina219.getCurrent_mA();
    powerValue = ina219.getPower_mW();

    voltageValue = busVoltage + shuntVoltage / 1000.0;
  }

  Serial.println();
  Serial.println("========== LUXMAP ==========");

  Serial.print("Lux: ");
  Serial.print(luxValue, 2);
  Serial.println(" lx");

  Serial.print("Voltage: ");
  Serial.print(voltageValue, 3);
  Serial.println(" V");

  Serial.print("Current: ");
  Serial.print(currentValue, 3);
  Serial.println(" mA");

  Serial.print("Power: ");
  Serial.print(powerValue, 3);
  Serial.println(" mW");

  Serial.print("Mode: ");
  Serial.println(
      controlMode == MODE_AUTO ? "AUTO" : "MANUAL"
  );

  Serial.print("Lamp command: ");
  Serial.println(lampIsOn ? "ON" : "OFF");

  if (rtcReady) {
    DateTime now = rtc.now();

    Serial.printf(
      "RTC time: %02d:%02d:%02d\n",
      now.hour(),
      now.minute(),
      now.second()
    );
  }

  Serial.println("============================");
}

// ==================== 9. AUTOMATIC CONTROL ====================

void runAutomaticControl() {
  if (!rtcReady) {
    Serial.println("RTC unavailable: cannot check schedule");
    return;
  }

  DateTime now = rtc.now();

  int currentMinutes = now.hour() * 60 + now.minute();

  // Ban ngày: 06:00 đến 17:30
  bool isDaytimeWindow =
      currentMinutes >= 360 &&
      currentMinutes <= 1050;

  if (!isDaytimeWindow) {
    // 17:31 đến 05:59: bật đèn
    setLamp(true);
    return;
  }

  // Ban ngày: điều khiển theo lux
  if (luxValue <= LUX_ON_THRESHOLD) {
    setLamp(true);
  } else if (luxValue >= LUX_OFF_THRESHOLD) {
    setLamp(false);
  }

  // Từ 50 đến 100 lux: giữ trạng thái hiện tại
}

// ==================== 11. SEND TELEMETRY ====================

void sendTelemetry() {
  if (!mqtt.connected()) return;

  JsonDocument doc;

  doc["deviceId"] = DEVICE_ID;
  doc["mode"] =
      controlMode == MODE_AUTO ? "AUTO" : "MANUAL";

  doc["lampCommand"] = lampIsOn ? "ON" : "OFF";

  doc["lux"] = luxValue;
  doc["voltage"] = voltageValue;
  doc["current"] = currentValue;
  doc["power"] = powerValue;

  if (rtcReady) {
    DateTime now = rtc.now();

    char timestamp[25];

    snprintf(
      timestamp,
      sizeof(timestamp),
      "%04d-%02d-%02dT%02d:%02d:%02d",
      now.year(),
      now.month(),
      now.day(),
      now.hour(),
      now.minute(),
      now.second()
    );

    doc["timestamp"] = timestamp;
  }

  String payload;
  serializeJson(doc, payload);

  Serial.println("Sending telemetry:");
  Serial.println(payload);

  bool sent = mqtt.publish(telemetryTopic.c_str(), payload.c_str(), false);
  Serial.println(sent ? "MQTT telemetry sent" : "MQTT telemetry failed");
}

// ==================== 12. SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(500);

  // Đặt relay OFF ngay khi khởi tạo
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  Serial.println();
  Serial.println("Starting LuxMap...");

  // DS3231 - initialize first, retry up to 5 times
  rtcReady = false;

  for (int i = 0; i < 5; i++) {
    if (rtc.begin(&Wire)) {
      rtcReady = true;
      break;
    }

    Serial.printf("RTC initialization attempt %d failed\\n", i + 1);
    delay(500);
  }

  if (rtcReady) {
    Serial.println("DS3231 initialized successfully");

    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("RTC adjusted to compile time");
    }

    DateTime now = rtc.now();

    Serial.printf(
      "RTC time: %04d-%02d-%02d %02d:%02d:%02d\\n",
      now.year(), now.month(), now.day(),
      now.hour(), now.minute(), now.second()
    );
  } else {
    Serial.println("ERROR: DS3231 initialization failed after 5 attempts");
  }

  // INA219
  ina219Ready = ina219.begin();

  if (ina219Ready) {
    ina219.setCalibration_32V_2A();
    Serial.println("INA219 initialized");
  } else {
    Serial.println("ERROR: INA219 not found");
  }

  // BH1750
  bh1750Ready = lightSensor.begin(
      BH1750::CONTINUOUS_HIGH_RES_MODE
  );

  if (bh1750Ready) {
    Serial.println("BH1750 initialized");
  } else {
    Serial.println("ERROR: BH1750 not found");
  }

  connectWiFi();
  setupMqtt();

  // Đọc cảm biến lần đầu
  readSensors();

  // AUTO được dùng mặc định khi khởi động
  runAutomaticControl();

  Serial.println("LuxMap setup completed");
}

// ==================== 13. MAIN LOOP ====================

void loop() {
  handleNetworkConfiguration();
  maintainWiFi();
  maintainMqtt();

  unsigned long currentTime = millis();

  // Đọc cảm biến mỗi giây
  if (currentTime - lastSensorTime >= SENSOR_INTERVAL) {
    lastSensorTime = currentTime;
    readSensors();
  }

  // Thực hiện chế độ điều khiển hiện tại
  if (controlMode == MODE_MANUAL) {
    setLamp(manualLampCommand);
  } else {
    runAutomaticControl();
  }

  // Gửi dữ liệu mỗi 10 giây
  if (telemetryDue || currentTime - lastTelemetryTime >= TELEMETRY_INTERVAL) {
    telemetryDue = false;
    lastTelemetryTime = currentTime;
    sendTelemetry();
  }

  delay(10);
}

