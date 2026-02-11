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
* **Simple Configuration**: The `hwr-pump-example.yaml` configuration should demonstrate best practices and be the reference for users. We prioritize good architecture over backward compatibility since this is a new library.
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

Before marking a task as complete, verify on actual hardware using `hwr-pump.yaml`:

1. **Compile & Flash**: Ensure no compilation errors.
2. **Discovery**: Does the device show up? (Check logs for "Found ALPHA HWR").
3. **Connection**: Does it connect *and stay connected*?
4. **Telemetry**: Do values update? (Wave hand over pump or start water flow to verify changes).

Note: `hwr-pump-example.yaml` is for documentation and compilation testing only (contains placeholder values). Use `hwr-pump.yaml` with real device configuration for actual hardware testing.

## 5. Documentation Requirements

* **Code Comments**: Explain *why* a specific byte sequence is used (reference the protocol doc).
  * *Bad*: `// Send 0x02`
  * *Good*: `// Send 0x02 (Class 10 Start Byte) - See Protocol Doc Sec 3.1`
* **PR/Commit Messages**: Clearly state what changed and what was tested.
* **README updates**: If a new feature is added (e.g., a "Boost Mode" switch), update the `README.md` and `hwr-pump-example.yaml` Config section immediately.

## 6. Architecture: Layered Service-Based Design

The ESPHome component **MUST** follow the same layered, service-based architecture as the [Python Reference Implementation](reference/alpha-hwr/src/alpha_hwr). This architecture separates concerns, improves testability, and makes the codebase maintainable.

### Reference Implementation Structure

The Python reference implementation organizes code into four layers:

```
alpha_hwr/
├── client.py              # Main client facade (orchestration layer)
├── models.py              # Data structures (TelemetryData, ScheduleEntry, etc.)
├── core/                  # Foundation layer
│   ├── transport.py       # BLE packet I/O, notification handling, transaction locking
│   ├── session.py         # Connection state management (IDLE, AUTHENTICATING, CONNECTED)
│   └── authentication.py  # Authentication handshake sequences
├── protocol/              # Protocol layer (stateless)
│   ├── codec.py           # Primitive encoding/decoding (float_be, uint16_be, etc.)
│   ├── frame_builder.py   # Build GENI request packets
│   ├── frame_parser.py    # Parse GENI response frames
│   └── telemetry_decoder.py  # Decode telemetry from Class 10 DataObjects
└── services/              # Business logic layer
    ├── base.py            # Shared protocol helpers for all services
    ├── telemetry.py       # Read sensor data (flow, pressure, power, temperature)
    ├── control.py         # Start/stop pump, set modes and setpoints
    ├── schedule.py        # Manage weekly pump schedules
    ├── device_info.py     # Read device identification and statistics
    ├── configuration.py   # Backup and restore pump configuration
    ├── time.py            # Read and synchronize pump real-time clock
    ├── history.py         # Read historical trend data
    └── event_log.py       # Read pump event log entries
```

**Key Architectural Principles:**

1. **Separation of Concerns**: Each layer has a single, well-defined responsibility.
2. **Client as Facade**: The `AlphaHWRClient` is a thin orchestration layer that delegates all work to services.
3. **Services Own Business Logic**: Each service encapsulates all operations for its domain (e.g., `TelemetryService` handles all telemetry operations).
4. **Protocol Layer is Stateless**: Frame builders and parsers are pure functions with no side effects.
5. **Core Layer Manages State**: Transport handles BLE I/O, Session tracks connection state, Authentication handles handshake.

### Required ESPHome Component Structure

The current monolithic `alpha_hwr.cpp` (45KB, 1300+ lines) **MUST BE REFACTORED** to follow this architecture:

```
components/alpha_hwr/
├── alpha_hwr.h            # Main component class (thin facade like client.py)
├── alpha_hwr.cpp          # Component registration and ESPHome integration
├── core/
│   ├── transport.h/cpp    # BLE packet I/O, transaction locking, response queuing
│   ├── session.h/cpp      # Connection state machine (enum class State)
│   └── auth.h/cpp         # Authentication handshake (challenge/response)
├── protocol/
│   ├── codec.h/cpp        # Endianness-safe float/int encoding/decoding
│   ├── frame_builder.h/cpp  # Build GENI requests (Class 10, Class 7, etc.)
│   ├── frame_parser.h/cpp   # Parse GENI responses (validate CRC, extract payload)
│   └── telemetry_decoder.h/cpp  # Map Class 10 objects to telemetry fields
├── services/
│   ├── base_service.h/cpp       # Shared helpers (_read_class10_object, etc.)
│   ├── telemetry_service.h/cpp  # Sensor readings and streaming
│   ├── control_service.h/cpp    # Start/stop, mode changes, setpoints
│   ├── schedule_service.h/cpp   # Schedule management
│   ├── device_info_service.h/cpp  # Device identification
│   └── time_service.h/cpp       # RTC synchronization
└── models/
    ├── telemetry_data.h     # TelemetryData struct
    ├── schedule_entry.h     # ScheduleEntry struct
    └── device_info.h        # DeviceInfo struct
```

### Refactoring Approach

**Phase 1: Extract Protocol Layer** (No Behavioral Changes)
- Move CRC calculation to `protocol/codec.cpp`
- Move packet building to `protocol/frame_builder.cpp`
- Move frame parsing to `protocol/frame_parser.cpp`
- Move telemetry decoding to `protocol/telemetry_decoder.cpp`
- Add unit tests for each protocol function

**Phase 2: Extract Core Layer** (Minimal Behavioral Changes)
- Move BLE I/O to `core/transport.cpp` (transaction lock, response queue)
- Move state management to `core/session.cpp` (state machine enum)
- Move authentication to `core/auth.cpp` (handshake sequence)
- Verify existing features still work

**Phase 3: Extract Services** (Refactor Business Logic)
- Create `services/base_service.cpp` with shared helpers
- Create `services/telemetry_service.cpp` for all sensor reading logic
- Create `services/control_service.cpp` for start/stop/mode logic
- Create `services/schedule_service.cpp` for schedule operations
- Update `alpha_hwr.cpp` to delegate to services

**Phase 4: Clean Up Main Component** (Simplify)
- Reduce `alpha_hwr.cpp` to a thin facade
- Initialize services in `setup()`
- Delegate all operations to services
- Remove all direct protocol manipulation from main class

### Benefits of This Architecture

1. **Testability**: Protocol layer can be unit tested without hardware.
2. **Maintainability**: Each file has a clear, single responsibility.
3. **Extensibility**: New features (e.g., `HistoryService`) can be added without touching existing code.
4. **Debuggability**: Smaller files and clear boundaries make bugs easier to isolate.
5. **Cross-Platform**: Services can be reused if we port to other frameworks (Arduino, PlatformIO, etc.).
6. **Reference Alignment**: Matches Python implementation 1:1, making it easier to verify correctness.

### Rules for Refactoring

1. **No Behavioral Changes During Refactoring**: Extract code, don't rewrite it.
2. **Test After Each Phase**: Verify `hwr-pump-example.yaml` still compiles and `hwr-pump.yaml` works with the new structure on hardware.
3. **Follow Reference Implementation**: When in doubt, do what the Python code does.
4. **Document Protocol References**: Every packet builder/parser must cite the protocol doc section.
5. **Improve as You Go**: Since this is a new library, feel free to improve APIs and configuration structure during refactoring if it results in better design.

## 7. Workflows

### Creating New Features

1. **Check Reference**: Look at how the Python lib does it (check `reference/alpha-hwr/src/alpha_hwr`).
2. **Identify Layer**: Determine which layer owns this feature (Protocol? Service? Core?).
3. **Plan**: Define the packet structure and state flow.
4. **Implement Protocol**: Add packet builder/parser in `protocol/` layer.
5. **Unit Test**: Verify the packet builder produces the correct hex (compare with Python).
6. **Implement Service**: Add business logic to appropriate service in `services/` layer.
7. **Integration**: Hook service into main component class.
8. **Verify**: Flash and test on hardware.
