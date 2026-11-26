# Telemetry wire regression

```powershell
g++ -std=c++17 -I C:/Arduino/libraries/ArduinoJson/src tests/TelemetryWire/main.cpp -o .b/telemetry-wire-test.exe
.b/telemetry-wire-test.exe
g++ -std=c++17 -I C:/Arduino/libraries/ArduinoJson/src tests/TelemetryWire/batch.cpp -o .b/telemetry-batch-test.exe
.b/telemetry-batch-test.exe
```

Checks round-trip preservation of sensor values, local RTC timestamp, session,
maximum uint32 sequence, AUTO type and backfill classification; compact ACK
identity; legacy decoding; mixed identity/version rejection; and bounded live
ACK cache eviction (an evicted identity must be replayed).

Both firmware directories contain the same `TelemetryWire.h`; compare them before
release. MQTT and SD retain full JSON. See `docs/LORA-TELEMETRY-THROUGHPUT.md`.

The batch test verifies four independent raw records survive packet round-trip,
including per-second timestamps and AUTO/full_time flags; rejects duplicate IDs,
bad values and oversized rows before publishing; and validates partial ACK lists.
