# LoRa telemetry throughput

## AUX stall detection (node `fast-v15-batch4-aux10`)

`lora_tx_blocked` now requires AUX LOW for **10 continuous seconds without UART
TX/RX progress**. The threshold is independent of the three-second retry timer.
Monitoring also works while the software TX queue is empty, because the normal
idle gate cannot enqueue telemetry when AUX is stuck LOW.

AUX HIGH or actual UART progress clears the stall immediately; the previous
30-second latch is removed. A normal four-sample transmission lasting more than
three seconds therefore does not create this warning. Real AUX LOW is still
respected by the transmitter; this change does not pretend the pin is HIGH,
erase queued samples, or replace durable Bridge ACKs.

`LINK STATS` additionally prints `RADIO HEALTH` with current blockage, stall event
count, maximum observed no-progress wait and the 10000 ms threshold. Ordinary serial
output remains electrical only. Sensor health reports the existing anomaly code
while blocked and sends an updated code list after recovery. The API may retain
an older warning until a fresh health report is received; existing historical
fault records are not deleted by a firmware upload.

Regression: `tests/RadioLink/stall.cpp` checks normal three-second transmission,
the ten-second boundary, progress, immediate recovery, empty-queue blockage and
millisecond-counter wraparound.

## Four-sample batches (v3)

Node firmware `fast-v14-batch4` continues measuring every 1000 ms and writing
each sample to SD first. It then groups up to four samples into one radio packet.
No averaging takes place. Each row retains its RTC timestamp, sequence, electrical
readings, lux, MANUAL/AUTO/full_time state, and live/backfill classification.
Only the shared device/session identity is sent once.

Gateway advertises `batchVersion: 3` on a genuine single-sample storage ACK. A new
node waits for this advertisement before batching; an old node continues sending
single records normally. Upgrade gateway and node; MQTT and Bridge/API schemas
stay unchanged.

Packet example (the values are illustrative):

```json
{"w":"b3","d":"LUXMAP-P001","s":"164698c933a6d962","b":[[100,"2026-11-26T15:00:01",1,15.125,5.101,93.75,478.219],[101,"2026-11-26T15:00:02",1,15.25,5.102,95,484.69]]}
```

Row order: `[sequence, measured_local_time, flags, lux, voltage, current_ma, power_mw]`.
Flags: bit 0 = ON; bit 1 = AUTO; bit 2 = full_time (requires AUTO);
bit 3 = backfill. The packet is bounded to 473 JSON bytes plus existing CRC framing,
so it remains within the 480-byte complete-frame path. Fewer than four rows may
be sent when a session changes, the frame is full, or a small live group has
waited four seconds. A representative four-row packet is 282 bytes.

Gateway validates the whole group and its device ACL before expanding it into
individual MQTT telemetry messages. Bridge persists them with existing idempotent
device/session/sequence identities. Only actual Bridge storage ACKs enter a bounded
eight-entry ACK queue, coalesced for 120 ms. Overflow drops a receipt, never data:
the node keeps its unconfirmed SD sample and retries.

Partial receipt example:

```json
{"w":"k3","d":"LUXMAP-P001","s":"164698c933a6d962","q":[100,102]}
```

Only 100 and 102 are confirmed; 101 and 103 remain in flight and are retried. An
incorrect device/session/sequence cannot release a sample. Confirmed copies are
reclaimed only when they reach the SD FIFO head and its NVS cursor commits.
Power loss or ACK-cache eviction may replay stored samples, never skip unconfirmed
records. The existing bounded RAM ACK cache also avoids sending confirmed live
copies again when the FIFO catches up.

Four recent live groups are prioritized before one FIFO backlog group when both
exist. The latest four samples are retained as RAM copies of SD records; older
unselected samples remain on SD. A group in flight stays stable until its members
are confirmed; new measurements continue during RF transmission and ACK waits.
Sensor health transmission also reserves an actual receive opportunity before
another data frame, avoiding a retry that talks over its ACK. Command ACKs and
gateway ON/OFF/AUTO priority remain separate from data grouping.

