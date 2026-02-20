# Component Architecture

The component follows a layered, service-based architecture matching the [Python reference implementation](https://github.com/eman/alpha-hwr).

## File Structure

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

## Layers

- **`alpha_hwr`** — Thin facade. Delegates all work to services. No direct protocol manipulation.
- **`core::`** — Manages BLE I/O, connection state, and authentication. The transport uses a command queue and 3-state FSM (`IDLE` → `SENDING_CHUNKS` → `AWAITING_RESPONSE`) to stay non-blocking inside ESPHome's event loop.
- **`protocol::`** — Stateless frame builders and parsers. Pure functions with no side effects. Fully unit-testable on host without hardware.
- **`services::`** — One service per domain. Each owns all operations for its area (telemetry, control, schedules, etc.).

## Key Design Notes

- **Non-blocking transport**: 50ms pacing between commands; only one command in flight at a time.
- **Response matching**: Flexible Object/Sub-ID matching handles pump firmware quirks (SubID 0 wildcard responses).
- **Time sync**: Automatic daily RTC synchronization via SNTP; initial sync fires 2 seconds after authentication.
- **Namespace organization**: ESPHome requires a flat file structure, so layering is achieved via C++ namespaces (`esphome::alpha_hwr::core`, `::protocol`, `::services`) rather than subdirectories.

## Adding New Features

1. Identify the layer: `protocol::` for packet encoding, `core::` for transport/state, `services::` for business logic.
2. Mirror the equivalent Python reference service.
3. Cite the [GENI protocol doc](https://eman.github.io/alpha-hwr/reimplementation/) for any packet formats.
4. Unit-test packet builders against known byte sequences before flashing.
