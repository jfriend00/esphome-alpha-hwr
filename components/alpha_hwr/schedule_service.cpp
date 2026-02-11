/**
 * Schedule Service Implementation for Grundfos ALPHA HWR Pump
 *
 * Implements schedule management operations including reading, writing, enabling/disabling,
 * and validation of weekly pump schedules.
 *
 * Based on: reference/alpha-hwr/src/alpha_hwr/services/schedule.py (931 lines)
 */

#include "schedule_service.h"
#include "codec.h"
#include "session.h"
#include "transport.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>
#include <map>

namespace esphome {
namespace alpha_hwr {
namespace services {

static const char *TAG = "schedule_service";

// Day names for parsing schedule entries
static const char *DAY_NAMES[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

ScheduleService::ScheduleService(core::Transport &transport, core::Session &session)
    : transport_(transport),
      session_(session),
      schedule_callback_(nullptr),
      write_callback_(nullptr),
      schedule_state_cached_(false),
      schedule_enabled_(false),
      last_state_poll_ms_(0) {
  ESP_LOGD(TAG, "ScheduleService initialized");
}

// -------------------------------------------------------------------------
// Schedule State Operations
// -------------------------------------------------------------------------

bool ScheduleService::get_state(bool *result) {
  if (!result) {
    ESP_LOGE(TAG, "get_state() called with null result pointer");
    return false;
  }

  if (!this->schedule_state_cached_) {
    ESP_LOGV(TAG, "Schedule state not cached yet");
    return false;
  }

  *result = this->schedule_enabled_;
  return true;
}

bool ScheduleService::poll_state() {
  if (!this->session_.is_ready()) {
    ESP_LOGV(TAG, "Cannot poll schedule state: session not ready");
    return false;
  }

  ESP_LOGD(TAG, "Polling schedule state (Object 84, SubID 1)...");

  // Build Class 10 READ request for Object 84, SubID 1 (ClockProgramOverview)
  // APDU: [0x0A][0x03][ObjID][SubID_H][SubID_L]
  uint8_t apdu[5];
  apdu[0] = 0x0A;   // Class 10
  apdu[1] = 0x03;   // OpSpec INFO (read)
  apdu[2] = 84;     // Object ID
  apdu[3] = 0x00;   // SubID high byte
  apdu[4] = 0x01;   // SubID low byte (SubID = 1)

  // Build GENI frame
  uint8_t frame[256];
  size_t frame_len = 0;
  this->build_geni_frame(0xE7, 0xF8, apdu, 5, frame, &frame_len);

  // Register response handler before sending request
  // IMPORTANT: Pump responds with SubID 0, not SubID 1 that we requested!
  // This appears to be a quirk of the ALPHA HWR firmware.
  // Capture 'this' pointer to access member variables in callback
  this->transport_.register_response_handler(0xDA01, 0,  // Type 218 (ClockProgramOverview), SubID 0 
    [this](const uint8_t* payload, size_t payload_len) {
      // ClockProgramOverview structure (10 bytes total):
      // Response frame: [STX][LEN][DST][SRC][Class][OpSpec][ObjID][SubID_H][SubID_L][...DATA...][CRC_H][CRC_L]
      // Payload starts at byte 9 (after [STX][LEN][DST][SRC][Class][OpSpec][ObjID][SubID_H][SubID_L])
      // 
      // Payload structure (matches Python reference):
      // Byte 0: max_nof_actions
      // Byte 1: max_nof_single_events
      // Byte 2: max_nof_alternative_events_per_day
      // Byte 3: max_nof_events_per_day
      // Byte 4: unknown (reserved?)
      // Byte 5: unknown (reserved?)
      // Byte 6: unknown (reserved?)
      // Byte 7: clock_program_enabled ← THIS IS WHAT WE WANT (matches Python line 147)
      // Byte 8: default_action
      // Bytes 9-12: base_set_point (float32, big-endian)
      
      ESP_LOGV(TAG, "Schedule state response received (%zu bytes)", payload_len);
      
      if (payload_len >= 8) {
        this->schedule_enabled_ = (payload[7] != 0);  // Changed from payload[4] to payload[7]
        this->schedule_state_cached_ = true;
        ESP_LOGD(TAG, "Schedule state updated: %s (byte 7 = 0x%02X)", 
                 this->schedule_enabled_ ? "enabled" : "disabled", payload[7]);
      } else {
        ESP_LOGW(TAG, "Schedule state response too short (%zu bytes, need at least 8)", payload_len);
      }
    }
  );

  // Send request via transport
  if (!this->transport_.write_packet(frame, frame_len, 
      [this](const uint8_t* data, size_t len) -> bool {
        return this->write_callback_(0x00, data, len);
      })) {
    ESP_LOGE(TAG, "Failed to send schedule state read request");
    return false;
  }

  this->last_state_poll_ms_ = millis();
  ESP_LOGD(TAG, "Schedule state poll request sent");
  return true;
}

bool ScheduleService::enable() { return this->set_state(true); }

bool ScheduleService::disable() { return this->set_state(false); }

bool ScheduleService::set_state(bool enable) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot set schedule state: session not ready");
    return false;
  }

