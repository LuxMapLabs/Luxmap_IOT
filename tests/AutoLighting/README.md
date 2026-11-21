# AUTO type firmware behavior

Node and gateway accept `autoType: scheduled | full_time` on the MQTT/LoRa command.
Missing AUTO type remains scheduled. MANUAL has no autoType.

- full_time: BH1750 all day; no RTC dependency for the control decision itself.
- scheduled: use BH1750 in the half-open daytime window, otherwise force ON.
- Both sensor paths require 5 consecutive valid samples (<50 lux ON, >50 lux OFF).
  Equality, invalid samples, samples matching current output and mode changes reset the streak.
- New expiring commands still need a valid RTC to check the existing TTL.
- Node persists type and schedule in NVS and includes type in AUTO telemetry/ACK.
- Gateway preserves type and rejects an ACK whose type does not match the queued command.
- AUTO selection uses the relay, sensor sampling and LoRa transport configuration of the firmware it is built with.

Host policy check:

```powershell
g++ -std=c++17 tests/AutoLighting/main.cpp -o .b/auto-lighting-test.exe
.b/auto-lighting-test.exe
```

Build both sketches before flashing. Hardware ACK and relay behavior still need testing after upload.
