"""ZHA Quirk for CitySports CS-WP9 Treadmill Controller.

Place this file in your Home Assistant config directory:
  config/custom_zha_quirks/citysports.py

Then add to configuration.yaml:
  zha:
    custom_quirks_path: /config/custom_zha_quirks/

Restart Home Assistant and reconfigure the device.

Entities created:
  - Switch: Treadmill On/Off (auto-discovered from On/Off cluster)
  - Number: Treadmill Speed (Analog Value, 1.0-12.0 km/h, step 0.1)
  - Sensor: Treadmill State (Analog Input, state codes: 1=starting, 2=running, 5=stopping, 6=idle)

BLE MAC is configured via the ZHA cluster UI (Basic > location_description).
"""

from zigpy.quirks.v2 import QuirkBuilder
from zigpy.zcl.clusters.general import AnalogInput, AnalogValue


(
    QuirkBuilder("ESPRESSIF", "TREADMILL")
    # Speed control (Analog Value cluster)
    .number(
        AnalogValue.AttributeDefs.present_value.name,
        AnalogValue.cluster_id,
        endpoint_id=1,
        min_value=1.0,
        max_value=7.5,
        step=0.1,
        unit="km/h",
        translation_key="treadmill_speed",
        fallback_name="Treadmill Speed",
    )
    # State code (Analog Input cluster)
    .number(
        AnalogInput.AttributeDefs.present_value.name,
        AnalogInput.cluster_id,
        endpoint_id=1,
        min_value=0,
        max_value=10,
        step=1,
        translation_key="treadmill_state",
        fallback_name="Treadmill State",
    )
    .add_to_registry()
)
