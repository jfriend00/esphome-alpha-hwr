#include "control_service.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace services {

static const char *TAG = "alpha_hwr.control";

// Class 10 Control Mode Mapping
// Maps ControlMode values to (ModeByte, SuffixBytes)
// Based on protocol specification in control.py lines 137-145
const ControlService::ControlModeMapping ControlService::CLASS10_CONTROL_MAP[] = {
    {0x00, {0x45, 0x65, 0x70, 0x00}},  // CONSTANT_PRESSURE (0)
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // PROPORTIONAL_PRESSURE (1) - not supported
    {0x02, {0x45, 0x65, 0x70, 0x00}},  // CONSTANT_SPEED (2)
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (3) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (4) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // AUTO_ADAPT (5) - not supported in Class 10
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (6) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (7) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // CONSTANT_FLOW (8) - not supported in Class 10
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (9) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (10) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (11) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (12) - unused
    {0x0D, {0x45, 0x65, 0x70, 0x00}},  // AUTO_ADAPT_RADIATOR (13)
    {0x0E, {0x45, 0x65, 0x70, 0x00}},  // AUTO_ADAPT_UNDERFLOOR (14)
    {0x0F, {0x45, 0x65, 0x70, 0x00}},  // AUTO_ADAPT_COMBINED (15)
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (16-24) - unused entries
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x19, {0x38, 0xC6, 0x70, 0x00}},  // DHW_ON_OFF (25)
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (26) - unused
    {0x1B, {0x39, 0x67, 0x70, 0x00}},  // TEMPERATURE_RANGE (27)
};

ControlService::ControlService(core::Transport &transport, core::Session &session)
    : transport_(transport), session_(session) {
  ESP_LOGI(TAG, "ControlService initialized");
}

void ControlService::set_schedule_callback(std::function<void(std::function<void()>, uint32_t)> callback) {
  schedule_callback_ = callback;
}

void ControlService::set_write_callback(std::function<bool(const uint8_t*, size_t)> callback) {
  write_callback_ = callback;
}

bool ControlService::start(uint8_t mode) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot start pump: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    return false;
  }

  ESP_LOGI(TAG, "Starting pump...");

  // Resolve target mode (255 = use current mode)
  uint8_t target_mode = (mode == 255) ? static_cast<uint8_t>(current_mode_) : mode;
  
  // Get Class 10 mapping for this mode
  ControlModeMapping mapping;
  if (!get_class10_mapping(static_cast<ControlMode>(target_mode), mapping)) {
    ESP_LOGE(TAG, "Mode %d not supported for Class 10 start", target_mode);
    return false;
  }

  // Build payload: [Header] [00=Run] [Mode] [Suffix]
  // Reference: control.py lines 210-216
  uint8_t payload[12];
  payload[0] = 0x2F;
  payload[1] = 0x01;
  payload[2] = 0x00;
  payload[3] = 0x00;
  payload[4] = 0x07;
  payload[5] = 0x00;
  payload[6] = 0x00;  // Flag: 0x00 = Start/Run
  payload[7] = mapping.mode_byte;
  memcpy(&payload[8], mapping.suffix, 4);

  // Build Class 10 SET packet
  // APDU: [0x0A][0x90][Sub-H][Sub-L][Obj-H][Obj-L][Payload...]
  // Sub 0x5600, Obj 0x0601
  // Reference: control.py lines 218-220
  uint8_t apdu[18];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x90;  // OpSpec: SET with length 16 (0x80 | 0x10)
  apdu[2] = 0x56;  // Sub ID high
  apdu[3] = 0x00;  // Sub ID low
  apdu[4] = 0x06;  // Obj ID high
  apdu[5] = 0x01;  // Obj ID low
  memcpy(&apdu[6], payload, 12);

  // Build GENI frame
  uint8_t packet[64];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 18, packet);

  // Send packet
  if (!write_callback_ || !write_callback_(packet, packet_len)) {
    ESP_LOGE(TAG, "Failed to write start command packet");
    return false;
  }

  // Schedule configuration commit (after 200ms delay)
  // Reference: control.py lines 223-226
  if (schedule_callback_) {
    schedule_callback_([this]() { this->send_configuration_commit(); }, 200);
  }

  // Update current mode if a specific mode was requested
  if (mode != 255) {
    current_mode_ = static_cast<ControlMode>(target_mode);
  }

  ESP_LOGI(TAG, "Pump start command sent (mode=%d)", target_mode);
  return true;
}

