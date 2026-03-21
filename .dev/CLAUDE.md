# CitySports CS-WP9 Treadmill Controller

## What This Is

NanoH2 (ESP32-H2) firmware that bridges a CitySports CS-WP9 treadmill's
proprietary BLE protocol to Zigbee HA for Home Assistant integration.

The NanoH2 lives inside the treadmill, powered by it.

## Architecture

- **BLE GATT Client** (NimBLE) connects to treadmill's `CITYSPORTS-Linker`
  by MAC address (no scanning — coexistence constraint)
- **Zigbee End Device** exposes On/Off + Analog Output (speed) + Analog Value (state)
- **Bridge task** passes commands (Zigbee→BLE) and status (BLE→Zigbee)
  via FreeRTOS queue + event group

## Key Technical Notes

- BLE + Zigbee coexistence: BLE Connected + Zigbee ED = stable. BLE Scanning
  + Zigbee = unstable. That's why we connect by MAC, never scan.
- Treadmill protocol: header 0xA1 for commands, 0x1A for notifications,
  XOR checksum. See protocol_poc/CS-WP9_BLE_PROTOCOL.md.
- Service UUID: ffeeddcc-bbaa-9988-7766-554433221100
  Write: ...1101, Notify: ...1102
- Notifications arrive every 170–350 ms. Zigbee reports are throttled to
  only fire on actual state/speed changes.

## v1 Limitations (known, intentional)

- Actual belt speed (byte 4 in type-01 notifications) always reads 1.0 —
  not exposed. Needs more captures at higher speeds.
- Type-0x02 telemetry (time, distance, calories, steps) not decoded.
- No pause/resume distinction — just start/stop.

## Build

Follows same patterns as beacon/ and irTemp/ projects. See Justfile.
