# Server C# cho gateway LoRa–MQTT

Chạy `dotnet run --project server/LuxMap.Api.csproj` từ gốc dự án. Swagger tại http://localhost:5000/swagger, MQTT broker tích hợp cổng 1883.

`Mqtt:Gateways` trong appsettings.json ánh xạ gateway MQTT client ID sang danh sách node. Mặc định LUXMAP-GW001 → LUXMAP-P001. Firmware gateway hiện hỗ trợ một NODE_ID.

Swagger POST command tạo commandId/version mới, lưu và publish retained. GET trạng thái có commandAcknowledged và lastCommandAck. Node ACK chỉ xác nhận code đã đặt GPIO, không chứng minh relay/bóng đèn hoạt động vật lý.

Telemetry nhận qua topic node do gateway hợp lệ gửi; bắt buộc sessionId và sequenceNumber, kiểm tra dữ liệu, lưu và chống trùng bền vững trước khi publish telemetryAck. Không ACK nếu lưu thất bại. Duplicate vẫn ACK để node tiến hàng đợi offline. GET lịch sử giới hạn 1000 mẫu, ledger chống trùng tồn tại riêng trong file JSON, tương thích khi đọc dữ liệu cũ chưa có ledger.

Xem [hướng dẫn đầy đủ](../README.md) cho topic, ACK, phần cứng, giới hạn lưu trữ và kiểm thử.
