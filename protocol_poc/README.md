# CitySports CS-WP9 Treadmill BLE Controller

Control your CitySports CS-WP9 treadmill from your PC or Home Assistant — no terrible GEARSTONE/EQiSports app required.

## Background

The CS-WP9 uses a proprietary BLE protocol over a custom GATT service (not standard FTMS). The protocol was reverse-engineered by capturing BT HCI snoop logs from the GEARSTONE Android app and analyzing the ATT layer traffic in Wireshark.

The treadmill exposes two BLE identities:

- **`CITYSPORTS-Audio`** — A2DP speaker (long-press `-` on handlebar to activate). Not relevant here.
- **`CITYSPORTS-Linker`** — control channel. This is what we talk to.

Only **one BLE connection at a time** is supported. When your PC or ESP32 is connected, the phone app won't be able to connect (and vice versa).

## Project Structure

```
.
├── README.md                   # this file
├── CS-WP9_BLE_PROTOCOL.md     # full protocol reference
├── citysports_control.py       # Python CLI controller (bleak)
├── esphome_treadmill.yaml      # ESPHome config for HA integration
└── bt-snoop-data/              # (optional) raw capture data
    ├── btsnoop_hci.log
    ├── att_io.csv
    ├── att_trace.csv
    └── gatt_profile.txt
```

## Quick Start — Python CLI

### Requirements

- Python 3.10+
- Bluetooth adapter on your PC (built-in or USB dongle)
- Windows / Linux / macOS

### Setup

```bash
cp .env.example .env
# Edit .env with your treadmill's MAC address (find it with nRF Connect)
uv run citysports_control.py
```

### Usage

```
> start          # 3-second countdown, then runs at 1.0 km/h
> speed 3.5      # set to 3.5 km/h
> speed 6        # set to 6.0 km/h
> status         # print current state + speed
> stop           # gradual deceleration to standstill
> quit           # stop + disconnect
```

### Configuration

Set your treadmill's BLE MAC address in `.env` (find it with nRF Connect or similar BLE scanner):

```
TREADMILL_MAC=70:19:88:XX:XX:XX
```

## Quick Start — Home Assistant (ESPHome)

### Requirements

- ESP32 dev board (any variant — DevKit, S3, C3)
- ESPHome addon in Home Assistant
- Treadmill within BLE range of the ESP32 (~5–10m)

### Setup

1. Copy `esphome_treadmill.yaml` into your ESPHome config directory
2. Update your `secrets.yaml` with `wifi_ssid`, `wifi_password`, `api_key`, `ota_password`, and `treadmill_mac`
4. Flash to your ESP32
5. The device auto-discovers in HA with these entities:
   - **Button**: Treadmill Start / Treadmill Stop
   - **Number**: Treadmill Speed (slider, 1.0–12.0 km/h)
   - **Sensor**: Treadmill Target Speed / Actual Speed
   - **Text Sensor**: Treadmill State (idle / starting / running / stopping)

### Dashboard Card (example)

```yaml
type: entities
title: Treadmill
entities:
  - entity: text_sensor.treadmill_state
  - entity: sensor.treadmill_target_speed
  - entity: sensor.treadmill_actual_speed
  - entity: number.treadmill_speed
  - entity: button.treadmill_start
  - entity: button.treadmill_stop
```

## Protocol Overview

Full details in [CS-WP9_BLE_PROTOCOL.md](CS-WP9_BLE_PROTOCOL.md).

### BLE Identifiers

| Field | Value |
|---|---|
| Service UUID | `ffeeddcc-bbaa-9988-7766-554433221100` |
| Write characteristic | `ffeeddcc-bbaa-9988-7766-554433221101` |
| Notify characteristic | `ffeeddcc-bbaa-9988-7766-554433221102` |

### Packet Format

```
[header] [payload...] [XOR checksum of all preceding bytes]
```

Commands use header `0xA1`, notifications use header `0x1A`.

### Command Reference

| Command | Bytes | Notes |
|---|---|---|
| Status query | `a1 05 00 a4` | Triggers status notification |
| Start | `a1 03 01 01 a2` | 3s countdown, then 1.0 km/h |
| Stop | `a1 03 01 05 a6` | Gradual decel to standstill |
| Set speed | `a1 01 02 01 SS XX` | SS = speed×10, XX = XOR checksum |

### Checksum Calculation

