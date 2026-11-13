#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// Built into the Arduino ESP32 core; no WiFiManager library required.
const char* CONFIG_PASSWORD = "luxmap123";
WebServer configWeb(80);
String configuredSsid;
String configuredPassword;
String configuredBroker;
bool networkApplyPending = false;
unsigned long networkSavedAt = 0;

String escapeConfigHtml(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

bool authorizeConfiguration() {
  if (configWeb.authenticate("admin", CONFIG_PASSWORD)) return true;
  configWeb.requestAuthentication();
  return false;
}

void showNetworkConfiguration() {
  if (!authorizeConfiguration()) return;
  String page = R"HTML(<!doctype html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cấu hình LuxMap</title><style>
body{font:16px system-ui;background:#eef3f8;color:#172338;margin:0;padding:24px}
main{max-width:480px;margin:20px auto;background:white;padding:24px;border-radius:16px}
label{display:block;margin-top:18px}input,button{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;font:inherit}
button{background:#145ac8;color:white;border:0;border-radius:8px;margin-top:24px}small{color:#526176}
</style></head><body><main><h1>Cấu hình LuxMap</h1><p>)HTML";
  page += WiFi.status() == WL_CONNECTED
    ? String("Đã kết nối Wi-Fi. IP ESP32: ") + WiFi.localIP().toString()
    : String("Chưa kết nối Wi-Fi. Kiểm tra tên mạng và mật khẩu.");
  page += R"HTML(</p><form method="post" action="/save">
<label>Tên Wi-Fi (SSID)<input name="ssid" maxlength="32" required value=")HTML";
  page += escapeConfigHtml(configuredSsid);
  page += R"HTML("></label><label>Mật khẩu Wi-Fi<input name="password" type="password" maxlength="63" autocomplete="new-password"></label>
<small>Để trống để giữ mật khẩu đã lưu. Nếu đổi Wi-Fi, nhập mật khẩu mới.</small>
<label><input style="width:auto" type="checkbox" name="open" value="1"> Mạng Wi-Fi không có mật khẩu</label>
<label>Địa chỉ MQTT broker<input name="server" type="text" maxlength="200" required placeholder="mqtt://192.168.1.20:1883" value=")HTML";
  page += escapeConfigHtml(configuredBroker);
  page += R"HTML("></label><small>Nhập mqtt://IP:1883, không thêm /api. Không dùng localhost.</small>
<button>Lưu và kết nối</button></form><p>Cấu hình vẫn được giữ sau khi tắt nguồn.</p></main></body></html>)HTML";
  configWeb.sendHeader("Cache-Control", "no-store");
  configWeb.send(200, "text/html; charset=utf-8", page);
}

bool validBrokerAddress(const String& address) {
  // LAN MQTT uses TCP without TLS. Reject credentials, paths and ambiguous URLs.
  if (!address.startsWith("mqtt://") || address.length() > 200) return false;
  String authority = address.substring(7);
  if (authority.isEmpty() || authority.indexOf('/') >= 0 || authority.indexOf('@') >= 0 ||
      authority.indexOf('?') >= 0 || authority.indexOf('#') >= 0) return false;
  int separator = authority.indexOf(':');
  String host = separator < 0 ? authority : authority.substring(0, separator);
  if (host.isEmpty() || host == "localhost" || host == "127.0.0.1" || host == "0.0.0.0") return false;
  for (unsigned int i = 0; i < host.length(); i++) {
    char c = host[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  }
  if (separator >= 0) {
    String port = authority.substring(separator + 1);
    if (port.isEmpty() || port.length() > 5) return false;
    for (unsigned int i = 0; i < port.length(); i++)
      if (port[i] < '0' || port[i] > '9') return false;
    if (port.toInt() < 1 || port.toInt() > 65535) return false;
  }
  return true;
}

void saveNetworkConfiguration() {
  if (!authorizeConfiguration()) return;
  String ssid = configWeb.arg("ssid");
  String password = configWeb.arg("password");
  String server = configWeb.arg("server");
  server.trim();
  while (server.endsWith("/")) server.remove(server.length() - 1);
  bool openNetwork = configWeb.arg("open") == "1";
  if (!openNetwork && password.isEmpty() && ssid == configuredSsid)
    password = configuredPassword;
  if (openNetwork) password = "";
  if (ssid.isEmpty() || ssid.length() > 32 ||
      (!openNetwork && (password.length() < 8 || password.length() > 63)) ||
      !validBrokerAddress(server)) {
    configWeb.send(400, "text/plain; charset=utf-8",
      "Thông tin không hợp lệ. SSID tối đa 32 byte, mật khẩu 8-63 byte (hoặc chọn mạng mở). "
      "Server dạng mqtt://IP:1883 hoặc mqtt://hostname:1883, không thêm /api. Quay lại để sửa.");
    return;
  }
  JsonDocument config;
  config["ssid"] = ssid;
  config["password"] = password;
  config["broker"] = server;
  String encoded;
  serializeJson(config, encoded);
  Preferences storage;
  if (!storage.begin("luxmap-net", false)) {
    configWeb.send(500, "text/plain; charset=utf-8", "Không mở được bộ nhớ cấu hình.");
    return;
  }
  // Store all fields together so they cannot become a mixture of two configurations.
  size_t written = storage.putString("config", encoded);
  storage.end();
  if (written != encoded.length()) {
    configWeb.send(500, "text/plain; charset=utf-8", "Lưu cấu hình thất bại. Hãy thử lại.");
    return;
  }
  configuredSsid = ssid;
  configuredPassword = password;
  configuredBroker = server;
  networkSavedAt = millis();
  networkApplyPending = true;
  configWeb.sendHeader("Cache-Control", "no-store");
  configWeb.send(200, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<h1>Đã lưu cấu hình</h1><p>ESP32 sẽ kết nối Wi-Fi mới. Chờ khoảng 15 giây rồi "
    "kết nối lại Wi-Fi cấu hình nếu cần và <a href='/'>kiểm tra trạng thái</a>. "
    "Nếu nhập sai, bạn vẫn có thể mở lại trang này để sửa.</p>");
}

void startNetworkConfiguration() {
  Preferences storage;
  if (storage.begin("luxmap-net", true)) {
    String encoded = storage.getString("config", "");
    storage.end();
    JsonDocument config;
    if (!deserializeJson(config, encoded)) {
      configuredSsid = config["ssid"] | "";
      configuredPassword = config["password"] | "";
      configuredBroker = config["broker"] | "";
      if (configuredBroker.isEmpty()) {
        String previous = config["server"] | "";
        if (previous.startsWith("http://")) {
          String host = previous.substring(7);
          int colon = host.indexOf(':' );
          if (colon >= 0) host = host.substring(0, colon);
          configuredBroker = String("mqtt://") + host + ":1883";
        }
      }
      if (!validBrokerAddress(configuredBroker)) configuredBroker = "";
    }
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  String apName = String("LUXMAP-SETUP-") + WiFi.macAddress().substring(12);
  apName.replace(":", "");
  if (!WiFi.softAP(apName.c_str(), CONFIG_PASSWORD))
    Serial.println("ERROR: cannot start configuration WiFi");
  configWeb.on("/", HTTP_GET, showNetworkConfiguration);
  configWeb.on("/save", HTTP_POST, saveNetworkConfiguration);
  configWeb.onNotFound([]() { configWeb.send(404, "text/plain", "Not found"); });
  configWeb.begin();
  Serial.print("Configuration WiFi: "); Serial.println(apName);
  Serial.print("Configuration page: http://"); Serial.println(WiFi.softAPIP());
  Serial.println("WiFi password: luxmap123; page login: admin / luxmap123");
  if (!configuredSsid.isEmpty())
    WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
}

void handleNetworkConfiguration() {
  configWeb.handleClient();
  // Send the HTTP response before changing the station connection.
  if (networkApplyPending && millis() - networkSavedAt >= 500) {
    WiFi.disconnect();
    WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
    networkApplyPending = false;
    Serial.println("Applying saved network configuration...");
  }
}

