# CitySports CS-WP9 Treadmill — Zigbee + BLE Controller

ESP32-H2 (NanoH2) that lives inside the treadmill, connects to its BLE
control interface, and exposes it to Home Assistant as a Zigbee device.

## How It Works

The treadmill uses a proprietary BLE protocol (not FTMS). The NanoH2 acts
as a BLE GATT client that connects to the treadmill's `CITYSPORTS-Linker`
BLE identity and translates commands to/from Zigbee HA clusters.

```
Home Assistant (ZHA)
    ↕ Zigbee
NanoH2 (this firmware)
    ↕ BLE GATT
Treadmill MCU
```

The NanoH2 is powered by the treadmill (mains power), so it boots when the
treadmill is plugged in and waits a few seconds before attempting BLE
connection.

## v1 Scope

- **On/Off** cluster → start / stop
- **Analog Output** → target speed (1.0 – 12.0 km/h)
- **Analog Value** → state code (1=starting, 2=running, 5=stopping, 6=idle)
- BLE MAC configurable via Zigbee (Basic.location_description)
- Auto-reconnect with exponential backoff

Not in v1 (needs more reverse engineering):
- Actual belt speed (byte 4 always reads 1.0 — needs captures at higher speeds)
- Telemetry (time, distance, calories, steps — type-0x02 notifications not decoded)
- Pause/resume (may be just stop + start, needs testing)

## Project Structure

```
.
├── README.md
├── Justfile                        # Build / flash / monitor recipes
├── firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   └── main/
│       ├── CMakeLists.txt
│       ├── idf_component.yml       # esp-zigbee-lib ~1.6.0
│       └── main.c                  # All firmware code
├── ha_quirks/
│   └── citysports.py               # ZHA quirk for friendly HA entities
├── protocol_poc/                   # Protocol docs + Python/ESPHome PoCs
│   ├── CS-WP9_BLE_PROTOCOL.md     # Full protocol reference
│   ├── README.md
│   ├── citysports_control.py       # Python CLI controller (bleak)
│   └── esphome_treadmill.yaml      # ESPHome alternative (separate ESP32)
└── .dev/
    └── zigbee_and_ble_reference.md  # NimBLE + coexistence research notes
```

## Setup

### Prerequisites

- ESP-IDF v5.3.3 (`just setup-idf` if not installed)
- M5Stack NanoH2 (or any ESP32-H2 board)

### Build & Flash

```bash
just set-target      # once, after first clone
just build           # compile
just flash           # build + flash via Windows COM port
just monitor         # serial output
just run             # flash + monitor
```

Update the COM port in the Justfile (`port := "COM9"`).

### Configure BLE MAC

After the device joins your Zigbee network, set the treadmill's BLE MAC
address via the ZHA quirk button (or directly write to Basic cluster's
`location_description` attribute):

```
70:19:88:XX:XX:XX
```

Find your treadmill's MAC with nRF Connect or similar BLE scanner — look
for `CITYSPORTS-Linker`.

The MAC is saved to NVS and persists across reboots.

### Home Assistant

Install the ZHA quirk (`ha_quirks/citysports.py`) to get friendly entities:

- **Switch**: Treadmill On/Off
- **Number**: Treadmill Speed (slider, 1.0–12.0 km/h, 0.1 step)
- **Sensor**: Treadmill State (1/2/5/6)
- **Button**: Set BLE MAC (one-time config)

## Protocol Reference

See [protocol_poc/CS-WP9_BLE_PROTOCOL.md](protocol_poc/CS-WP9_BLE_PROTOCOL.md)
for the full reverse-engineered protocol.

## BLE + Zigbee Coexistence

The ESP32-H2 has a single 2.4 GHz radio shared between BLE and 802.15.4
(Zigbee) via time-division multiplexing. Key design decisions:

- **No BLE scanning** — connects directly by MAC address (BLE scanning +
  Zigbee = unstable per Espressif coexistence docs)
- **BLE Connected + Zigbee ED = stable** — the supported operating mode
- **Connection params** (50–100 ms interval) balance BLE responsiveness
  with Zigbee airtime
- `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` enables automatic coexistence
