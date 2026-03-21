# CitySports CS-WP9 BLE Protocol Reference

Reverse-engineered from BT HCI snoop log captured 2026-03-21.

## BLE Identifiers

| Field | Value |
|-------|-------|
| Device name | `CITYSPORTS-Linker` |
| MAC address | `XX:XX:XX:XX:XX:XX` (find yours with nRF Connect) |
| Service UUID | `ffeeddcc-bbaa-9988-7766-554433221100` |
| Write characteristic | `ffeeddcc-bbaa-9988-7766-554433221101` (handle `0x0013`) |
| Notify characteristic | `ffeeddcc-bbaa-9988-7766-554433221102` (handle `0x0010`) |

The device also advertises as `CITYSPORTS-Audio` for A2DP speaker pairing (separate BLE identity, activated by long-pressing the "-" button on the handlebar).

## Packet Format

All packets (commands and notifications) share the same envelope:

```
[header] [payload bytes...] [XOR checksum]
```

- **Commands** (app → treadmill): header = `0xA1`, written to characteristic `...1101`
- **Notifications** (treadmill → app): header = `0x1A`, received on characteristic `...1102`

The **checksum** is the XOR of all preceding bytes (header + payload). Example:

```
a1 05 00 a4
         ^^ = a1 ^ 05 ^ 00 = a4
```

## Commands (Write to `...1101`)

### Status Query
```
a1 05 00 a4
```
Triggers the treadmill to respond with current status notifications.

### Start
```
a1 03 01 01 a2
```
Initiates the 3-second countdown, then starts the belt at 1.0 km/h.

### Stop / Pause
```
a1 03 01 05 a6
```
Gradual deceleration to standstill. The treadmill transitions through state `0x05` (stopping) then `0x06` (idle).

### Set Speed
```
a1 01 02 01 SS [xor]
```
Where `SS` = target speed × 10 (in 0.1 km/h resolution).

| Speed | SS (hex) | Full command |
|-------|----------|--------------|
| 1.0 km/h | `0x0A` | `a1 01 02 01 0a a9` |
| 2.0 km/h | `0x14` | `a1 01 02 01 14 b7` |
| 3.0 km/h | `0x1E` | `a1 01 02 01 1e bd` |
| 5.0 km/h | `0x32` | `a1 01 02 01 32 91` |
| 8.0 km/h | `0x50` | `a1 01 02 01 50 f3` |
| 10.0 km/h | `0x64` | `a1 01 02 01 64 c7` |
| 12.0 km/h | `0x78` | `a1 01 02 01 78 db` |

Speed range depends on mode:
- **Walking mode** (handlebar down): 1.0 – 6.0 km/h
- **Running mode** (handlebar up): 1.0 – 12.0 km/h

## Notifications (Received on `...1102`)

Notifications arrive approximately every 170–350ms. Three message types observed:

### Type 0x01 — Status (13 bytes)

```
1a 01 09 78 AA 00 00 TT 00 SS 00 00 [xor]
```

| Offset | Field | Description |
|--------|-------|-------------|
| 0 | `0x1A` | Header |
| 1 | `0x01` | Message type |
| 2 | `0x09` | Payload length |
| 3 | `0x78` | Constant (model identifier?) |
| 4 | `AA` | Actual/current speed × 10 |
| 5–6 | `00 00` | Unknown (always zero in capture) |
| 7 | `TT` | Target speed × 10 |
| 8 | `00` | Unknown |
| 9 | `SS` | State code |
| 10–11 | `00 00` | Unknown |
| 12 | XOR | Checksum |

### State Codes

| Code | State | Description |
|------|-------|-------------|
| `0x01` | Starting | 3-second countdown active |
| `0x02` | Running | Belt moving at target speed |
| `0x05` | Stopping | Belt decelerating to halt |
| `0x06` | Idle | Stopped, ready for start command |

### Type 0x02 — Telemetry (16 bytes)

```
1a 02 0c 02 TT EE DD DD CC CC SS SS 00 00 00 [xor]
```

Incremental workout metrics (time, distance, calories, steps). Exact field mapping not fully decoded — values increment monotonically during a session.

### Type 0x05 — Initial Status (16 bytes)

Sent once on connection. Contains device info and session state. Observed only at connection establishment.

## Connection Sequence

1. Scan for `CITYSPORTS-Linker`
2. Connect
3. Discover services, locate `...1100`
4. Write `0x0100` to CCCD descriptor (`0x2902`) on characteristic `...1102` to enable notifications
5. Write status query `a1 05 00 a4` to `...1101`
6. Begin receiving notifications on `...1102`
7. Send start/stop/speed commands as needed

## Notes

- The treadmill only accepts **one BLE connection** at a time. If Home Assistant or an ESP32 is connected, the phone app will not be able to connect (and vice versa).
- The app sends speed changes as individual 0.1 km/h increments (matching each press of the +/- buttons). It is unknown whether larger jumps (e.g. going directly from 2.0 to 8.0) are accepted — likely yes, but untested.
- The `0x78` constant at offset 3 in type-01 notifications may be a model identifier (0x78 = 120 decimal).
- Byte 4 in the status notification (`AA` field) was consistently `0x0A` (1.0 km/h) throughout the capture — this may represent the actual belt speed lagging behind the target, or it may have a different meaning. Further testing with higher speeds needed.