  ESP_LOGI(TAG, "%s schedule...", enable ? "Enabling" : "Disabling");

  // NOTE: Simplified implementation without read-modify-write
  // Uses hardcoded default values from ALPHA HWR pump specification
  // Full implementation with read-modify-write will be added in Phase 8
  
  // ClockProgramOverview structure (10 bytes):
  // Byte 0: max_nof_actions = 140 (0x8C)
  // Byte 1: max_nof_single_events = 35 (0x23)
  // Byte 2: max_nof_alternative_events_per_day = 5 (0x05)
  // Byte 3: max_nof_events_per_day = 5 (0x05)
  // Byte 4: clock_program_enabled = enable ? 0x01 : 0x00
  // Byte 5: default_action = 1 (START)
  // Bytes 6-9: base_set_point = 0.0 (0x00000000)
  uint8_t structure_bytes[10] = {
    0x8C,                     // max_nof_actions
    0x23,                     // max_nof_single_events
    0x05,                     // max_nof_alternative_events_per_day
    0x05,                     // max_nof_events_per_day
    enable ? (uint8_t)0x01 : (uint8_t)0x00,  // clock_program_enabled
    0x01,                     // default_action
    0x00, 0x00, 0x00, 0x00    // base_set_point
  };

  // Build APDU: Class 10 SET command for Object 84, SubID 1
  // OpSpec 0x93 = OpSpec 4 (SET), Length 19
  uint8_t apdu[21];
  apdu[0] = 0x0A;    // Class 10
  apdu[1] = 0x93;    // OpSpec 4, Length 19
  apdu[2] = 84;      // Object 84 (decimal)
  apdu[3] = 0x00;    // SubID high byte
  apdu[4] = 0x01;    // SubID low byte (SubID = 1)
  apdu[5] = 0x00;    // Reserved
  apdu[6] = 0xDA;    // Type 218 (ClockProgramOverview)
  apdu[7] = 0x01;    // Type continued
  apdu[8] = 0x00;    // Type continued
  apdu[9] = 0x00;    // Size high byte
  apdu[10] = 0x0A;   // Size low byte (10 bytes)
  memcpy(apdu + 11, structure_bytes, 10);

  // Write command
  if (!this->write_class10_command(apdu, 21)) {
    ESP_LOGE(TAG, "Failed to write schedule enable/disable command");
    return false;
  }

  ESP_LOGI(TAG, "Schedule %s command sent", enable ? "enable" : "disable");
  return true;
}

// -------------------------------------------------------------------------
// Schedule Entry Operations
// -------------------------------------------------------------------------

bool ScheduleService::read_entries(std::vector<ScheduleEntry> *entries, int layer) {
  // TODO: Implement async read operation
  // For now, return false to indicate not implemented
  ESP_LOGW(TAG, "read_entries() not yet implemented - requires async read support");
  entries->clear();
  return false;
}

bool ScheduleService::write_entries(const std::vector<ScheduleEntry> &entries, uint8_t layer) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot write schedule entries: session not ready");
    return false;
  }

  // Validate layer
  if (layer > 4) {
    ESP_LOGE(TAG, "Invalid layer: %d. Must be 0-4.", layer);
    return false;
  }

  // Validate entries
  std::vector<std::string> errors;
  if (!this->validate_entries(entries, &errors)) {
    ESP_LOGE(TAG, "Schedule validation failed:");
    for (const auto &error : errors) {
      ESP_LOGE(TAG, "  %s", error.c_str());
    }
    return false;
  }

