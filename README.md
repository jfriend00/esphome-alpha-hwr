# ALPHA HWR Component for ESPHome

ESPHome component for Grundfos ALPHA HWR hot water recirculation pumps. This component uses BLE (Bluetooth Low Energy) to decode the pump's GENI protocol and retrieve real-time sensor data.

## Features

- **Real-time Monitoring**:
  - Flow rate (m³/h)
  - Head pressure (m)
  - Power consumption (W)
  - Motor RPM
  - Water/media temperature (°C)
  - **Grid Voltage (V)** (Enhanced)
  - **DC Voltage (V)** (Enhanced)
  - **Motor Current (A)** (Enhanced)
  - **Inlet Pressure (bar)** (Enhanced)
  - **Outlet Pressure (bar)** (Enhanced)
  - **Converter Temperature (°C)** (Enhanced)
  - **PCB Temperature (°C)** (Enhanced)
  - **Control Box Temperature (°C)** (Enhanced)
- **Automatic Connection**: Manages the BLE handshake and authentication.
- **Secure Pairing**: Supports BLE Bonding for encrypted communication.
- **Reliable Discovery**: Identifies pumps via Grundfos Company ID.

---

## Bluetooth Pairing

The Grundfos ALPHA HWR pump supports **two modes of operation**:

### Basic Mode (Default - No Pairing Required)
The pump streams basic telemetry via **passive BLE notifications** without requiring pairing:
- Flow rate, head pressure, power, RPM, water temperature

This mode works out-of-the-box with the `hwr-pump-example.yaml` configuration.

### Enhanced Mode (Requires Pairing)
By enabling BLE bonding (pairing), you gain access to **additional telemetry**:
- Grid voltage, DC voltage, motor current
- Inlet/outlet pressure sensors
- Converter/PCB/control box temperatures

**To enable pairing**, add `enable_pairing: true` to your configuration:

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true  # Enable BLE pairing for enhanced telemetry
  voltage:
    name: "Grid Voltage"
  voltage_dc:
    name: "DC Voltage"
  current:
    name: "Motor Current"
  inlet_pressure:
    name: "Inlet Pressure"
  # ... other enhanced sensors
```

### Pairing Procedure
1.  **Configure ESP32**: Use the `hwr-pairing-example.yaml` configuration with your pump's MAC address and `enable_pairing: true`.
2.  **Enter Pairing Mode**: On the pump, press and hold the **Bluetooth button** until the Bluetooth icon on the display starts flashing.
3.  **ESP32 Connection**: Ensure the ESP32 is powered on and within range. It will automatically detect the pump and attempt to bond.
4.  **Verify**: Check the ESPHome logs. You should see `✓ BLE authentication complete (Pairing/Bonding successful)!`.
5.  **Status Sensor**: If you configured the `pairing_status` binary sensor, it will show as `Connected` in Home Assistant once bonding is established.

> **Note**: Once paired, the ESP32 stores the encryption keys. You don't need to put the pump in pairing mode again unless you factory reset the ESP32 or the pump.

---

## Enhanced Telemetry
By default, the pump streams basic telemetry. By establishing a secure paired connection, this component can access additional telemetry data:

### Motor State Object (High Frequency - ~10 Hz)
- **Grid Voltage (V)** - AC input voltage
- **DC Voltage (V)** - DC bus voltage (motor power supply)
- **Motor Current (A)** - Current draw
- **Converter Temperature (°C)** - Motor electronics temperature

### Flow & Pressure Object (Hydraulic Data - ~1 Hz)
- **Flow Rate (m³/h)** - Water flow through pump
- **Head Pressure (m)** - Differential pressure generated
- **Inlet Pressure (bar)** - Water inlet pressure
- **Outlet Pressure (bar)** - Water outlet pressure

### Temperature Object (Environmental Data - ~1 Hz)
- **Water/Media Temperature (°C)** - Fluid being pumped
- **PCB Temperature (°C)** - Circuit board temperature
- **Control Box Temperature (°C)** - Enclosure ambient temperature

These enhanced metrics enable:
- **Electrical Health Monitoring**: Track voltage stability (AC/DC) and current draw
- **Hydraulic Analysis**: Monitor inlet/outlet pressures for system diagnostics
- **Thermal Management**: Monitor all system temperatures
- **Bi-directional Communication**: Enables future features like remote start/stop and schedule management

## Hardware Requirements

- **ESP32 Board**: ESP32, ESP32-C3, or ESP32-S3 (must support BLE).
- **Grundfos ALPHA HWR Pump**: Powered on and BLE enabled.
- **Proximity**: Keep the ESP32 within 0.5–2 meters during initial setup.

---

## Integration Guide

### Step 1: Discover Your Pump's MAC Address

You need your pump's MAC address to configure the BLE client. You can find this using a mobile BLE scanner app or by running the provided discovery utility on your ESP32.

**Using the Discovery Utility:**
1.  Navigate to `components/alpha_hwr/discovery_example.yaml`.
2.  Add your WiFi secrets to a `secrets.yaml` file (see below).
3.  Flash the discovery firmware:
    ```bash
    esphome run components/alpha_hwr/discovery_example.yaml
    ```
4.  Watch the logs for "Found ALPHA HWR Pump!" and copy the MAC address (e.g., `3C:E0:02:XX:XX:XX`).

### Step 2: Configure Secrets

Create a `secrets.yaml` file in your project root to store sensitive credentials:

```yaml
wifi_ssid: "YourWiFi"
wifi_password: "YourWiFiPassword"
ap_password: "FallbackAPPassword"
api_key: "GeneratARandom32ByteKeyHere"
ota_password: "SecureOTAPassword"
```

### Step 3: Create Your Device Configuration

Create a file named `hwr-pump.yaml` (or copy the provided example) with the following structure:

```yaml
esphome:
  name: hwr-pump
  friendly_name: "HWR Pump"