This preserves one-second resolution in history while delivery occurs in bursts.
It does not guarantee a new server arrival every second or an exact four-second
batch interval. Verify actual throughput and backlog clearance on the physical
radio pair; all nodes share the available RF airtime.

Host test: `tests/TelemetryWire/batch.cpp` covers four-sample round-trip, partial
ACK format, malformed/duplicate sample rejection, session boundaries and frame size.

Electrical capture remains 1000 ms, INA226 R002 (0.002 ohm), averaging 16.
No RF quiet delay is enabled by default. Normal node serial output remains electrical only.

## Radio changes

- Telemetry uses short JSON field names after capability negotiation. SD records and
  gateway MQTT publications retain their original full JSON schema, timestamps and precision.
- A new node first sends legacy telemetry. A new gateway advertises `wireVersion: 2`
  on a genuine Bridge storage ACK. Only then does the node use compact telemetry.
  The gateway returns compact ACKs only after receiving compact telemetry from that device.
  Node-first or gateway-first upgrades therefore remain compatible with the old peer.
- CRC16 still covers the complete radio payload. Mixed compact/full identities are rejected.
- Gateway command transmissions still reserve a 1500 ms reply window. Storage ACKs
  do not reserve another reply window: ACKs do not themselves require an ACK.
- A completed UART frame is released once AUX is HIGH, without an extra 300 ms
  software chunk delay. AUX LOW is never treated as HIGH.
- Data transmissions require 80 ms of radio idle time. Measurement and command polling
  continue independently. An unacknowledged live sample is retained for retry instead
  of replacing its identity with a newer sample while its ACK is in transit.
  If the ACK is missing, retransmission additionally requires a 2500 ms quiet receive
  opportunity after actual radio activity, not merely a timer from UART enqueue.
  A matching ACK releases this wait immediately; normal successful cycles do not
  incur a fixed 2500 ms pause.
- Live traffic receives four slots before an older SD record receives a slot when
  backlog exists. This does not create additional RF capacity or guarantee 1 Hz delivery.
- A bounded volatile cache remembers 64 Bridge-ACKed live identities. When one reaches
  the SD FIFO head, its durable cursor is advanced without retransmitting the same record.
  Failed cursor writes retain the record. Cache eviction or power loss causes safe replay;
  unacknowledged records are never removed by this optimization.

## Wire mapping

`w=t2` means telemetry; `w=a2` means telemetry ACK. The fields are:

| Short | Public field |
|---|---|
| d | deviceId |
| s | sessionId |
| q | sequenceNumber |
| t | timestamp |
| m | mode |
| c | lampCommand |
| l | lux |
| v | voltage |
| i | current |
| p | power |
| r | receptionType |
| u | autoType |

No API, MQTT broker credentials, LoRa air-data rate, UART baud rate or radio
address/channel is changed. A representative AUTO telemetry payload is 186 bytes
before the existing framing, compared with more than 300 bytes in full JSON.

## Validation and rollout

Host tests: `tests/TelemetryWire/main.cpp`, `tests/RadioLink/main.cpp`,
`tests/GatewayRadioLink/main.cpp`. Build both sketches before upload.

Upload the node and gateway, then read `LINK STATS` and `LIVE STATS` over 60 seconds.
`wire=2` confirms negotiation. Compare actual unique stored samples and ACKs,
not only `telemetry_tx`, which also counts retries. Check MQTT receipt times on the
Bridge, ON/OFF command ACKs, and offline SD replay followed by durable cursor advance.

The target is approximately 60 unique samples received per minute for one node.
This is not established by a 1-second measurement timer. If the round trip still
exceeds one second, read and adjust the air-data rate on both E32 modules together
in configuration mode, or use batched samples to preserve 1-second history with
less frequent arrivals. More nodes share the same RF airtime.