  ESP_LOGI(TAG, "Writing %zu schedule entries to layer %d...", entries.size(), layer);

  // Prepare 42-byte payload (7 days × 6 bytes)
  uint8_t payload_data[42];
  memset(payload_data, 0, 42);  // Initialize with zeros (disabled entries)

  // Map day names to indices
  std::map<std::string, int> day_indices;
  for (int i = 0; i < 7; i++) {
    day_indices[DAY_NAMES[i]] = i;
  }

  // Fill payload with entries
  for (const auto &entry : entries) {
    auto it = day_indices.find(entry.get_day());
    if (it == day_indices.end()) {
      ESP_LOGW(TAG, "Invalid day name in entry: %s", entry.get_day().c_str());
      continue;
    }

    int day_idx = it->second;
    size_t offset = day_idx * 6;

    // Fill 6-byte entry
    entry.to_bytes(payload_data + offset);

    ESP_LOGD(TAG, "Added entry for %s at offset %zu: %02X %02X %02X %02X %02X %02X", entry.get_day().c_str(), offset,
             payload_data[offset], payload_data[offset + 1], payload_data[offset + 2], payload_data[offset + 3],
             payload_data[offset + 4], payload_data[offset + 5]);
  }

  // Build APDU
  uint16_t sub_id = 1000 + layer;
  uint8_t sub_h = (sub_id >> 8) & 0xFF;
  uint8_t sub_l = sub_id & 0xFF;

  uint8_t apdu[53];  // 11 header bytes + 42 data bytes
  apdu[0] = 0x0A;    // Class 10
  apdu[1] = 0xB3;    // OpSpec 5
  apdu[2] = 84;      // Object ID 84
  apdu[3] = sub_h;   // SubID high byte
  apdu[4] = sub_l;   // SubID low byte
  apdu[5] = 0x00;    // Reserved
  apdu[6] = 0xDE;    // Type 222 header
  apdu[7] = 0x01;
  apdu[8] = 0x00;
  apdu[9] = 0x00;   // Size high byte
  apdu[10] = 0x2A;  // Size low byte (42 bytes)

  // Append payload
  memcpy(apdu + 11, payload_data, 42);

  // Send write command
  if (!this->write_class10_command(apdu, 53)) {
    ESP_LOGE(TAG, "Failed to write schedule to layer %d", layer);
    return false;
  }

  ESP_LOGI(TAG, "Schedule written successfully to layer %d", layer);
  return true;
}

bool ScheduleService::clear_entry(const std::string &day, uint8_t layer) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot clear schedule entry: session not ready");
    return false;
  }

  // Validate day
  bool valid_day = false;
  for (const char *valid : DAY_NAMES) {
    if (day == valid) {
      valid_day = true;
      break;
    }
  }
  if (!valid_day) {
    ESP_LOGE(TAG, "Invalid day name: %s", day.c_str());
    return false;
  }

  // Validate layer
  if (layer > 4) {
    ESP_LOGE(TAG, "Invalid layer: %d. Must be 0-4.", layer);
    return false;
  }

  ESP_LOGI(TAG, "Clearing schedule entry for %s on layer %d...", day.c_str(), layer);

  // Read current schedule for this layer
  std::vector<ScheduleEntry> entries;
  if (!this->read_entries(&entries, layer)) {
    ESP_LOGE(TAG, "Failed to read current schedule for layer %d", layer);
    return false;
  }

  // Filter out the entry for the specified day
  std::vector<ScheduleEntry> filtered_entries;
  for (const auto &entry : entries) {
    if (entry.get_day() != day) {
      filtered_entries.push_back(entry);
    }
  }

  // Write back the filtered entries
  return this->write_entries(filtered_entries, layer);
}

// -------------------------------------------------------------------------
// Validation Methods
// -------------------------------------------------------------------------

