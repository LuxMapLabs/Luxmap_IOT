# Server C# cho gateway LoRa–MQTT

Chạy `dotnet run --project server/LuxMap.Api.csproj` từ gốc dự án. Swagger tại http://localhost:5000/swagger, MQTT broker tích hợp cổng 1883.

`Mqtt:Gateways` trong appsettings.json ánh xạ gateway MQTT client ID sang danh sách node. Mặc định LUXMAP-GW001 → LUXMAP-P001. Firmware gateway hiện hỗ trợ một NODE_ID.

Đặt biến môi trường `Mqtt__ProvisioningKey` thành khóa quản trị ngẫu nhiên dài ít nhất 32 ký tự trước khi chạy bridge. Gọi `POST /api/luxmap/gateways/{gatewayId}/credential` qua localhost hoặc HTTPS nội bộ với header `X-Iot-Provisioning-Key`. Phản hồi trả `clientId`, `username`, `password` và `nodeIds`. Mỗi gateway được cấp một lần, lần sau trả 409. Lưu secret ngay khi nhận. Bridge chỉ lưu hash trong `data/gateway-credentials.json`, cần sao lưu cùng thư mục dữ liệu.

Đọc mật khẩu Wi-Fi cấu hình của từng gateway trên Serial sau khi nạp, kết nối Wi-Fi cấu hình và đăng nhập trang cấu hình bằng `admin` cùng mật khẩu đó. Nhập broker, Gateway ID, Node ID, username và secret vừa cấp. Gateway lưu các trường vào NVS. MQTT TCP hiện dành cho mạng nội bộ tin cậy vì thông tin xác thực chưa được mã hóa trên đường truyền.

Swagger POST command tạo commandId/version mới, lưu và publish retained. GET trạng thái có commandAcknowledged và lastCommandAck. Node ACK chỉ xác nhận code đã đặt GPIO, không chứng minh relay/bóng đèn hoạt động vật lý.

Telemetry nhận qua topic node do gateway hợp lệ gửi; bắt buộc sessionId và sequenceNumber, kiểm tra dữ liệu, lưu và chống trùng bền vững trước khi publish telemetryAck. Không ACK nếu lưu thất bại. Duplicate vẫn ACK để node tiến hàng đợi offline. GET lịch sử giới hạn 1000 mẫu, ledger chống trùng tồn tại riêng trong file JSON, tương thích khi đọc dữ liệu cũ chưa có ledger.

Xem [hướng dẫn đầy đủ](../README.md) cho topic, ACK, phần cứng, giới hạn lưu trữ và kiểm thử.