esp32:
  board: esp32-c3-devkitm-1 # Update for your board
  variant: esp32c3
  framework:
    type: esp-idf # Required for stable BLE

# Import the custom component
external_components:
  - source:
      type: local
      path: .
    components: [alpha_hwr]

# Enable BLE Scanning
esp32_ble_tracker:
  scan_parameters:
    interval: 1.1s
    window: 1.1s

# Configure BLE Client
ble_client:
  - mac_address: "XX:XX:XX:XX:XX:XX" # Replace with your MAC
    id: hwr_pump_client

# Configure the Component
alpha_hwr:
  ble_client_id: hwr_pump_client
  flow:
    name: "Flow Rate"
  head:
    name: "Head Pressure"
  power:
    name: "Power Consumption"
  rpm:
    name: "Motor Speed"
  temp_media:
    name: "Water Temperature"

# Standard Connectivity
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "HWR-Pump-Fallback"
    password: !secret ap_password

api:
  encryption:
    key: !secret api_key

ota:
  password: !secret ota_password
```

### Step 4: Build and Flash

```bash
esphome run hwr-pump.yaml
```

Once running, Home Assistant will automatically detect the device (via API) and expose the sensors.

---

## Architecture & Protocol

### Component Layout
- **`alpha_hwr/__init__.py`**: Registers the component, binds the YAML schema, and links the BLE client.
- **`alpha_hwr/alpha_hwr.cpp`**: Implements the BLE event handler (`gattc_event_handler`). It manages the authentication state machine, parses incoming GENI packets, and publishes sensor data.

### BLE Handshake
1.  **Connection**: ESPHome connects to the pump's MAC.
2.  **Discovery**: Scans for GENI Service UUID `0000fdd0-0000-1000-8000-00805f9b34fb`.
3.  **Authentication**: The component writes a sequence of "Magic" and "Unlock" packets to the TX characteristic to enable telemetry.
4.  **Telemetry**: The pump streams Class 10 frames (`OpSpec 0x0E`) on the RX characteristic. The component decodes these Big-Endian floats into ESPHome sensor values.

### Extending Coverage
To add more sensors:
1.  Identify the Object ID and Offset using the [Protocol Reference](https://eman.github.io/alpha-hwr/).
2.  Add a new sensor pointer in `alpha_hwr.h`.
3.  Add the decoding logic in `alpha_hwr.cpp` inside `decode_packet()`.
4.  Register the sensor in `__init__.py`.

---

## Troubleshooting

### Device Not Discovered
- **Check Power**: Ensure pump is on.
- **Check Range**: Move ESP32 closer (0.5m - 2m).
- **Check Concurrency**: BLE is point-to-point. Ensure no other phone or hub is connected to the pump.

### Connection Loops / Disconnects
- **Logs**: Enable verbose logging to see the handshake:
    ```yaml
    logger:
      level: VERBOSE
      logs:
        esp32_ble_client: VERBOSE
        alpha_hwr: VERBOSE
    ```
- **Authentication**: Look for `Authenticating with pump...` in the logs. If this fails, the pump might require a different protocol version.

### No Telemetry
- **Wait**: It can take 5-10 seconds after connection for data to flow.
- **Activity**: Some pumps only report data when the motor is running.

### Pairing/Bonding Issues

The component provides detailed pairing logs to help debug authentication problems. When pairing is in progress, you'll see log messages like:

**Successful Pairing:**
```
[I][alpha_hwr:446] BLE security request from device AA:BB:CC:DD:EE:FF - accepting
[I][alpha_hwr:438] ✓ BLE authentication complete (Pairing/Bonding successful)!
[I][alpha_hwr:439]   Device: AA:BB:CC:DD:EE:FF
[I][alpha_hwr:440]   Auth mode: 0x01
[I][alpha_hwr:520] BLE bonding complete - device credentials stored
```

**Failed Pairing:**
```
[W][alpha_hwr:466] ✗ BLE authentication failed!
[W][alpha_hwr:467]   Device: AA:BB:CC:DD:EE:FF
[W][alpha_hwr:468]   Failure reason: Timeout (0x07)
[W][alpha_hwr:469]   Auth mode: 0x00
```

**Common Failure Reasons:**
- **"Confirm Value Mismatch"** (0x04): The pump rejected pairing - ensure pump is in pairing mode (Bluetooth icon flashing)
- **"Timeout"** (0x07): Pairing took too long - check BLE signal strength, move ESP32 closer
- **"Encryption Key Missing"** (0x05): Bonding information was lost - factory reset ESP32 or clear bonds
- **"Bond Creation Failed"** (0x08): Storage issue - check ESP32 NVS partition

**Troubleshooting Steps:**
1. **Put pump in pairing mode**: Hold Bluetooth button until icon flashes
2. **Clear existing bonds**: If you see "Encryption Key Missing", reflash ESP32 or use `esp_ble_remove_bond_device()`
3. **Check signal strength**: Keep ESP32 within 0.5-2m during pairing
4. **Verify configuration**: Ensure `esp32.framework.type: esp-idf` is set (Arduino framework has pairing limitations)
5. **Check logs**: Enable DEBUG level logging for detailed GAP event information

---

## References

- **Protocol Documentation**: [https://eman.github.io/alpha-hwr/](https://eman.github.io/alpha-hwr/) - Detailed UUIDs, packet structures, and authentication bytes.
- **ESPHome BLE Client**: [https://esphome.io/components/ble_client.html](https://esphome.io/components/ble_client.html)
