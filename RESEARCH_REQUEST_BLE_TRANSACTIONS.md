# Research Request: Non-Blocking BLE Request-Response in ESPHome

## Purpose
This document requests research into how to implement **synchronous request-response BLE transactions** in ESPHome's **single-threaded event loop** without blocking.

## The Core Problem

### What We're Trying to Do
Send a BLE GATT write to a pump and **wait for an acknowledgment response** (via BLE notification) before continuing. This is a **two-phase commit** where:
1. Client sends write → Pump buffers to RAM
2. Client reads ACK → Pump commits to flash

### Why It's Hard in ESPHome
ESPHome runs on a **single event loop** that handles:
- WiFi management
- Home Assistant API
- BLE operations
- All user code

**Blocking the loop causes device freeze:**
```cpp
// ❌ CRASHES THE DEVICE
while (millis() - start < 3000) {
  delay(10);  // Freezes WiFi, BLE, everything
}
```

### Python Reference (Works)
```python
# Uses asyncio - non-blocking but synchronous-looking
async with self._transaction_lock:
    self.clear_response_queue()
    await self.write(packet)
    response = await self.read_response(timeout=3.0)  # Waits up to 3s
    return response
```

### Our C++ Attempt (Doesn't Work)
```cpp
// Registers handler and returns immediately
register_response_handler(0xDE01, [](bytes response) {
  ack_received = true;  // Never fires
});

set_timeout(3000, [on_complete]() {
  on_complete(true);  // Called but write doesn't persist
});

return;  // Returns immediately - ACK never consumed
```

**Result:** Device stays online, but pump doesn't persist the write (read-back shows 0 entries).

## Research Questions

### 1. What Pattern Do Other ESPHome BLE Components Use?

**Search for:**
- Components that send BLE write and wait for response
- How they avoid blocking the event loop
- State machines, deferred callbacks, or other patterns

**Suggested Components to Study:**
- `esphome/components/ble_client/` - Core implementation
- `esphome/components/xiaomi_ble/` - If it does writes
- `esphome/components/pvvx_mithermometer/` - If it does config writes
- `esphome/components/bluetooth_proxy/` - Queue management

**Where to Look:**
- ESPHome GitHub: https://github.com/esphome/esphome
- Component examples: `esphome/components/*/`
- Look for: `register_for_notify()`, `write_value()`, response handling

### 2. What ESP-IDF Patterns Exist?

**Search for:**
- ESP32 BLE GATT client request-response patterns
- How to wait for notification without blocking
- FreeRTOS primitives that work in single-threaded context

**Documentation to Check:**
- [ESP-IDF BLE GATT Client](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gattc.html)
- [FreeRTOS Event Groups](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html#event-groups)
- ESP-IDF examples: `examples/bluetooth/bluedroid/ble_gattc/`

### 3. Can We Use Component::loop()?

**Investigate:**
- Can we implement a state machine in `loop()` that polls for responses?
- Can we queue operations and process them one at a time?
- How to signal completion back to caller?

**Possible Pattern:**
```cpp
enum class OpState { IDLE, WAITING_RESPONSE };
OpState state_ = OpState::IDLE;
uint32_t response_deadline_ = 0;
bool ack_received_ = false;

void write_schedule_async() {
  send_packet();
  state_ = OpState::WAITING_RESPONSE;
  response_deadline_ = millis() + 3000;
  ack_received_ = false;
}

void loop() override {
  if (state_ == OpState::WAITING_RESPONSE) {
    if (ack_received_ || millis() > response_deadline_) {
      state_ = OpState::IDLE;
      handle_completion(ack_received_);
    }
  }
}

void on_notify(bytes data) {
  // BLE notification handler
  if (is_schedule_ack(data)) {
    ack_received_ = true;
  }
}
```

### 4. Do We Need to Actively Consume BLE Notifications?

**Critical Question:**
- Does `register_for_notify()` guarantee callback execution?
- Are notifications buffered in hardware FIFO?
- Do callbacks fire from ISR or event loop?
- Can notifications be dropped if event loop is busy?

**Test Needed:**
- Does the Python reference actually receive a response?
- What does the pump send back (packet capture)?

## Technical Context

### Hardware
- **Device:** ESP32-C3 DevKit
- **Pump:** Grundfos ALPHA HWR (BLE GATT server)
- **Protocol:** Custom GENI protocol over BLE

### BLE Configuration
```cpp
// Service/Characteristic UUID
"25598669-4359-44D1-B8D1-5D7F26538AFE"

// Notification registration
client->register_for_notify(char_handle, [](bytes data) {
  // Handle notification
});

// Write operation
client->write_value(char_handle, packet, 
                    ESP_GATT_WRITE_TYPE_NO_RSP);
```

### Expected Behavior
1. Send 53-byte schedule write packet
2. Wait up to 3 seconds for Type 0xDE01 response
3. If ACK received → pump commits to flash
4. If timeout → treat as success (matches Python)
5. Read back schedule → should show 1 entry

### Current Behavior
1. Send 53-byte schedule write packet
2. Return immediately (no wait)
3. Timeout fires after 3 seconds
4. Read back schedule → shows 0 entries (write didn't persist)

## Success Criteria

Find a pattern that:
1. ✅ Sends BLE write packet
2. ✅ Waits up to 3 seconds for notification response
3. ✅ Doesn't block ESPHome event loop
4. ✅ Keeps WiFi/HA/BLE responsive
5. ✅ Pump persists the write (read-back shows entries)

## Code References

### Our Implementation
- `components/alpha_hwr/schedule_service.cpp:485-598` - Async write method
- `components/alpha_hwr/alpha_hwr.cpp:163-165` - Timeout wiring
- `hwr-pump.yaml:149-190` - Test button configuration

### Python Reference
- `reference/alpha-hwr/src/alpha_hwr/core/transport.py:415-485` - `query()` method
- `reference/alpha-hwr/src/alpha_hwr/services/schedule.py:520` - `write_entries()` usage

### Working Directory
All paths are relative to the git repository root (`esphome-alpha-hwr/`)

## Deliverable

Please provide:
1. **Pattern identification:** How do existing ESPHome components solve this?
2. **Code examples:** Working examples from ESPHome or ESP-IDF
3. **Implementation guidance:** How to adapt the pattern to our use case
4. **Trade-offs:** What are the limitations/gotchas?

If no existing pattern found:
- Explain why this is difficult
- Suggest alternative approaches
- Recommend if we need a full transport layer refactor