```python
def xor_checksum(data: bytes) -> int:
    result = 0
    for b in data:
        result ^= b
    return result

# Example: set speed 3.0 km/h (0x1E)
payload = bytes([0xA1, 0x01, 0x02, 0x01, 0x1E])
checksum = xor_checksum(payload)  # 0xBD
# Full command: a1 01 02 01 1e bd
```

### State Machine

```
     cmd_start()         belt reaches target
  IDLE (0x06) ──────> STARTING (0x01) ──────> RUNNING (0x02)
    ^                                            │
    │              cmd_stop()                     │
    └──── STOPPING (0x05) <───────────────────────┘
```

### Status Notification (type 0x01)

```
1a 01 09 78 AA 00 00 TT 00 SS 00 00 [xor]
                 │           │     │
                 │           │     └── state (01/02/05/06)
                 │           └──────── target speed × 10
                 └──────────────────── actual speed × 10 (see known issues)
```

## Known Issues / TODO

### Actual speed field (byte 4) reads incorrectly

In the initial capture, byte 4 in type-0x01 notifications was stuck at `0x0A` (1.0 km/h) regardless of the actual belt speed. Possible explanations:

- The capture was done at low speeds in walking mode, and the field might only update at higher speeds
- Byte 4 might not be "actual speed" but rather "minimum speed" or "mode base speed"
- The field might need a different trigger to start reporting (e.g. a specific command we haven't sent)

**To investigate**: do another BT snoop capture at higher speeds in running mode (handlebar up, speeds >6 km/h). Compare byte 4 values. Also try capturing with the treadmill accelerating/decelerating to see if the value changes dynamically.

### Type 0x02 telemetry notifications not fully decoded

The 16-byte type-0x02 messages contain incremental workout metrics (time, distance, calories, steps), but exact field offsets aren't mapped yet. They arrive interleaved with type-0x01 at roughly the same rate.

**To investigate**: do a longer session, log all type-0x02 messages, correlate byte positions with the display values shown on the treadmill console.

### Untested: large speed jumps

The GEARSTONE app sends speed changes as individual 0.1 km/h increments. It's unknown whether the treadmill accepts large jumps (e.g. going from 2.0 straight to 10.0 km/h). Probably fine, but worth confirming.

### Untested: pause vs. stop distinction

The capture shows `a1 03 01 05 a6` used for both pause and stop scenarios. There might be a separate pause command, or the treadmill might remember the previous speed and resume at that speed when started again after a "pause-stop". The EQiSports app is known to lack proper pause/resume — the hughesjs/FitnessMachine Flutter app added it, so there may be an undiscovered resume command.

**To investigate**: after sending stop, try sending start — does it resume at the previous speed or always restart at 1.0 km/h? If always 1.0, then "pause" would need to be implemented client-side (remember speed, stop, then start + set speed on resume).

### ESPHome `ble_write` with dynamic values

The ESPHome speed slider uses a `!lambda` to compute the command bytes including the XOR checksum. This should work with recent ESPHome versions (2024.2+), but if it doesn't compile, the fallback is to use a `script` with hard-coded speed steps or a custom component.

## Capture Your Own BT Snoop Data

If you need to capture additional protocol interactions:

1. **Android**: Settings → Developer Options → Enable "Bluetooth HCI snoop log"
2. Connect with the GEARSTONE app, exercise the features you want to decode
3. Disable the snoop log
4. Pull with: `adb pull /data/misc/bluetooth/logs/btsnoop_hci.log` (path varies by Android version)
5. Open in Wireshark — filter: `btatt.handle == 0x0013` for writes, `btatt.handle == 0x0010` for notifications
6. Export ATT packets as CSV for easier analysis

## Related Projects

- [hughesjs/FitnessMachine](https://github.com/hughesjs/FitnessMachine) — Flutter app replacing EQiSports, specifically for CitySports treadmills. Dart-based, reverse-engineers the same protocol family.
- [dudanov/hassio-ftms](https://github.com/dudanov/hassio-ftms) — HA custom integration for FTMS treadmills (won't work for CS-WP9 since it's not FTMS, but useful reference).
- [samsonovss/ESPHome-Treadmill-FTMS](https://github.com/samsonovss/ESPHome-Treadmill-FTMS) — ESPHome treadmill controller via UART (different approach, but good ESPHome patterns).
- [Decathlon FTMS web console](https://github.com/Decathlon/domyos-developers/tree/main/ftms-treadmill-web-console) — Web Bluetooth FTMS example. Doesn't work for CS-WP9 but good reference for BLE treadmill control patterns.

## License

Do whatever you want with this. The protocol is what it is.
