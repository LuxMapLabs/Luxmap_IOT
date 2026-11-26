# Gateway ACK receive window regression

Host test uses the serial/AUX stub in ../RadioLink/Arduino.h.

```powershell
g++ -std=c++17 -I tests/RadioLink tests/GatewayRadioLink/main.cpp -o .b/gateway-radio-test.exe
.b/gateway-radio-test.exe
```

Checks a 330-byte command takes the bounded complete-frame fast path, real AUX LOW blocks completion,
and no new frame can start during the 1500 ms receive window. Incoming ACKs are still processed during
that window, and an in-progress receive frame blocks a new transmission.

The previous gateway split frames over 256 bytes into 48-byte writes and allowed an immediate retry
when the UART TX queue became empty. Scheduling retries from enqueue time could eliminate the ACK
receive opportunity for a long command. The fix does not fake AUX HIGH or change MQTT credentials,
node IDs, the wire CRC, INA226 settings, or durable telemetry ACK semantics.

Firmware boot identifier: gateway-ack-window-v1. A hardware ON/OFF test remains required to establish
whether this scheduling defect explains a particular field failure.
