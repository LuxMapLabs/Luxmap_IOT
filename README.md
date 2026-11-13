# LuxMap: node LoRa + gateway MQTT + server C#

## Cấu trúc

- `iot/espnode/espnode.ino`: ESP32 tại cột đèn, đọc cảm biến, điều khiển relay, lưu MicroSD, truyền nhận E32 qua UART.
- `iot/espserver/espserver.ino`: ESP32 gateway, nhận E32, chuyển dữ liệu MQTT, chuyển lệnh/ACK từ server về node. ESP này không chạy server C#.
- `server/`: ASP.NET Core API/Swagger và MQTT broker tích hợp.
- `iot/sketch_oct23a/`: bản firmware Wi-Fi trực tiếp cũ, giữ để tham khảo; không dùng cho kiến trúc LoRa mới.

```text
INA226/BH1750/DS3231 → espnode + MicroSD ↔ E32 433MHz ↔ E32 + espserver ↔ Wi-Fi/MQTT ↔ C# server
                                             Lệnh từ Swagger đi theo chiều ngược lại
```

## Chạy server và nạp firmware

1. Mở `iot/espnode/espnode.ino` bằng Arduino IDE, chọn đúng ESP32 DevKit và nạp cho board tại đèn. Giữ các header đi kèm trong thư mục.
2. Mở `iot/espserver/espserver.ino`, nạp cho board gateway. Giữ thư mục `src` (PubSubClient 2.8 kèm giấy phép MIT); không cần tự cài PubSubClient.
3. Server giữ dữ liệu cũ trong `server/data/devices.json`. Chạy:

```powershell
cd "E:\Ki9\DoAn\iot\code iot"
dotnet run --project server/LuxMap.Api.csproj
```

Dừng server cũ bằng Ctrl+C trước nếu đang giữ cổng 5000/1883. Swagger: http://localhost:5000/swagger. MQTT TCP: cổng 1883. Không cần Mosquitto riêng.

4. Chỉ gateway cần Wi-Fi. Kết nối Wi-Fi `LUXMAP-GATEWAY-XXXX`, mật khẩu `luxmap123`; mở http://192.168.4.1, đăng nhập `admin` / `luxmap123`. Nhập Wi-Fi và `mqtt://IP_MAY_SERVER:1883`, lưu. Node không cần nhập Wi-Fi. Gateway tự đọc cấu hình mạng cũ nếu board đã có.
5. Cho phép máy gateway truy cập TCP 1883 trên máy server qua firewall; TCP 5000 chỉ cần cho Swagger/API từ máy khác. Không tắt toàn bộ firewall.
6. Swagger: POST `/api/luxmap/devices/LUXMAP-P001/command`, chọn ON/OFF/AUTO. Lệnh đi MQTT → gateway → LoRa → node. `commandAcknowledged` trong GET trạng thái chỉ thành true khi server nhận đúng commandId từ node. Đây là xác nhận phần mềm node đã xử lý/GPIO đã được đặt, không phải cảm biến xác nhận bóng đèn thực sự sáng.

## ID và phân quyền gateway

Mặc định node `LUXMAP-P001`, gateway `LUXMAP-GW001`. Khai báo ở đầu hai sketch và `Mqtt:Gateways` trong appsettings.json phải khớp. Cấu hình mặc định là một node/một gateway. Firmware gateway hiện chỉ chuyển tiếp NODE_ID đã cấu hình; nhiều node cần mở rộng bộ lập lịch radio của gateway, không chỉ thêm tên vào JSON server.

MQTT client ID phải là gateway đã đăng ký. Gateway chỉ được publish telemetry/commandAck và subscribe command/telemetryAck của các node được gán. Server không còn dùng client ID làm node ID. Danh sách gán node: GET `/api/luxmap/gateways`.

Đây vẫn là bản LAN tin cậy, chưa có xác thực MQTT/TLS. Mapping ID chỉ hạn chế topic, không thay thế xác thực thiết bị; không mở broker ra Internet.

## Đấu dây (ESP32 DevKit WROOM, kiểm tra board thực tế)

