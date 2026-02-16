# ALPHA HWR Component for ESPHome

ESPHome component for Grundfos ALPHA HWR hot water recirculation pumps. Provides bi-directional BLE control and monitoring via the GENI protocol with Home Assistant integration.

## Features

### Real-time Monitoring
- Flow rate (m³/h), head pressure (m), inlet pressure (bar)
- Power consumption (W), motor RPM, motor current (A)
- Grid voltage (V), DC voltage (V)
- Water temperature, PCB temperature, control box temperature
- Active alarms and warnings with human-readable descriptions

### Pump Control
- Start/stop pump (switch)
- Control mode selection: Temperature Control, Constant Speed, Constant Flow, Constant Pressure, Proportional Pressure, AutoAdapt
- Mode-specific setpoints (temperature range, RPM, flow, pressure)
- Cycle time adjustments
- AutoAdapt toggle

### Schedule Management
- Read/write weekly pump schedules (7 days, configurable start/end times)
- Single event (one-time) schedule support with date/time
- Schedule enable/disable switch
- Schedule editor with day, layer, start/end time inputs

### Device Information & Diagnostics
- Serial number, product name, software/hardware/BLE versions
- Operating statistics: total start count, lifetime operating hours
- Event log: last 20 pump start/stop events with timestamps
- History trends: flow, head, temperature, power-on hours (10-point series)
- Cycle timestamps: last 10 pump cycle start times

### Automatic Features
- Daily pump RTC synchronization via SNTP
- Auto-reconnection on BLE disconnect
- Persistent BLE bonding

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

esp32:
  board: esp32-c3-devkitm-1
  variant: esp32c3
  framework:
    type: esp-idf  # Required for stable BLE

external_components:
  - source:
      type: local
      path: .
    components: [alpha_hwr]

packages:
  alpha_hwr_pairing: !include packages/alpha_hwr_pairing.yaml
  alpha_hwr_controls: !include packages/alpha_hwr_controls.yaml

esp32_ble_tracker:
  scan_parameters:
    interval: 1.1s
    window: 1.1s

ble_client:
  - mac_address: !secret pump_mac
    id: hwr_pump_client

alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true

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
| Grid/DC voltage, motor current | No | Yes |
| Inlet pressure | No | Yes |
| PCB/control box temperatures | No | Yes |
| Pump control (start/stop/mode) | No | Yes |
| Schedule management | No | Yes |
| Device info, event log, history | No | Yes |

---

## Home Assistant Entities

### Sensors (Telemetry)

| Entity | Type | Unit | Description |
|--------|------|------|-------------|
| Flow Rate | sensor | m³/h | Water flow through pump |
| Head Pressure | sensor | m | Differential pressure |
| Power Consumption | sensor | W | Electrical power draw |
| Motor Speed | sensor | RPM | Motor rotation speed |
| Water Temperature | sensor | °C | Fluid temperature |
| Grid Voltage | sensor | V | AC input voltage |
| DC Voltage | sensor | V | DC bus voltage |
| Motor Current | sensor | A | Current draw |
| Inlet Pressure | sensor | bar | Water inlet pressure |
| PCB Temperature | sensor | °C | Circuit board temperature |
| Control Box Temperature | sensor | °C | Enclosure temperature |
| Active Alarms | text_sensor | — | Current alarm descriptions |
| Active Warnings | text_sensor | — | Current warning descriptions |

### Controls

| Entity | Type | Description |
|--------|------|-------------|
| Pump Running | switch | Start/stop the pump |
| Remote Mode | switch | Enable remote control (required for mode changes) |
| Temperature AutoAdapt | switch | Toggle AutoAdapt learning mode |
| Schedule Enabled | switch | Enable/disable weekly schedule |
| Pump Control Mode | select | Choose operating mode |
| Temperature Range Min/Max | number | Temperature setpoints (°C/°F) |
| Constant Speed Setpoint | number | RPM target (300-4200) |
| Constant Flow Setpoint | number | Flow target (m³/h) |
| Constant Pressure Setpoint | number | Pressure target (m) |
| Proportional Pressure Setpoint | number | Proportional pressure target (m) |
| Cycle Time On/Off | number | Cycle timing (seconds) |

### Schedule Editor

| Entity | Type | Description |
|--------|------|-------------|
| Schedule Editor Day | select | Mon–Sun day selector |
| Schedule Editor Layer | select | Schedule layer (0–4) |
| Schedule Start Hour/Minute | number | Entry start time |
| Schedule End Hour/Minute | number | Entry end time |
| Save Schedule Entry | button | Write entry to pump |
| Clear Schedule Entry | button | Remove entry from pump |

