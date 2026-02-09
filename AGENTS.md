# AGENTS.md - Developer Guide for Agents

This document serves as the **primary reference for AI Agents** and developers working on the `esphome-alpha-hwr` project. It defines the project's goals, coding standards, and strategic approach to ensure consistency and quality.

## 1. Project Mission & Goals

Our mission is to provide a robust, reliable, and feature-rich ESPHome component for the **Grundfos ALPHA HWR** hot water recirculation pump.

### core Objectives

1. **Metric Parity**: Match the telemetry capabilities of the [Python Reference Implementation](https://github.com/eman/alpha-hwr).
2. **Control Capability**: Implement bi-directional control (Start/Stop, Set Mode, Schedules) via BLE.
3. **Stability**: Ensure the component is stable significantly longer than the transient connection times of a mobile app.
4. **User Experience**: Provide "It Just Works" discovery and configuration for Home Assistant users.

### Strategic Principles

* **Incrementalism**: We build complexity layer-by-layer.
  * *Phase 1:* Passive Telemetry (READ only).
  * *Phase 2:* Authenticated Telemetry (UNLOCK + READ).
  * *Phase 3:* Basic Control (WRITE commands).
  * *Phase 4:* Complex Management (Schedules, Time Sync).
* **Backward Compatibility**: The `hwr-pump-example.yaml` configuration **MUST ALWAYS** work. Advanced features (like pairing/bonding) must be additive or optional, not breaking the simplest use case unless absolutely necessary.
* **No Regressions**: New features (e.g., adding schedule writing) must not break existing features (e.g., live flow rate reporting).

## 2. Key References

Agents **MUST** consult these resources before making architectural decisions:

* **Protocol Documentation**: [https://eman.github.io/alpha-hwr/reimplementation/](https://eman.github.io/alpha-hwr/reimplementation/) (The "Bible" for the GENI byte-level protocol).
* **Python Reference Code**: [https://github.com/eman/alpha-hwr](https://github.com/eman/alpha-hwr) (If you need to know *how* to implement a sequence, look here first).
* **ESPHome BLE Client**: [https://esphome.io/components/ble_client/](https://esphome.io/components/ble_client/)
* **ESPHome Bluetooth Proxy**: [https://esphome.io/components/bluetooth_proxy/](https://esphome.io/components/bluetooth_proxy/)

## 3. Development Standards

### C++ (ESPHome Component)

* **Style**: Follow standard ESPHome/Google C++ conventions.
* **Logging**: Use `ESP_LOGx` macros for all output.
  * `ESP_LOGV`: Packet dumps, heavy frequency loop data.
  * `ESP_LOGD`: State transitions, single packet summaries.
  * `ESP_LOGI`: Connection events, successful auth, major status changes.
  * `ESP_LOGW`: Retries, unexpected (but handled) data, timeout warnings.
  * `ESP_LOGE`: Critical failures, unrecoverable errors.
* **Endianness**: The GENI protocol is **Big-Endian**. Always use helper functions (e.g., `read_float_be`, `put_unaligned_be32`) or standard `htonl`/`ntohl` to ensure portability. **Do not assume host endianness.**
* **State Machines**: Explicitly model complex interactions (e.g., Authentication Handshake) as state machines `enum class State { IDLE, AUTH_CHALLENGE, AUTH_RESPONSE, CONNECTED }`. Avoid deep nested `if/else` in the loop.

### Python (Support Scripts/Tools)

* **Formatting**: Use `black` and strict type hinting (`mypy`).
* **Structure**: Keep tools modular. If writing a script to parse logs, put it in `tools/`.

## 4. Testing Strategy

### Unit Testing (Host-Based)

Logic that does not depend on ESP hardware **MUST** be testable on the host machine. This includes:

* **CRC Calculations**: Verify `calc_crc16` against known test vectors.
* **Frame Encoders/Decoders**: Verify that `build_packet` produces the exact byte sequence expected by the pump.
* **Parsing Logic**: Verify `decode_packet` correctly extracts float values from raw hex strings.

*Action*: Create simple C++ test files (e.g., `tests/test_protocol.cpp`) that can be compiled with `g++` or `clang` on the developer's machine to verify this logic without flashing a device.

### Hardware Verification

Before marking a task as complete, verify on actual hardware using `hwr-pump-example.yaml`:

1. **Compile & Flash**: Ensure no compilation errors.
2. **Discovery**: Does the device show up? (Check logs for "Found ALPHA HWR").
3. **Connection**: Does it connect *and stay connected*?
4. **Telemetry**: Do values update? (Wave hand over pump or start water flow to verify changes).

## 5. Documentation Requirements

* **Code Comments**: Explain *why* a specific byte sequence is used (reference the protocol doc).
  * *Bad*: `// Send 0x02`
  * *Good*: `// Send 0x02 (Class 10 Start Byte) - See Protocol Doc Sec 3.1`
* **PR/Commit Messages**: Clearly state what changed and what was tested.
* **README updates**: If a new feature is added (e.g., a "Boost Mode" switch), update the `README.md` and `hwr-pump-example.yaml` Config section immediately.

## 6. Workflows

### Creating New Features

1. **Check Reference**: Look at how the Python lib does it.
2. **Plan**: Define the packet structure and state flow.
3. **Implement Logic**: Add the packet builder/parser in C++.
4. **Unit Test**: Verify the packet builder produces the correct hex.
5. **Integration**: Hook it up to `alpha_hwr.cpp` loop or action.
6. **Verify**: Flash key and test.