bool ScheduleService::validate_entries(const std::vector<ScheduleEntry> &entries, std::vector<std::string> *errors) {
  errors->clear();

  // Validate each entry's time range
  for (size_t i = 0; i < entries.size(); i++) {
    const auto &entry = entries[i];

    // Check valid day name
    bool valid_day = false;
    for (const char *day : DAY_NAMES) {
      if (entry.get_day() == day) {
        valid_day = true;
        break;
      }
    }
    if (!valid_day) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Entry %zu: Invalid day name '%s'", i, entry.get_day().c_str());
      errors->push_back(std::string(buf));
    }

    // Check valid layer
    if (entry.get_layer() > 4) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Entry %zu (%s): Invalid layer %d (must be 0-4)", i, entry.get_day().c_str(),
               entry.get_layer());
      errors->push_back(std::string(buf));
    }

    // Check valid time range
    std::string error_msg;
    if (!entry.is_valid_time_range(&error_msg)) {
      char buf[256];
      snprintf(buf, sizeof(buf), "Entry %zu (%s %s-%s): %s", i, entry.get_day().c_str(),
               entry.get_begin_time().c_str(), entry.get_end_time().c_str(), error_msg.c_str());
      errors->push_back(std::string(buf));
    }
  }

  // Check for overlaps - only compare enabled entries
  for (size_t i = 0; i < entries.size(); i++) {
    const auto &entry1 = entries[i];
    if (!entry1.is_enabled())
      continue;

    for (size_t j = i + 1; j < entries.size(); j++) {
      const auto &entry2 = entries[j];
      if (!entry2.is_enabled())
        continue;

      if (entry1.overlaps_with(entry2)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Overlap detected: %s layer %d: %s-%s overlaps with %s-%s", entry1.get_day().c_str(),
                 entry1.get_layer(), entry1.get_begin_time().c_str(), entry1.get_end_time().c_str(),
                 entry2.get_begin_time().c_str(), entry2.get_end_time().c_str());
        errors->push_back(std::string(buf));
      }
    }
  }

  // Check layer counts (warn if too many entries per day/layer)
  std::map<std::pair<std::string, int>, int> day_layer_counts;
  for (const auto &entry : entries) {
    if (entry.is_enabled()) {
      auto key = std::make_pair(entry.get_day(), entry.get_layer());
      day_layer_counts[key]++;
    }
  }

  for (const auto &kv : day_layer_counts) {
    if (kv.second > 10) {  // Arbitrary high limit
      char buf[256];
      snprintf(buf, sizeof(buf), "Warning: %s layer %d has %d entries. This may exceed pump capacity.",
               kv.first.first.c_str(), kv.first.second, kv.second);
      errors->push_back(std::string(buf));
    }
  }

  return errors->empty();
}

// -------------------------------------------------------------------------
// Internal Helper Methods
// -------------------------------------------------------------------------

// NOTE: read_class10_object() removed - requires async implementation
// For Phase 7, only write operations (enable/disable/write_entries) are supported
// Read operations (get_state/read_entries) will be added in Phase 8

bool ScheduleService::write_class10_command(const uint8_t *apdu, size_t apdu_len) {
  if (!this->write_callback_) {
    ESP_LOGE(TAG, "Write callback not configured");
    return false;
  }

  // Build GENI frame
  uint8_t frame[256];
  size_t frame_len = 0;
  this->build_geni_frame(0xE7, 0xF8, apdu, apdu_len, frame, &frame_len);

  ESP_LOGD(TAG, "Writing Class 10 command (%zu bytes)", frame_len);

  // Write frame via callback
  if (!this->write_callback_(0x00, frame, frame_len)) {
    ESP_LOGE(TAG, "Failed to write Class 10 command");
    return false;
  }

  // For write operations, we typically don't wait for a response
  // The pump may or may not send an acknowledgment
  ESP_LOGD(TAG, "Class 10 write command sent successfully");
  return true;
}

void ScheduleService::build_geni_frame(uint8_t dst, uint8_t src, const uint8_t *apdu, size_t apdu_len, uint8_t *frame,
                                        size_t *frame_len) {
  // Build GENI frame: [Start][Length][Dst][Src][APDU...][CRC_H][CRC_L]
  uint8_t length = 1 + 1 + apdu_len;  // Dst + Src + APDU

  frame[0] = 0x27;  // Start byte (request frame)
  frame[1] = length;
  frame[2] = dst;
  frame[3] = src;
  memcpy(frame + 4, apdu, apdu_len);

  // Calculate CRC (excludes start byte, includes everything else)
  uint16_t crc = protocol::calc_crc16_read(frame + 1, length + 1);
  frame[4 + apdu_len] = (crc >> 8) & 0xFF;      // CRC high byte
  frame[4 + apdu_len + 1] = crc & 0xFF;         // CRC low byte

  *frame_len = 4 + apdu_len + 2;  // Start + Length + Dst + Src + APDU + CRC
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