### Single Events

| Entity | Type | Description |
|--------|------|-------------|
| Single Event Start/End Month/Day/Hour/Minute | number | Event date/time |
| Add Single Event | button | Write one-time event to pump |
| Single Event Clear Slot | number | Slot number to clear |
| Clear Single Event | button | Remove one-time event |
| Refresh Single Events | button | Re-read events from pump |
| Single Events | text_sensor | Active one-time events display |

### Diagnostics

| Entity | Type | Description |
|--------|------|-------------|
| Serial Number | text_sensor | Pump serial number |
| Product Name | text_sensor | Device model name |
| Software Version | text_sensor | Firmware version |
| Hardware Version | text_sensor | Hardware revision |
| BLE Version | text_sensor | Bluetooth firmware |
| Start Count | sensor | Total pump starts |
| Operating Hours | sensor | Lifetime operating hours |
| Event Log | text_sensor | Last 20 start/stop events |
| History Trends | text_sensor | 10-point trend data |
| Cycle Timestamps | text_sensor | Last 10 cycle times |
| Weekly Schedule | text_sensor | Current schedule display |
| Control Mode | text_sensor | Active control mode |
| Pairing Status | binary_sensor | BLE bond status |
| Uptime | sensor | ESP32 uptime |

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

## Architecture

The component follows a layered, service-based architecture matching the [Python reference implementation](https://github.com/eman/alpha-hwr):

```
components/alpha_hwr/
├── alpha_hwr.h/cpp              # Main component (thin facade, orchestration)
├── core::                       # Foundation layer
│   ├── transport.h/cpp          # BLE I/O, command queue, FSM transaction manager
│   ├── session.h/cpp            # Connection state machine
│   ├── auth.h/cpp               # Authentication handshake
│   └── ble_connection_manager   # BLE connection lifecycle
├── protocol::                   # Protocol layer (stateless)
│   ├── codec.h/cpp              # Endianness-safe encoding/decoding, CRC
│   ├── frame_builder.h/cpp      # Build GENI request packets
│   ├── frame_parser.h/cpp       # Parse GENI responses
│   └── telemetry_decoder.h/cpp  # Decode Class 10 DataObjects
└── services::                   # Business logic layer
    ├── telemetry_service        # Sensor readings and polling
    ├── control_service          # Start/stop, modes, setpoints
    ├── schedule_service         # Weekly schedule management
    ├── device_info_service      # Device ID strings + operating statistics
    ├── time_service             # Pump RTC synchronization
    ├── event_log_service        # Start/stop event history
    ├── history_service          # Trend data + cycle timestamps
    └── sensor_publisher         # Map telemetry to ESPHome sensors
```

### Key Design Decisions

- Non-blocking transport: Command queue + FSM ensures ESPHome's event loop is never blocked
- 50ms command pacing: Prevents pump buffer overflows
- Wildcard response matching: Handles pump firmware quirks (SubID 0 responses)
- Daily time sync: Automatic RTC synchronization via SNTP
- Boot-resilient data reads: Device info, event log, etc. read on every boot regardless of auth state

---

## Troubleshooting

### Device Not Discovered
- Ensure pump is powered on with BLE enabled
- Move ESP32 closer (0.5–2m)
- Only one BLE client can connect at a time; disconnect phone apps first

### Connection Loops / Disconnects
Enable debug logging:
```yaml
logger:
  level: DEBUG
  logs:
    alpha_hwr: DEBUG
    esp32_ble_client: DEBUG
```

### No Telemetry After Boot
- Wait 10–15 seconds for the full boot sequence (auth > device info > schedules > event log > history)
- Check logs for authentication completion

### Pairing Failures
- "Confirm Value Mismatch" (0x04): Pump not in pairing mode; hold Bluetooth button until icon flashes
- "Timeout" (0x07): Move ESP32 closer to pump
- "Encryption Key Missing" (0x05): Re-flash ESP32 to clear stored bonds

### Setpoints Show "Unknown"
Setpoints only display for the currently active control mode. Switch to a mode to see its setpoint.

---

## References

- **Protocol Documentation**: [https://eman.github.io/alpha-hwr/](https://eman.github.io/alpha-hwr/)
- **Python Reference Implementation**: [https://github.com/eman/alpha-hwr](https://github.com/eman/alpha-hwr)
- **ESPHome BLE Client**: [https://esphome.io/components/ble_client/](https://esphome.io/components/ble_client/)
