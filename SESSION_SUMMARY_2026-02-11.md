# Session Summary: 2026-02-11 - Schedule Write Investigation

## What We Accomplished

### 1. Root Cause Analysis ✅
Identified why schedule writes don't persist to pump flash:
- **Python reference** uses blocking `transport.query()` with 3-second wait for ACK
- **Our C++ code** uses fire-and-forget (sends packet, returns immediately)
- **Hypothesis:** Pump uses two-phase commit (buffer → wait for ACK read → commit to flash)

### 2. Attempted Blocking Solution ❌
- Added `while` loop with `delay()` to wait 3 seconds
- **Result:** Device completely froze, WiFi disconnected, required hard reset
- **Learning:** ESPHome single-threaded architecture cannot tolerate blocking operations

### 3. Implemented Async Solution ⚠️
- Added `write_entries_async()` with completion callback pattern
- Used ESPHome's `set_timeout()` for non-blocking 3-second wait
- Registered response handler for Type 0xDE01 (schedule ACK)
- **Result:** Device stays responsive, but write still doesn't persist (0 entries on read-back)
- **Issue:** ACK handler registered but may not be actively consumed during wait period

## Files Modified

### C++ Component
- `components/alpha_hwr/schedule_service.h` - Added async write methods and timeout callback setter
- `components/alpha_hwr/schedule_service.cpp` - Implemented async write with callbacks (lines 485-598)
- `components/alpha_hwr/alpha_hwr.h` - Exposed async API to YAML
- `components/alpha_hwr/alpha_hwr.cpp` - Wired up ESPHome `set_timeout()` callback

### Configuration & Testing
- `hwr-pump.yaml` - Added "Write Test Schedule (Async)" button (lines 149-190)
- `test_python_schedule_write.py` - Created Python test script (needs fixing)

### Documentation
- `AGENTS.md` - Added Section 7 (Current Status), Section 8 (Critical Challenge), Section 10 (Session History)
- `RESEARCH_REQUEST_BLE_TRANSACTIONS.md` - Created research brief for next agent

## Current Status

### Device State
- **ESP32-C3:** Online and responsive at 10.0.1.86
- **Firmware:** Compiled and flashed successfully (config_hash: 0x8025e8b8)
- **Test Button:** Available in Home Assistant - "Write Test Schedule (Async)"

### Test Results
```bash
# Press async write button
# Wait 4 seconds
# Press read button
# Result: "READ SUCCESS: 0 entries on layer 0"
# Expected: "READ SUCCESS: 1 entries on layer 0"
```

**Conclusion:** Async implementation keeps device responsive but write still doesn't persist.

## The Blocking Issue

### Core Problem
ESPHome is **single-threaded** - blocking the event loop freezes:
- WiFi management
- Home Assistant API communication
- BLE notification callbacks
- All user code

### What Python Does (Works)
```python
async with self._transaction_lock:
    self.clear_response_queue()          # Drain stale notifications
    await self.write(packet)             # Send request
    response = await self.read_response(timeout=3.0)  # Wait for ACK
    return response
```

### What We Do (Doesn't Work)
```cpp
register_response_handler(0xDE01, [](bytes) { ack_received = true; });
set_timeout(3000, [on_complete]() { on_complete(true); });
return;  // Returns immediately - ACK never consumed?
```

### Why It Might Fail
Possible issues:
1. Need to actively **poll/read** BLE notifications during wait (not just register handler)
2. Need **transaction locking** to prevent other operations during wait
3. Need **response queue management** to drain stale notifications before write
4. BLE notification callbacks might not fire if not actively consuming

## Next Steps for Future Agents

### 1. Research Required (HIGH PRIORITY)
See `RESEARCH_REQUEST_BLE_TRANSACTIONS.md` for detailed research brief.

**Key Questions:**
- How do other ESPHome BLE components implement request-response without blocking?
- What ESP-IDF patterns exist for non-blocking BLE transactions?
- Can we use `Component::loop()` with state machine to poll for responses?
- Do we need to actively consume BLE notifications in `loop()`?

**Resources:**
- ESPHome BLE components: `ble_client`, `xiaomi_ble`, `pvvx_mithermometer`, `bluetooth_proxy`
- ESP-IDF docs: BLE GATT Client API, FreeRTOS Event Groups
- ESPHome GitHub: https://github.com/esphome/esphome

### 2. Consider Full Transport Refactor (MEDIUM PRIORITY)
This problem likely requires extracting a proper transport layer per Section 6 of `AGENTS.md`:
- Create `core/transport.h/cpp` with transaction locking and response queue
- Implement non-blocking wait pattern (based on research findings)
- Align with Python reference implementation architecture

### 3. Test Python Reference (LOW PRIORITY)
Verify pump actually sends ACK response:
- Fix `test_python_schedule_write.py` import issues
- Run Python test to capture actual pump response
- Use BLE packet capture if needed (Wireshark/nRF Connect)

## Possible Solutions

### Pattern A: State Machine in loop()
```cpp
enum class OpState { IDLE, WAITING_RESPONSE };
OpState state_ = OpState::IDLE;
uint32_t response_deadline_ = 0;
bool ack_received_ = false;

void write_schedule_async() {
  send_packet();
  state_ = OpState::WAITING_RESPONSE;
  response_deadline_ = millis() + 3000;
}

void loop() override {
  if (state_ == OpState::WAITING_RESPONSE) {
    if (ack_received_ || millis() > response_deadline_) {
      state_ = OpState::IDLE;
      on_complete(ack_received_);
    }
  }
}
```

### Pattern B: Operation Queue
Queue write operations and process them one at a time in `loop()`:
```cpp
struct PendingOp {
  bytes packet;
  uint32_t deadline;
  std::function<void(bool)> callback;
};

std::queue<PendingOp> pending_ops_;

void loop() override {
  if (!pending_ops_.empty()) {
    auto& op = pending_ops_.front();
    if (response_received_ || millis() > op.deadline) {
      op.callback(response_received_);
      pending_ops_.pop();
    }
  }
}
```

## Key Learnings

1. **ESPHome is single-threaded** - never use `delay()` or blocking loops
2. **Async != Non-blocking wait** - registering callbacks isn't enough
3. **May need active polling** - BLE notifications might need to be consumed in `loop()`
4. **Python's asyncio is different** - `await` doesn't block the event loop, but our code does
5. **Architecture matters** - this likely needs proper transport layer refactor

## References

### Documentation
- `AGENTS.md` - Full project documentation (see Section 8 for challenge details)
- `RESEARCH_REQUEST_BLE_TRANSACTIONS.md` - Research brief for next agent
- Python reference: `reference/alpha-hwr/src/alpha_hwr/core/transport.py:415-485`

### Code
- Async write implementation: `components/alpha_hwr/schedule_service.cpp:485-598`
- Test button: `hwr-pump.yaml:149-190`

### Protocol
- Protocol docs: https://eman.github.io/alpha-hwr/reimplementation/
- Python reference: https://github.com/eman/alpha-hwr
- ESPHome BLE: https://esphome.io/components/ble_client/

## Questions for User

Before next steps, might be good to confirm:
1. Should we prioritize research into existing patterns first?
2. Or should we start implementing state machine in `loop()` based on Pattern A?
3. Is a full transport layer refactor acceptable at this stage?
