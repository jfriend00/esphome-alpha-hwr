# Schedule Write Issue - Root Cause Analysis

## Problem Summary
Schedule writes appear to succeed but don't persist to the pump. Write returns `true`, but immediate read-back shows 0 entries.

## Tests Performed

### Test 1: Basic Write ❌
- Wrote Monday 06:00-08:00 to layer 0
- Read back: 0 entries
- **Result**: Write doesn't persist

### Test 2: Enable Schedule First ❌  
- Enabled schedule before writing
- Wrote Monday 06:00-08:00
- Read back: 0 entries  
- **Result**: Enabling first doesn't help

### Test 3: Protocol Comparison ✅
- Compared C++ APDU byte-by-byte with Python reference
- **Result**: IDENTICAL structure (OpSpec 0xB3, Type 0xDE01, 42-byte payload)

## Key Findings

### 1. Read Operations Work Correctly ✅
- Read button consistently returns "READ SUCCESS: 0 entries"
- This proves:
  - Pump IS responding to our read requests
  - Our callback system IS working
  - Response parsing IS correct
  - The pump simply has NO entries stored

### 2. Write Packet Structure is Correct ✅
Our APDU matches Python exactly:
```
[0x0A][0xB3][84][SubH][SubL][0x00][0xDE][0x01][0x00][0x00][0x2A][42 bytes]
```

### 3. Critical Difference: Transaction Model ⚠️

**Python Implementation** (`schedule.py:520`):
```python
success = await self._write_class10_command(0xE7, 0xF8, bytes(apdu))
```

Which calls (`schedule.py:913`):
```python
response = await self.transport.query(frame, timeout=3.0)
```

**Key Behavior**: 
- `query()` **BLOCKS** until response received OR 3-second timeout
- Python treats BOTH response AND timeout as success
- Transaction is atomic: send → wait → return

**C++ Implementation** (`schedule_service.cpp:464`):
```cpp
if (!this->write_class10_command(apdu, 53)) {
  ESP_LOGE(TAG, "Failed to write schedule to layer %d", layer);
  return false;
}
```

Which calls (`schedule_service.cpp:626-634`):
```cpp
// Write frame via callback
if (!this->write_callback_(0x00, frame, frame_len)) {
  ESP_LOGE(TAG, "Failed to write Class 10 command");
  return false;
}

// For write operations, we typically don't wait for a response
// The pump may or may not send an acknowledgment
ESP_LOGD(TAG, "Class 10 write command sent successfully");
return true;
```

**Key Behavior**:
- Fire-and-forget: send packet → return immediately
- NO waiting for response or acknowledgment
- Comment explicitly states we don't wait

## Root Cause Hypothesis

The pump likely uses a **two-phase commit** for schedule writes:

1. **Phase 1**: Receive write packet → buffer in RAM → send ACK
2. **Phase 2**: On receiving ACK request → commit buffer to flash/EEPROM

Our C++ code sends the write but never waits for the ACK, so the pump:
- Accepts the write into a buffer
- Waits for us to read the ACK
- We never read it
- Pump discards the buffered write
- Next read shows 0 entries

This is similar to how databases work with transactions:
```
BEGIN TRANSACTION
  INSERT INTO schedule VALUES (...)
COMMIT  ← We're missing this step!
```

## Supporting Evidence

1. **Control Service Works**: Our start/stop/mode changes DO persist, and control.py uses `_send_configuration_commit()` after writes
2. **Read Callbacks Fire**: Proves our async callback system works
3. **No Errors**: Pump accepts our packets without error responses
4. **Python Uses query()**: Explicitly waits for transaction completion

## Next Steps

### Option A: Implement Proper Transaction Handling ⭐ **RECOMMENDED**
Modify `write_class10_command()` to wait for acknowledgment:

```cpp
bool ScheduleService::write_class10_command(const uint8_t *apdu, size_t apdu_len) {
  // Register ACK handler BEFORE sending
  bool ack_received = false;
  this->transport_.register_response_handler(0xDE01, 0,
    [&ack_received](const uint8_t* data, size_t len) {
      ESP_LOGI(TAG, "Write ACK received (%zu bytes)", len);
      ack_received = true;
    }
  );
  
  // Send frame
  if (!this->write_callback_(0x00, frame, frame_len)) {
    return false;
  }
  
  // Wait for ACK (up to 3 seconds, matching Python)
  uint32_t start = millis();
  while (!ack_received && (millis() - start < 3000)) {
    delay(10);  // Yield to allow callback processing
  }
  
  if (ack_received) {
    ESP_LOGI(TAG, "Write transaction completed with ACK");
  } else {
    ESP_LOGW(TAG, "Write transaction timeout (treating as success per Python behavior)");
  }
  
  return true;  // Match Python: treat both ACK and timeout as success
}
```

**Concerns**:
- `delay()` and `millis()` in ESPHome may block event loop
- Need to ensure callbacks can fire during wait loop

**Better Approach**: Use ESPHome's async patterns
- Convert `write_entries()` to use completion callbacks
- Button lambda waits via ESPHome delay actions
- More complex but proper for ESPHome architecture

### Option B: Test Python Implementation
- Disconnect ESP32
- Run `test_python_schedule_write.py`
- If Python ALSO fails → different root cause (firmware version, pump state, etc.)
- If Python succeeds → confirms our transaction handling is the issue

### Option C: BLE Packet Capture
- Use Wireshark or btsnoop
- Capture Python write (working)
- Capture ESP32 write (not working)
- Compare actual packets on the wire
- Look for differences in timing, responses, or packet structure

## Implementation Priority

1. **First**: Test Python implementation (quickest validation)
2. **Second**: If Python works, implement proper transaction handling in C++
3. **Third**: If both fail, investigate pump state/firmware requirements

## Related Files

- `components/alpha_hwr/schedule_service.cpp:389-470` - `write_entries()`
- `components/alpha_hwr/schedule_service.cpp:612-635` - `write_class10_command()`
- `reference/alpha-hwr/src/alpha_hwr/services/schedule.py:880-931` - Python `_write_class10_command()`
- `reference/alpha-hwr/src/alpha_hwr/core/transport.py:415-485` - Python `query()` method

## Decision Point

**Question**: Should we implement blocking transaction wait in C++, or keep it async with callbacks?

**ESPHome Best Practice**: Async with callbacks (non-blocking)
**Python Reference**: Blocking (but uses asyncio, so non-blocking in practice)
**Trade-off**: Complexity vs. correctness

**Recommendation**: Start with simple blocking wait to validate hypothesis, then refactor to async if it works.