bool ControlService::stop(uint8_t mode) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot stop pump: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    return false;
  }

  ESP_LOGI(TAG, "Stopping pump...");

  // Resolve target mode (255 = use current mode)
  uint8_t target_mode = (mode == 255) ? static_cast<uint8_t>(current_mode_) : mode;
  
  // Get Class 10 mapping for this mode
  ControlModeMapping mapping;
  if (!get_class10_mapping(static_cast<ControlMode>(target_mode), mapping)) {
    ESP_LOGE(TAG, "Mode %d not supported for Class 10 stop", target_mode);
    return false;
  }

  // Build payload: [Header] [01=Stop] [Mode] [Suffix]
  // Reference: control.py lines 275-280
  uint8_t payload[12];
  payload[0] = 0x2F;
  payload[1] = 0x01;
  payload[2] = 0x00;
  payload[3] = 0x00;
  payload[4] = 0x07;
  payload[5] = 0x00;
  payload[6] = 0x01;  // Flag: 0x01 = Stop
  payload[7] = mapping.mode_byte;
  memcpy(&payload[8], mapping.suffix, 4);

  // Build Class 10 SET packet
  // APDU: [0x0A][0x90][Sub-H][Sub-L][Obj-H][Obj-L][Payload...]
  // Reference: control.py lines 282-285
  uint8_t apdu[18];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x90;  // OpSpec: SET with length 16 (0x80 | 0x10)
  apdu[2] = 0x56;  // Sub ID high
  apdu[3] = 0x00;  // Sub ID low
  apdu[4] = 0x06;  // Obj ID high
  apdu[5] = 0x01;  // Obj ID low
  memcpy(&apdu[6], payload, 12);

  // Build GENI frame
  uint8_t packet[64];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 18, packet);

  // Send packet
  if (!write_callback_ || !write_callback_(packet, packet_len)) {
    ESP_LOGE(TAG, "Failed to write stop command packet");
    return false;
  }

  // Schedule configuration commit (after 200ms delay)
  // Reference: control.py lines 290-293
  if (schedule_callback_) {
    schedule_callback_([this]() { this->send_configuration_commit(); }, 200);
  }

  ESP_LOGI(TAG, "Pump stop command sent (mode=%d)", target_mode);

  return true;
}

