# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "bleak",
#     "python-dotenv",
# ]
# ///
"""
CitySports CS-WP9 Treadmill BLE Controller
===========================================
Controls the treadmill over Bluetooth Low Energy from a PC.

Usage:
    uv run citysports_control.py

Protocol reverse-engineered from BT HCI snoop log analysis.
"""

import asyncio
import os
import struct
from enum import IntEnum

from dotenv import load_dotenv
from bleak import BleakClient, BleakScanner

load_dotenv()

# --- BLE identifiers ---
TREADMILL_MAC = os.environ["TREADMILL_MAC"]
SERVICE_UUID = "ffeeddcc-bbaa-9988-7766-554433221100"
WRITE_UUID   = "ffeeddcc-bbaa-9988-7766-554433221101"
NOTIFY_UUID  = "ffeeddcc-bbaa-9988-7766-554433221102"


class TreadmillState(IntEnum):
    IDLE     = 0x06
    STARTING = 0x01
    RUNNING  = 0x02
    STOPPING = 0x05


# --- Protocol helpers ---

def _xor_checksum(data: bytes) -> int:
    xor = 0
    for b in data:
        xor ^= b
    return xor


def _build_command(*payload_bytes: int) -> bytes:
    """Build a command packet: [0xA1] [payload...] [XOR checksum]."""
    data = bytes([0xA1, *payload_bytes])
    return data + bytes([_xor_checksum(data)])


def cmd_status_query() -> bytes:
    """Query current treadmill status."""
    return _build_command(0x05, 0x00)


def cmd_start() -> bytes:
    """Start the treadmill (3-second countdown, then runs at 1.0 km/h)."""
    return _build_command(0x03, 0x01, 0x01)


def cmd_stop() -> bytes:
    """Stop/pause the treadmill (gradual deceleration)."""
    return _build_command(0x03, 0x01, 0x05)


def cmd_set_speed(kmh: float) -> bytes:
    """
    Set target speed.

    Args:
        kmh: Speed in km/h (0.1 increments, range 1.0 - 12.0).
             Values are clamped to the valid range.
    """
    raw = max(10, min(120, round(kmh * 10)))
    return _build_command(0x01, 0x02, 0x01, raw)


# --- Notification parser ---

def parse_notification(data: bytearray) -> dict | None:
    """Parse a type-01 status notification."""
    if len(data) < 12 or data[0] != 0x1A:
        return None

    msg_type = data[1]

    if msg_type == 0x01 and len(data) >= 12:
        return {
            "type": "status",
            "actual_speed_kmh": data[4] / 10.0,
            "target_speed_kmh": data[7] / 10.0,
            "state": TreadmillState(data[9]) if data[9] in TreadmillState._value2member_map_ else data[9],
        }

    if msg_type == 0x02 and len(data) >= 15:
        return {
            "type": "telemetry",
            "raw": data.hex(),
        }

    return {"type": f"unknown_{msg_type:#x}", "raw": data.hex()}


# --- Main controller ---

class TreadmillController:
    def __init__(self):
        self.client: BleakClient | None = None
        self.last_status: dict | None = None
        self._status_event = asyncio.Event()

    def _on_notify(self, _characteristic, data: bytearray):
        parsed = parse_notification(data)
        if parsed and parsed.get("type") == "status":
            self.last_status = parsed
            self._status_event.set()

    async def connect(self, address: str = TREADMILL_MAC):
        print(f"Scanning for {address}...")
        device = await BleakScanner.find_device_by_address(address, timeout=10)
        if not device:
            raise RuntimeError(f"Treadmill not found at {address}")

        self.client = BleakClient(device)
        await self.client.connect()
        print(f"Connected to {device.name} ({device.address})")

        # Enable notifications
        await self.client.start_notify(NOTIFY_UUID, self._on_notify)

        # Initial status query
        await self.client.write_gatt_char(WRITE_UUID, cmd_status_query())
        await asyncio.sleep(0.5)

        if self.last_status:
            print(f"  State: {self.last_status['state']}")
            print(f"  Speed: {self.last_status['target_speed_kmh']} km/h")

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()
            print("Disconnected.")

    async def _write(self, data: bytes):
        if not self.client or not self.client.is_connected:
            raise RuntimeError("Not connected")
        await self.client.write_gatt_char(WRITE_UUID, data)

    async def start(self):
        print("Starting treadmill...")
        await self._write(cmd_start())

    async def stop(self):
        print("Stopping treadmill...")
        await self._write(cmd_stop())

    async def set_speed(self, kmh: float):
        print(f"Setting speed to {kmh:.1f} km/h...")
        await self._write(cmd_set_speed(kmh))

    async def query_status(self) -> dict | None:
        self._status_event.clear()
        await self._write(cmd_status_query())
        try:
            await asyncio.wait_for(self._status_event.wait(), timeout=2.0)
        except asyncio.TimeoutError:
            pass
        return self.last_status


# --- Interactive CLI ---

async def interactive():
    ctrl = TreadmillController()

    try:
        await ctrl.connect()
    except Exception as e:
        print(f"Connection failed: {e}")
        return

    print("\nCommands: start | stop | speed <km/h> | status | quit")
    print("Example: speed 3.5\n")

    try:
        while True:
            try:
                line = await asyncio.get_event_loop().run_in_executor(None, input, "> ")
            except EOFError:
                break

            parts = line.strip().lower().split()
            if not parts:
                continue

            cmd = parts[0]

            if cmd == "quit" or cmd == "q":
                await ctrl.stop()
                break
            elif cmd == "start":
                await ctrl.start()
            elif cmd == "stop":
                await ctrl.stop()
            elif cmd == "speed" and len(parts) > 1:
                try:
                    kmh = float(parts[1])
                    await ctrl.set_speed(kmh)
                except ValueError:
                    print("Usage: speed <number>")
            elif cmd == "status":
                status = await ctrl.query_status()
                if status:
                    print(f"  State: {status['state']}")
                    print(f"  Target: {status['target_speed_kmh']} km/h")
                    print(f"  Actual: {status['actual_speed_kmh']} km/h")
                else:
                    print("  No response")
            else:
                print("Unknown command. Try: start | stop | speed <km/h> | status | quit")

    finally:
        await ctrl.disconnect()


if __name__ == "__main__":
    asyncio.run(interactive())
