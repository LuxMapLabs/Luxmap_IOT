# ESP32 firmware

Hai sketch mới:

- [espnode/espnode.ino](espnode/espnode.ino): nạp cho ESP32 ở đèn, có cảm biến + relay + E32 + MicroSD.
- [espserver/espserver.ino](espserver/espserver.ino): nạp cho ESP32 gateway, có E32 + Wi-Fi/MQTT. Tên espserver là gateway, server C# vẫn nằm trong thư mục server ở gốc dự án.

Xem [hướng dẫn chạy và bảng đấu dây](../README.md). Firmware trực tiếp Wi-Fi cũ ở sketch_oct23a được giữ để tham khảo, không nạp cho hai board LoRa mới.