bool ControlService::set_mode(ControlMode mode) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot set mode: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    return false;
  }

  uint8_t mode_val = static_cast<uint8_t>(mode);
  ESP_LOGI(TAG, "Setting control mode to %d (%s)...", mode_val, get_mode_name(mode));

  // Try Class 10 first
  // Reference: control.py lines 388-408
  ControlModeMapping mapping;
  if (get_class10_mapping(mode, mapping)) {
    // Build payload: [Header] [00=Run] [Mode] [Suffix]
    uint8_t payload[12];
    payload[0] = 0x2F;
    payload[1] = 0x01;
    payload[2] = 0x00;
    payload[3] = 0x00;
    payload[4] = 0x07;
    payload[5] = 0x00;
    payload[6] = 0x00;  // Flag: 0x00 = Run
    payload[7] = mapping.mode_byte;
    memcpy(&payload[8], mapping.suffix, 4);

    // Build Class 10 SET packet
    uint8_t apdu[18];
    apdu[0] = 0x0A;  // Class 10
    apdu[1] = 0x90;  // OpSpec: SET with length 16 (0x80 | 0x10)
    apdu[2] = 0x56;  // Sub ID high
    apdu[3] = 0x00;  // Sub ID low
    apdu[4] = 0x06;  // Obj ID high
    apdu[5] = 0x01;  // Obj ID low
    memcpy(&apdu[6], payload, 12);

  // Build GENI frame
  uint8_t packet[64];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 18, packet);

  // Send packet
  if (!write_callback_ || !write_callback_(packet, packet_len)) {
    ESP_LOGE(TAG, "Failed to write start command packet");
    return false;
  }
    
    current_mode_ = mode;
    ESP_LOGI(TAG, "Mode set to %s (Class 10)", get_mode_name(mode));
    return true;
  }

  // Fallback to Class 3 for unsupported modes
  // Reference: control.py lines 410-434
  ESP_LOGD(TAG, "Mode %d not in Class 10 map, trying Class 3...", mode_val);

  // Class 3 command ID mapping
  // Reference: control.py lines 413-422
  uint8_t cmd_id = 0;
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:
      cmd_id = 0x18;
      break;
    case ControlMode::PROPORTIONAL_PRESSURE:
      cmd_id = 0x17;
      break;
    case ControlMode::CONSTANT_SPEED:
      cmd_id = 0x04;
      break;
    case ControlMode::AUTO_ADAPT:
      cmd_id = 0x06;
      break;
    case ControlMode::CONSTANT_FLOW:
      cmd_id = 0x15;
      break;
    case ControlMode::AUTO_ADAPT_RADIATOR:
      cmd_id = 0x1E;
      break;
    case ControlMode::AUTO_ADAPT_UNDERFLOOR:
      cmd_id = 0x1F;
      break;
    case ControlMode::AUTO_ADAPT_COMBINED:
      cmd_id = 0x20;
      break;
    default:
      ESP_LOGE(TAG, "Unsupported control mode: %d", mode_val);
      return false;
  }

  // Build Class 3 command: [0x03, 0xC1, cmd_id]
  // Reference: control.py line 429 (uses FrameBuilder.build_command_info)
  uint8_t apdu[3];
  apdu[0] = 0x03;  // Class 3
  apdu[1] = 0xC1;  // WRITE command
  apdu[2] = cmd_id;

  // Build GENI frame
  uint8_t packet[32];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 3, packet);

  // Send packet
  if (!write_callback_ || !write_callback_(packet, packet_len)) {
    ESP_LOGE(TAG, "Failed to write mode command packet (Class 3)");
    return false;
  }
  
  current_mode_ = mode;
  ESP_LOGI(TAG, "Mode set to %s (Class 3)", get_mode_name(mode));
  return true;
}

bool ControlService::enable_remote_mode() {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot enable remote mode: session not ready");
    return false;
  }

  ESP_LOGI(TAG, "Enabling remote mode...");

  // Class 3: 03 C1 07
  // Reference: control.py lines 329-332
  uint8_t apdu[3] = {0x03, 0xC1, 0x07};
  
  uint8_t packet[32];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 3, packet);
  
  if (!write_callback_ || !write_callback_(packet, packet_len)) {
    ESP_LOGE(TAG, "Failed to write enable remote command");
    return false;
  }
  
  ESP_LOGI(TAG, "Remote mode enabled");
  return true;
}

bool ControlService::disable_remote_mode() {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot disable remote mode: session not ready");
    return false;
  }

  ESP_LOGI(TAG, "Disabling remote mode (Auto)...");

  // Class 3: 03 C1 06
  // Reference: control.py lines 358-361
  uint8_t apdu[3] = {0x03, 0xC1, 0x06};
  
  uint8_t packet[32];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 3, packet);
  
  if (!write_callback_ || !write_callback_(packet, packet_len)) {
    ESP_LOGE(TAG, "Failed to write disable remote command");
    return false;
  }
  
  ESP_LOGI(TAG, "Remote mode disabled (Auto)");
  return true;
}

