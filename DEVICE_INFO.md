# Device Information in Home Assistant

This document explains how device information from the ALPHA HWR pump appears in Home Assistant.

## What's Included

The ESPHome component automatically provides the following information to Home Assistant:

### Static Device Info (from `project` config)
- **Manufacturer**: Grundfos
- **Model**: ALPHA HWR
- **Firmware Version**: 1.0.0 (ESPHome component version)

### Dynamic Device Info (from pump)
The following text sensors are created and updated when the ESP connects to the pump:

- **Product Name**: Product model string from pump (e.g., "ALPHA HWR")
- **Serial Number**: Full pump serial number (e.g., "10000479")
- **Software Version**: Pump firmware version (e.g., "92601618V04.02.01.02539")
- **Hardware Version**: Pump hardware revision (e.g., "92601617V01.03.00.00469")
- **BLE Version**: Bluetooth firmware version (e.g., "92811431V06.00.01.00001")

## Configuration

### Basic Setup

In your ESPHome YAML config, ensure you have the `project` section configured:

```yaml
esphome:
  name: hwr-pump
  friendly_name: "HWR Pump"
  project:
    name: "grundfos.alpha_hwr"
    version: "1.0.0"
```

### Adding Device Info Sensors

Use the package configuration to include device info sensors:

```yaml
packages:
  alpha_hwr: !include packages/alpha_hwr_pairing.yaml
```

The package automatically configures all 5 device info text sensors.

## How It Appears in Home Assistant

### Device Info Card

When you click on the device in Home Assistant, you'll see:

```
┌─────────────────────────────────────┐
│ HWR Pump                            │
├─────────────────────────────────────┤
│ Manufacturer: Grundfos              │
│ Model: Alpha HWR                    │
│ Firmware: 1.0.0                     │
│                                     │
│ Integration: ESPHome                │
│ Device ID: xxxxx                    │
└─────────────────────────────────────┘
```

### Device Info Entities

Below the device info card, you'll find these text sensors:

- **Product Name**: ALPHA HWR
- **Serial Number**: 10000479
- **Software Version**: 92601618V04.02.01.02539
- **Hardware Version**: 92601617V01.03.00.00469
- **BLE Version**: 92811431V06.00.01.00001

## Advanced: Adding Serial Number to Device Info Card

If you want the serial number to appear in the main Device Info card (the grey box at the top), you have two options:

### Option 1: Device Registry API (Recommended)

Create a Home Assistant automation that updates the device registry when the serial number sensor changes:

```yaml
# In Home Assistant's configuration.yaml or automations.yaml
automation:
  - alias: "Update HWR Pump Serial Number"
    trigger:
      - platform: state
        entity_id: text_sensor.serial_number
    condition:
      - condition: template
        value_template: "{{ trigger.to_state.state not in ['unknown', 'unavailable'] }}"
    action:
      - service: python_script.update_device_info
        data:
          device_id: "{{ device_id('text_sensor.serial_number') }}"
          serial_number: "{{ states('text_sensor.serial_number') }}"
```

You'll need to create a Python script at `<config>/python_scripts/update_device_info.py`:

```python
device_id = data.get('device_id')
serial_number = data.get('serial_number')

if device_id and serial_number:
    device_registry = hass.helpers.device_registry.async_get(hass)
    device_entry = device_registry.async_get(device_id)
    
    if device_entry:
        device_registry.async_update_device(
            device_id,
            serial_number=serial_number
        )
```

### Option 2: Use Friendly Names

A simpler approach is to include the serial number in the device's friendly name:

```yaml
esphome:
  name: hwr-pump
  friendly_name: "HWR Pump (SN: ${serial_number})"
```

However, this requires knowing the serial number in advance.

## Verification

To verify device info sensors are working:

1. Check ESP logs after device boots:
   ```
   [INFO] Querying device information...
   [INFO] Received Class 7 string ID 10: 'ALPHA HWR'
   [INFO] Received Class 7 string ID 9: '10000479'
   ```

2. In Home Assistant, go to **Settings → Devices & Services → ESPHome**

3. Click on your HWR Pump device

4. Verify all 5 text sensors appear with values

## Troubleshooting

### Sensors show "Unknown" or "Unavailable"

- **Check ESP logs**: Look for "Querying device information..." message
- **Verify BLE connection**: Ensure pump is authenticated (check "Pairing Status" sensor)
- **Power cycle**: Restart the ESP32 to trigger a fresh device info query

### Sensors not appearing

- **Check package inclusion**: Ensure `alpha_hwr_pairing.yaml` is included
- **Verify config**: Run `esphome config hwr-pump.yaml` to check for errors
- **Rebuild firmware**: Run `esphome compile hwr-pump.yaml` and upload

### Serial number has extra "1" prefix

This was fixed in version 1.0.0. If you're seeing "110000479" instead of "10000479", update to the latest component version.

## References

- [ESPHome Core Configuration](https://esphome.io/components/esphome.html)
- [ESPHome Project Information](https://esphome.io/components/esphome.html#project-information)
- [Home Assistant Device Registry](https://developers.home-assistant.io/docs/device_registry_index/)
