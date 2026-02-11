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

## 6. Architecture: Layered Service-Based Design ✅

The ESPHome component follows the same layered, service-based architecture as the [Python Reference Implementation](reference/alpha-hwr/src/alpha_hwr). This architecture separates concerns, improves testability, and makes the codebase maintainable.

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

### ESPHome Component Implementation (COMPLETED)

> **Note on Structure**: ESPHome requires a flat file structure in `components/alpha_hwr/`, so the layered architecture is implemented using **C++ namespaces** instead of subdirectories. This provides the same logical separation while maintaining ESPHome compatibility.

**Current Structure:**

```
components/alpha_hwr/
├── alpha_hwr.h/cpp                  # Main component (thin facade, orchestration)
├── core::                           # Foundation layer (namespace)
│   ├── transport.h/cpp              # BLE I/O, command queue, FSM transaction manager
│   ├── session.h/cpp                # Connection state machine
│   ├── auth.h/cpp                   # Authentication handshake
│   └── ble_connection_manager.h/cpp # BLE connection lifecycle management
├── protocol::                       # Protocol layer (namespace, stateless)
│   ├── codec.h/cpp                  # Endianness-safe encoding/decoding, CRC
│   ├── frame_builder.h/cpp          # Build GENI request packets
│   ├── frame_parser.h/cpp           # Parse GENI responses, validate CRC
│   └── telemetry_decoder.h/cpp      # Decode Class 10 DataObjects to telemetry
└── services::                       # Business logic layer (namespace)
    ├── telemetry_service.h/cpp      # Sensor readings and polling
    ├── control_service.h/cpp        # Start/stop, modes, setpoints
    ├── schedule_service.h/cpp       # Weekly schedule management
    └── sensor_publisher.h/cpp       # Map telemetry to ESPHome sensors
```

**Implementation Highlights:**

* **Namespace Organization**: `esphome::alpha_hwr::core`, `esphome::alpha_hwr::protocol`, `esphome::alpha_hwr::services`
* **Thin Facade**: `AlphaHwrComponent` delegates all operations to services (no direct protocol manipulation)
* **Non-Blocking Transport**: Command queue + FSM state machine (`IDLE`, `SENDING_CHUNKS`, `AWAITING_RESPONSE`)
* **Transaction Safety**: 50ms pacing between commands, response matching by Object/Sub-ID
* **Reference Alignment**: File structure and APIs mirror Python implementation 1:1

### Benefits of This Architecture

1. **Testability**: Protocol layer can be unit tested without hardware.
2. **Maintainability**: Each file has a clear, single responsibility (avg ~200-300 lines vs original 1300+ line monolith).
3. **Extensibility**: New features (e.g., `HistoryService`, `TimeService`) can be added without touching existing code.
4. **Debuggability**: Smaller files and clear boundaries make bugs easier to isolate.
5. **ESPHome Compatibility**: Flat file structure with namespace-based organization meets ESPHome requirements.
6. **Reference Alignment**: Matches Python implementation 1:1, making it easier to verify correctness.

### Rules for Future Development

1. **Maintain Layering**: New features should be added to the appropriate layer/namespace.
2. **Follow Reference Implementation**: When implementing new features, mirror the Python code structure.
3. **Document Protocol References**: Every packet builder/parser must cite the protocol doc section.
4. **Test After Changes**: Verify `hwr-pump-example.yaml` compiles and `hwr-pump.yaml` works on hardware.
5. **Keep Services Focused**: Each service should own a single domain (telemetry, control, schedules, etc.).

## 7. Current Status & Implementation Progress

### ✅ Completed Features

#### Phase 1-2: Telemetry & Authentication (COMPLETE)

* [x] BLE discovery and connection
* [x] Authentication handshake (challenge/response)
* [x] Live telemetry streaming (flow rate, pressure, power, temperature)
* [x] All sensors exposed to Home Assistant
* [x] Connection stability and reconnection logic

#### Phase 3: Basic Control (COMPLETE)

* [x] Start/Stop pump commands
* [x] Mode selection (Auto, Manual, Off)
* [x] Setpoint adjustments (temperature targets)
* [x] Control buttons in Home Assistant

#### Phase 4: Schedule Management (COMPLETE)

* [x] Schedule reading from all 5 layers (0-4)
* [x] Schedule parsing and decoding
* [x] Schedule display in Home Assistant
* [x] Schedule packet building (53-byte APDU format)
* [x] Schedule validation logic
* [x] Write packet construction matches Python reference
* [x] **Schedule write persistence** (RESOLVED via Non-Blocking Transaction Manager)

### 🔄 Current Status: Component Fully Functional & Architecturally Aligned

**Architecture Status:** ✅ **COMPLETE** - The component uses a robust, layered architecture with a non-blocking transaction manager that perfectly matches the Python reference implementation's logic while respecting ESPHome's single-threaded constraints.

**What Works:**