const char *ControlService::get_mode_name(ControlMode mode) {
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:
      return "CONSTANT_PRESSURE";
    case ControlMode::PROPORTIONAL_PRESSURE:
      return "PROPORTIONAL_PRESSURE";
    case ControlMode::CONSTANT_SPEED:
      return "CONSTANT_SPEED";
    case ControlMode::AUTO_ADAPT:
      return "AUTO_ADAPT";
    case ControlMode::CONSTANT_FLOW:
      return "CONSTANT_FLOW";
    case ControlMode::AUTO_ADAPT_RADIATOR:
      return "AUTO_ADAPT_RADIATOR";
    case ControlMode::AUTO_ADAPT_UNDERFLOOR:
      return "AUTO_ADAPT_UNDERFLOOR";
    case ControlMode::AUTO_ADAPT_COMBINED:
      return "AUTO_ADAPT_COMBINED";
    case ControlMode::DHW_ON_OFF:
      return "DHW_ON_OFF";
    case ControlMode::TEMPERATURE_RANGE:
      return "TEMPERATURE_RANGE";
    case ControlMode::NONE:
      return "NONE";
    default:
      return "UNKNOWN";
  }
}

size_t ControlService::build_geni_packet(uint8_t source, uint8_t service_id,
                                          const uint8_t *apdu, size_t apdu_len,
                                          uint8_t *packet_out) {
  // Frame format: [STX][LEN][ServiceID][Source][APDU][CRC-H][CRC-L]
  // Reference: base.py::_build_geni_packet() lines 181-214
  
  size_t length = 1 + 1 + apdu_len;  // ServiceID + Source + APDU
  
  packet_out[0] = 0x27;  // STX (FRAME_START)
  packet_out[1] = static_cast<uint8_t>(length);
  packet_out[2] = service_id;
  packet_out[3] = source;
  memcpy(&packet_out[4], apdu, apdu_len);
  
  // Calculate CRC over [LEN][ServiceID][Source][APDU]
  uint16_t crc = protocol::calc_crc16(&packet_out[1], length + 1);
  
  packet_out[4 + apdu_len] = (crc >> 8) & 0xFF;
  packet_out[5 + apdu_len] = crc & 0xFF;
  
  return 4 + apdu_len + 2;  // STX + LEN + ServiceID + Source + APDU + CRC
}

void ControlService::send_configuration_commit() {
  // Sub 0x5400, Obj 0xDA01
  // Reference: control.py lines 1038-1048
  ESP_LOGD(TAG, "Sending configuration commit...");
  
  uint8_t apdu[21];
  // Hardcoded hex from Python: 0A9354000100DA0100000A02050005000100000000
  const uint8_t commit_data[] = {
      0x0A, 0x93, 0x54, 0x00, 0x01, 0x00, 0xDA, 0x01,
      0x00, 0x00, 0x0A, 0x02, 0x05, 0x00, 0x05, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x00
  };
  memcpy(apdu, commit_data, 21);
  
  uint8_t packet[64];
  size_t packet_len = build_geni_packet(0xF8, 0xE7, apdu, 21, packet);
  
  if (write_callback_ && write_callback_(packet, packet_len)) {
    ESP_LOGD(TAG, "Configuration commit sent");
  } else {
    ESP_LOGW(TAG, "Failed to write configuration commit");
  }
}

bool ControlService::get_class10_mapping(ControlMode mode, ControlModeMapping &mapping) {
  uint8_t mode_val = static_cast<uint8_t>(mode);
  
  // Check if mode is in valid range and has non-zero mode byte
  if (mode_val >= sizeof(CLASS10_CONTROL_MAP) / sizeof(CLASS10_CONTROL_MAP[0])) {
    return false;
  }
  
  const ControlModeMapping &map_entry = CLASS10_CONTROL_MAP[mode_val];
  
  // Check if mode byte is non-zero (indicates supported mode)
  if (map_entry.mode_byte == 0x00 && mode_val != 0) {
    return false;
  }
  
  mapping = map_entry;
  return true;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