| Thiết bị | Chân module | GPIO ESP32 |
|---|---|---|
| INA226/BH1750/DS3231 (node) | SDA / SCL | 21 / 22 |
| Relay (node) | IN | 27 |
| MicroSD SPI (node) | SCK / MISO / MOSI / CS | 18 / 19 / 23 / 5 |
| E32 (cả hai board) | TXD → RX / RXD ← TX | 16 / 17 |
| E32 (cả hai board) | AUX | 4 |
| E32 (cả hai board) | M0 / M1 | GND / GND |

Cấp nguồn đúng loại module, nối chung GND. E32 cần nguồn đủ dòng khi phát và anten 433MHz; không đưa 5V vào GPIO. MicroSD dùng đúng điện áp của module và thẻ FAT32. GPIO16/17 phải có sẵn trên board; GPIO5 là chân strapping của ESP32 cổ điển, nếu SD làm board không boot cần đổi CS sang GPIO phù hợp và sửa code.

INA226 dùng thư viện Wire có sẵn, địa chỉ mặc định `0x40`. Kiểm tra chữ in trên shunt và đặt `INA226_SHUNT_OHMS` trong sketch trước khi nạp: `R100` là 0,100 Ω, `R010` là 0,010 Ω. Nếu module có chân VBUS riêng, nối VBUS với IN− để đo phía tải. Với R100, dải đo dòng xấp xỉ ±0,819 A theo giới hạn đầu vào shunt; chọn module/shunt phù hợp nếu tải vượt dải. Telemetry giữ V, mA, mW.

Hai E32 phải cùng cấu hình UART 9600 8N1, air rate, channel và chế độ transparent, M0/M1 LOW. Firmware không tự ghi lại cấu hình E32; các phiên bản module có thể khác kênh mặc định, phải kiểm tra cặp đang có. AUX cần nối để điều tiết truyền. Đây là LoRa điểm–điểm qua UART E32, không phải LoRaWAN.

## Lưu offline, ACK và chống trùng

Node đọc cảm biến mỗi giây, tạo bản ghi mỗi 10 giây. Mỗi lần khởi động tạo sessionId mới, mỗi bản ghi tăng sequenceNumber. Bản ghi JSON có CRC16 được append và flush vào `/telemetry.log` trên SD trước khi gửi radio. Khung LoRa có dạng `@JSON*CRC16\n`, gửi từng đoạn 48 byte có AUX và khoảng nghỉ; tối đa 768 byte/khung. Node giữ một bản ghi đang chờ server ACK, sau ACK chuyển sang bản tiếp theo; timeout 10–13 giây sẽ gửi lại. Có jitter để giảm đụng khung khi hai phía cùng thử lại.

Luồng dữ liệu: SD → LoRa → gateway → MQTT → server kiểm tra và lưu file thành công → MQTT telemetryAck → gateway → LoRa → node lưu ACK cursor vào NVS. Gateway nhận được dữ liệu hoặc MQTT publish trả true không được coi là server đã lưu. Khi Wi-Fi/server/LoRa lỗi, record chưa ACK vẫn nằm trên SD; gateway không cần SD.

Server dùng `(deviceId, sessionId, sequenceNumber)` để chống trùng. Ledger dải sequence được lưu cùng trạng thái, không bị xóa theo lịch sử hiển thị 1000 bản ghi. Gửi lại bản trùng vẫn nhận ACK; bản đến sai thứ tự vẫn được lưu, latest của cùng session không bị lùi sequence. Gửi bù có thể làm API hiển thị dữ liệu cũ trong thời gian đang đuổi backlog. `receivedAt` là UTC server nhận, `timestamp` là giờ RTC node đo, không đổi thành giờ gateway gửi.