* **Non-Blocking BLE Transactions**: All operations use a command queue and FSM to prevent event loop starvation.
* **Schedule Persistence**: Writes successfully persist to pump flash storage (verified via hardware read-back).
* **Command Pacing**: Automatic 50ms delay between packets/fragments ensures pump processing time.
* **Flexible Response Matching**: Handles pump firmware quirks (e.g., SubID 0 responses for non-zero requests).
* **Full Telemetry**: All registers (Motor, Flow, Pressure, Temp, Alarms) are polled and published correctly.
* **Bi-directional Control**: Start/Stop, Mode changes, and Schedule management are fully operational.

## 8. The Solution: Non-Blocking BLE Transaction Manager

### 8.1 The Challenge
The Grundfos pump uses a **two-phase commit** protocol for many operations (especially flash writes like schedules). The client must:
1. Send the write packet.
2. **Wait for and actively consume** an acknowledgment notification (usually Type 0xDE01).
3. Failing to consume the ACK within a specific window causes the pump to discard the change.

In ESPHome's single-threaded environment, a simple `delay()` or `while(!available())` freezes the device. An async approach that just registers a callback and returns immediately often misses the ACK because the event loop continues and other operations (like telemetry polls) interfere.

### 8.2 The Implementation: Command Queue + FSM
The refactored `core::Transport` layer implements a **Finite State Machine (FSM)** and a **Command Queue** (`std::deque<Command>`) to manage transactions safely.

#### The State Machine
* **IDLE**: Waiting for a new command in the queue.
* **SENDING_CHUNKS**: Splitting large packets (like the 53-byte schedule) into 20-byte BLE MTU chunks with 50ms pacing.
* **AWAITING_RESPONSE**: Non-blockingly waiting for a matching notification based on Object ID and Sub-ID.

#### Pacing & Isolation
The `Transport::loop()` (called every iteration of `AlphaHwrComponent::loop()`) ensures that:
1. Only one command is "in flight" at a time (Transaction Isolation).
2. A minimum of 50ms exists between every BLE write (Pacing).
3. Response matching is performed against incoming notifications before any new command is sent.

### 8.3 Firmware Quirks Handled
The implementation includes specific logic for pump-specific behaviors:
* **SubID 0 Quirk**: The pump often responds with `SubID 0` even when a specific Sub-ID (like `1000`) was requested. The matching logic now treats `SubID 0` as a wildcard match for the requested Object ID.
* **OpSpec 0x01**: Some writes trigger an immediate `OpSpec 0x01` response. The matching logic has been tuned to be flexible while ensuring the transaction window remains open long enough for the commit to finish.

## 9. Workflows

### Creating New Features

The layered architecture is now in place. When adding new features:

1. **Check Reference**: Look at how the Python implementation does it (check `reference/alpha-hwr/src/alpha_hwr`).
2. **Identify Layer**: Determine which namespace/layer owns this feature:
   * `protocol::` for packet encoding/decoding (stateless)
   * `core::` for BLE transport, session state, authentication
   * `services::` for business logic (telemetry, control, schedules, etc.)
3. **Plan**: Define the packet structure and state flow.
4. **Implement Protocol**: Add packet builder/parser functions in the `protocol` namespace (codec, frame_builder, frame_parser, telemetry_decoder).
5. **Unit Test**: Verify the packet builder produces the correct hex (compare with Python reference).
6. **Implement Service**: Add business logic to the appropriate service in the `services` namespace or create a new service.
7. **Integration**: Hook the service into the main `AlphaHwrComponent` class (add accessors as needed).
8. **Verify**: Compile `hwr-pump-example.yaml`, flash `hwr-pump.yaml`, and test on hardware.

## 10. Development Session History

### Session: 2026-02-11 - Part 2: Non-Blocking Transaction Manager & Schedule Persistence Success

**Goal:** Resolve schedule write persistence issue and implement robust BLE request-response transactions.

**What We Did:**

1. **Implemented Non-Blocking Transport Layer**
   * Created a command queue (`std::deque<Command>`) in `core::Transport`.
   * Implemented a 3-state FSM (`IDLE`, `SENDING_CHUNKS`, `AWAITING_RESPONSE`) in `loop()`.
   * Added 50ms pacing between packet fragments and commands.
   * Implemented chunked sending for large packets (53-byte schedules).

2. **Resolved Schedule Persistence Blocker**
   * Discovered that the pump requires active ACK consumption within a 3-second window to commit data to flash.
   * Implemented asynchronous response matching by Object ID and Sub-ID.
   * Handled the pump's `SubID 0` response quirk.
   * **Verified:** Successfully wrote a test schedule and read it back from the pump's flash.

3. **Service Integration & Cleanup**
   * Refactored `TelemetryService`, `ControlService`, `ScheduleService`, and `Authentication` to use the unified `transport_.send_command()` API.
   * Removed obsolete `WriteCallback` and `SchedulerCallback` patterns.
   * Updated Home Assistant button lambdas to be asynchronous for better UI feedback.

**Current State:**

* **Device:** Fully operational and stable (10.0.1.86).
* **Telemetry:** Streaming smoothly with paced polling.
* **Schedules:** Persistence fixed; bi-directional management fully functional.
* **Architecture:** Layered, service-based, and non-blocking.
