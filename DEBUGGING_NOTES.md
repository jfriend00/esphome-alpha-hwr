# Schedule Write Debugging Notes

## Current Status
- **Problem**: Schedule writes appear to succeed but don't persist
- **Symptom**: Write returns `true`, but immediate read-back shows 0 entries
- **Test**: Monday 06:00-08:00 entry written to layer 0, verified as 0 entries

## Investigation Steps Completed

### 1. Protocol Comparison ✓
- Compared C++ APDU structure with Python reference byte-by-byte
- **Result**: IDENTICAL structure
  ```
  [0x0A][0xB3][84][SubH][SubL][0x00][0xDE][0x01][0x00][0x00][0x2A][42 bytes payload]
  ```

### 2. Configuration Commit Investigation ✓
- Python `schedule.py` does NOT call `_send_configuration_commit()` after writes
- Only `control.py` and `time.py` use configuration commits
- **Result**: Configuration commit not required for schedules

### 3. Transaction Model Investigation ✓
- Python uses `transport.query()` which waits for response/timeout
- C++ uses fire-and-forget `write_packet()` 
- **Key Difference**: Python waits up to 3 seconds, C++ returns immediately
- **Hypothesis**: Pump may need acknowledgment before committing write

## Next: Python Reference Implementation Test

### Test Script
`test_python_schedule_write.py` - Tests if Python can write schedules to the pump

### Possible Outcomes

#### Scenario A: Python SUCCEEDS (Expected 1 entry, got 1 entry)
This means our C++ implementation has a protocol bug. Next steps:
1. Use Wireshark/btsnoop to capture Python BLE traffic
2. Capture ESP32 BLE traffic at same time
3. Compare packets byte-by-byte on the wire
4. Check for:
   - CRC calculation differences
   - Endianness issues
   - Frame structure differences
   - Missing/extra bytes

#### Scenario B: Python FAILS (Expected 1 entry, got 0 entries)
This means the pump requires something the Python code doesn't show. Investigate:
1. **Enable Schedule First**: Maybe schedule must be enabled before writing entries
   - Test: Enable schedule switch, THEN write entries
2. **Firmware Version**: Check if pump firmware differs from Python dev's pump
   - Read device info to get firmware version
3. **Hidden State**: Maybe pump needs to be in specific mode/state
   - Check operational mode, running state, etc.
4. **Timing Issue**: Maybe pump needs delay between write and commit
   - Test with longer delays (5s, 10s)
5. **Protocol Evolution**: Maybe protocol changed since Python was written
   - Check Python lib update dates vs pump firmware dates

#### Scenario C: Python Connection Fails
- Check if pump is still connected to ESP32
- Verify BLE is working on laptop
- Try manual disconnect: `bluetoothctl disconnect 3C:E0:02:50:98:BF`

## Additional Investigation Ideas

### If Both Fail: Check Schedule Enable State
```yaml
# In hwr-pump.yaml, test sequence:
1. Read schedule state (should show enable/disable)
2. Enable schedule if disabled
3. Write entry
4. Read back
```

### Capture BLE Traffic
```bash
# macOS BLE packet capture
# 1. Install Wireshark with BLE support
# 2. Enable Bluetooth HCI snoop log:
sudo log config --mode "level:debug" --subsystem com.apple.bluetooth
# 3. Capture to file
sudo log stream --predicate 'subsystem == "com.apple.bluetooth"' > ble_capture.log
```

### Compare with Control Service
Our control service writes (start/stop/mode) DO work and persist. Compare:
1. Control uses configuration commit, schedule doesn't
2. Control writes to different Object IDs
3. Both use OpSpec 0xB3 for SET operations

Maybe schedule writes need configuration commit despite Python not showing it?

## Code Locations

### C++ Implementation
- `components/alpha_hwr/schedule_service.cpp:389-470` - `write_entries()`
- `components/alpha_hwr/schedule_service.cpp:612-635` - `write_class10_command()`

### Python Reference
- `reference/alpha-hwr/src/alpha_hwr/services/schedule.py:344-527` - `write_entries()`
- `reference/alpha-hwr/src/alpha_hwr/services/schedule.py:880-931` - `_write_class10_command()`
- `reference/alpha-hwr/src/alpha_hwr/core/transport.py:415-485` - `query()` method

### Test Files
- `hwr-pump.yaml:149-191` - Write test button
- `hwr-pump.yaml:124-147` - Read test button
- `test_python_schedule_write.py` - Python validation script
