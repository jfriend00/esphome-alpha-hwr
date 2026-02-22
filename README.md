# ALPHA HWR Component for ESPHome

ESPHome component for Grundfos ALPHA HWR hot water recirculation pumps. Provides bi-directional BLE control and monitoring via the GENI protocol with Home Assistant integration.

## Features

- **Telemetry**: Flow rate, head/inlet pressure, power, RPM, current, voltage, temperatures, alarms/warnings
- **Control**: Start/stop, control mode selection (Temperature, Constant Speed/Flow/Pressure, Proportional Pressure, AutoAdapt), mode-specific setpoints
- **Schedules**: Read/write weekly schedules and one-time events
- **Diagnostics**: Device info strings, operating statistics, event log, history trends
- **Automatic**: Daily RTC sync via SNTP, auto-reconnect, persistent BLE bonding

---

## Hardware Requirements

- **ESP32 Board**: ESP32, ESP32-C3, or ESP32-S3 (must support BLE)
- **Grundfos ALPHA HWR Pump**: Powered on with BLE enabled
- **Proximity**: Keep within 0.5–2 meters during initial pairing

---

## Quick Start

### 1. Discover Your Pump's MAC Address

Use a BLE scanner app (e.g., nRF Connect) or run:

```bash
esphome run components/alpha_hwr/discovery_example.yaml
```

Look for "Found ALPHA HWR Pump!" and copy the MAC address.

### 2. Configure Secrets

Create `secrets.yaml`:

```yaml
wifi_ssid: "YourWiFi"
wifi_password: "YourWiFiPassword"
ap_password: "FallbackAPPassword"
api_key: "GenerateARandom32ByteKeyHere"
ota_password: "SecureOTAPassword"
pump_mac: "3C:E0:02:XX:XX:XX"  # Your pump's MAC
```

### 3. Create Device Configuration

See `hwr-pump-example.yaml` for a complete reference configuration. Minimal setup:

```yaml
esphome:
  name: hwr-pump
  friendly_name: "HWR Pump"

substitutions:
  mac_address: "3C:E0:02:XX:XX:XX"  # Your pump's MAC

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main
  alpha_hwr_controls: github://eman/esphome-alpha-hwr/packages/alpha_hwr_controls.yaml@main

esp32:
  board: esp32-c3-devkitm-1
  variant: esp32c3
  framework:
    type: esp-idf  # Required for stable BLE

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_key

ota:
  platform: esphome
  password: !secret ota_password
```

### 4. Build and Flash

```bash
esphome run hwr-pump.yaml
```

---

## Bluetooth Pairing

### Pairing Procedure
1. Configure `enable_pairing: true` in your YAML
2. Enter Pairing Mode: Press and hold the pump's Bluetooth button until the icon flashes
3. Flash & Boot: The ESP32 will auto-detect and bond with the pump
4. Verify: Check logs for BLE authentication completion

Once paired, encryption keys are stored permanently. No need to re-pair unless you factory reset the ESP32 or pump.

### Basic vs Enhanced Mode

| Feature | Basic (No Pairing) | Enhanced (Paired) |
|---------|-------------------|-------------------|
| Flow, head, power, RPM, temp | Yes | Yes |
| AC/DC voltage, motor current | No | Yes |
| Inlet pressure | No | Yes |
| PCB/control box temperatures | No | Yes |
| Pump control (start/stop/mode) | No | Yes |
| Schedule management | No | Yes |
| Device info, event log, history | No | Yes |

---

## Schedule Card Installation

The `alpha-hwr-schedule-card` is a custom Lovelace card for interactive schedule management. It provides a more user-friendly interface than individual number/button inputs.

### Installation Steps

1. **Add the card repository to Home Assistant**:
   - Go to Settings > Devices & Services > Automations & Scenes
   - Open your Home Assistant configuration directory
   - Add this to your `configuration.yaml`:
     ```yaml
     homeassistant:
       packages:
         alpha_hwr_lovelace: !include packages/lovelace.yaml
     ```

2. **Or use HACS (recommended)**:
   - Install HACS if you haven't already: https://hacs.xyz/docs/setup/prerequisites
   - Add the custom repository: https://github.com/eman/alpha-hwr-schedule-card
   - Search for "alpha-hwr-schedule-card" in HACS
   - Click Install
   - Restart Home Assistant

3. **Add the card to a dashboard**:
   - Edit your Home Assistant dashboard
   - Click "Create New Card" or edit an existing card
   - Select "Custom: Alpha HWR Schedule Card"
   - Configure the entity IDs:
     ```yaml
     type: custom:alpha-hwr-schedule-card
     weekly_schedule: text_sensor.hwr_pump_weekly_schedule
     single_events: text_sensor.hwr_pump_single_events
     set_schedule_service: esphome.hwr_pump_set_schedule_entry
     clear_schedule_service: esphome.hwr_pump_clear_schedule_entry
     single_event_service: esphome.hwr_pump_set_single_event
     clear_single_event_service: esphome.hwr_pump_clear_single_event
     ```

Alternatively, see `docs/schedule-management.md` for programmatic schedule management via automations and the REST API.

---

## References

- **Protocol Documentation**: [https://eman.github.io/alpha-hwr/](https://eman.github.io/alpha-hwr/)
- **Python Reference Implementation**: [https://github.com/eman/alpha-hwr](https://github.com/eman/alpha-hwr)
- **ESPHome BLE Client**: [https://esphome.io/components/ble_client/](https://esphome.io/components/ble_client/)
- **Component Architecture**: [docs/architecture.md](docs/architecture.md)
