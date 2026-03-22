"""ZHA Quirk for CitySports CS-WP9 Treadmill Controller.

Place this file in your Home Assistant config directory:
  config/custom_zha_quirks/citysports.py

Then add to configuration.yaml:
  zha:
    custom_quirks_path: /config/custom_zha_quirks/

Restart Home Assistant and reconfigure the device.

Entities created:
  - Switch: Treadmill On/Off (auto-discovered from On/Off cluster)
  - Number: Treadmill Speed (EP1, Analog Value, 1.0-7.5 km/h, step 0.1)
  - Number: Default Start Speed (EP2, Analog Value, 1.0-7.5 km/h, step 0.1)
  - Sensor: Treadmill State (EP1, Analog Input, auto-discovered)

Speed values stored as x10 in ZCL (multiplier=0.1 converts to km/h).
BLE MAC is configured via the ZHA cluster UI (Basic > location_description).
"""

from zigpy.quirks.v2 import QuirkBuilder
from zigpy.zcl.clusters.general import AnalogValue


(
    QuirkBuilder("ESPRESSIF", "TREADMILL")
    # Speed control (EP1 Analog Value, stored as x10)
    .number(
        AnalogValue.AttributeDefs.present_value.name,
        AnalogValue.cluster_id,
        endpoint_id=1,
        min_value=1.0,
        max_value=7.5,
        step=0.1,
        unit="km/h",
        multiplier=0.1,
        translation_key="treadmill_speed",
        fallback_name="Treadmill Speed",
    )
    # Default start speed (EP2 Analog Value, stored as x10)
    .number(
        AnalogValue.AttributeDefs.present_value.name,
        AnalogValue.cluster_id,
        endpoint_id=2,
        min_value=1.0,
        max_value=7.5,
        step=0.1,
        unit="km/h",
        multiplier=0.1,
        translation_key="treadmill_start_speed",
        fallback_name="Default Start Speed",
    )
    .add_to_registry()
)
