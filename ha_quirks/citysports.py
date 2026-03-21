"""ZHA Quirk for CitySports CS-WP9 Treadmill Controller.

Place this file in your Home Assistant config directory:
  config/custom_zha_quirks/citysports.py

Then add to configuration.yaml:
  zha:
    custom_quirks_path: /config/custom_zha_quirks/

Restart Home Assistant and reconfigure the device.

Entities created:
  - Switch: Treadmill On/Off (auto-discovered from On/Off cluster)
  - Number: Treadmill Speed (Analog Output, 1.0–12.0 km/h, step 0.1)
  - Sensor: Treadmill State (Analog Value, state codes: 1=starting, 2=running, 5=stopping, 6=idle)
  - Button: Treadmill BLE MAC (Basic.location_description, write-once config)
"""

from zigpy.quirks.v2 import QuirkBuilder
from zigpy.zcl.clusters.general import AnalogOutput, AnalogValue, Basic


(
    QuirkBuilder("ESPRESSIF", "TREADMILL")
    # Speed control (Analog Output cluster)
    .number(
        AnalogOutput.AttributeDefs.present_value.name,
        AnalogOutput.cluster_id,
        min_value=1.0,
        max_value=12.0,
        step=0.1,
        unit="km/h",
        translation_key="treadmill_speed",
        fallback_name="Treadmill Speed",
    )
    # State code (Analog Value cluster) — read-only sensor
    .number(
        AnalogValue.AttributeDefs.present_value.name,
        AnalogValue.cluster_id,
        min_value=0,
        max_value=10,
        step=1,
        translation_key="treadmill_state",
        fallback_name="Treadmill State",
    )
    # BLE MAC address configuration (Basic.location_description)
    .write_attr_button(
        Basic.AttributeDefs.location_description.name,
        Basic.cluster_id,
        translation_key="treadmill_ble_mac",
        fallback_name="Treadmill BLE MAC",
    )
    .add_to_registry()
)