Node giữ lệnh mới nhất trong NVS, có version và commandId để bỏ qua lệnh cũ và xử lý lại an toàn khi retransmit. Gateway thử lại lệnh qua LoRa đến khi có commandAck; nó phát lại commandAck lên server định kỳ để chịu mất gói MQTT QoS 0. Server bỏ qua ACK sai/cũ, không ghi file lặp lại khi cùng lệnh đã xác nhận. Nếu reset/xóa dữ liệu server hoặc thay node, cần kiểm tra lại version/lệnh đã lưu; không tự xóa NVS đang có dữ liệu chờ.

## Topic

| Topic | Chiều | Retain |
|---|---|---|
| `luxmap/LUXMAP-P001/telemetry` | gateway → server | Không |
| `luxmap/LUXMAP-P001/telemetryAck` | server → gateway → node | Không |
| `luxmap/LUXMAP-P001/command` | server → gateway → node | Có |
| `luxmap/LUXMAP-P001/commandAck` | node → gateway → server | Không |

Telemetry có deviceId, sessionId, sequenceNumber, timestamp tùy chọn, mode, lampCommand, lux (lx), voltage (V), current (mA), power (mW), appliedCommandId. Server chỉ ACK payload hợp lệ đã lưu hoặc xác định là trùng.

## Giới hạn và kiểm tra thực tế

- Chưa chạy 8 mẫu/giây. Radio 2.4kbps, ACK và JSON phù hợp bản đo thưa 10 giây; muốn tăng tốc phải đo airtime, đổi gói và lịch truyền.
- SD log append-only, giới hạn 64MiB/file trong bản prototype, không tự xóa record. Khi đủ hoặc lỗi thẻ, giữ tối đa một record trong RAM, tạm bỏ mẫu mới khi record đó chưa gửi được; Serial báo lỗi. RAM không chịu được mất nguồn. Định kỳ sao lưu/thay thẻ khi toàn bộ backlog đã ACK; thẻ mới có queue ID mới và cursor từ 0. Không dùng tính năng format/reset trong khi còn record chưa đồng bộ.
- File hỏng/ghi dở do mất điện được CRC phát hiện và báo trên Serial; không thể khôi phục dữ liệu vật lý đã hỏng. NVS cursor được cập nhật sau ACK; gián đoạn trước đó có thể gửi trùng, server xử lý được. Chưa cam kết chịu mọi trường hợp mất nguồn giữa thao tác hệ thống tệp.
- LoRa là half-duplex, lệnh có thể chờ khung đang truyền, timeout và retransmit; không cam kết phản hồi tức thời như Wi-Fi trực tiếp. CRC phát hiện lỗi truyền, không phải mã hóa/xác thực radio.
- AUTO từ 06:00 đến hết 18:00: một mẫu dưới 50 lux bật, một mẫu trên 50 lux tắt, đúng 50 lux giữ trạng thái. Từ 18:01 đến 05:59 luôn bật. Ban ngày, mẫu BH1750 lỗi giữ trạng thái; nếu không có RTC thì AUTO giữ trạng thái. Ngưỡng nằm trong `iot/espnode/AutoLighting.h`. Relay HIGH bật / LOW tắt. Node bỏ qua bản ghi nếu thiếu mẫu hợp lệ hoặc số đo điện thuộc trạng thái relay trước đó.
- Kiểm tra trên board: truyền đúng 2 chiều → gửi OFF từ Swagger và thấy commandAcknowledged=true → ngắt Wi-Fi gateway, chờ vài mẫu → bật lại và xác minh dữ liệu gửi bù → tắt/bật node khi còn backlog → xác minh không mất/trùng dữ liệu đã ghi đúng trên SD.

## Kiểm thử phần mềm

```powershell
dotnet build server/LuxMap.Api.csproj
dotnet run --project tests/MqttSmoke/MqttSmoke.csproj -- "E:\Ki9\DoAn\iot\code iot\server\bin\Debug\net8.0\LuxMap.Api.dll"
```

Test dùng cổng 15001/11883 và dữ liệu riêng, mô phỏng gateway MQTT, kiểm tra ACK sau commit, replay/out-of-order/dedup, ACK lệnh, whitelist topic và restart. Test này không thay thế kiểm tra sóng LoRa, dây nối và SD trên board thật.
#   L u x m a p _ I O T  
 