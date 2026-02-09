# ALPHA HWR Component for ESPHome

ESPHome component for Grundfos ALPHA HWR hot water recirculation pumps. This component uses BLE (Bluetooth Low Energy) to decode the pump's GENI protocol and retrieve real-time sensor data.

## Features

- **Real-time Monitoring**:
  - Flow rate (m³/h)
  - Head pressure (m)
  - Power consumption (W)
  - Motor RPM
  - Media temperature (°C)
- **Automatic Connection**: Manages the BLE handshake and authentication.
- **Reliable Discovery**: Identifies pumps via Grundfos Company ID.

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

---

## References

- **Protocol Documentation**: [https://eman.github.io/alpha-hwr/](https://eman.github.io/alpha-hwr/) - Detailed UUIDs, packet structures, and authentication bytes.
- **ESPHome BLE Client**: [https://esphome.io/components/ble_client.html](https://esphome.io/components/ble_client.html)
