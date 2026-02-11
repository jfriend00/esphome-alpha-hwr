/**
 * BLE Transport Layer Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/transport.py
 */

#include "transport.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <algorithm>

namespace esphome {
namespace alpha_hwr {
namespace core {

static const char *const TAG = "alpha_hwr.transport";

Transport::Transport() 
    : reassembling_(false),
      expected_packet_length_(0),
      packet_callback_(nullptr) {
  reassembly_buffer_.reserve(MAX_PACKET_SIZE);
  ESP_LOGD(TAG, "Transport initialized");
}

void Transport::set_packet_callback(PacketCallback callback) {
  packet_callback_ = callback;
  ESP_LOGD(TAG, "Packet callback registered");
}

bool Transport::is_frame_start(uint8_t byte) const {
  return (byte == FRAME_START_RESPONSE || byte == FRAME_START_REQUEST);
}

uint16_t Transport::calculate_expected_length() const {
  if (reassembly_buffer_.size() < 2) {
    return 0;
  }
  // GENI packet: [Start][Length][Payload...][CRC_H][CRC_L]
  // Total = Length field + 4 (start + length + 2-byte CRC)
  // But actual packet format is: [Start][Length][Payload][CRC_H][CRC_L]
  // where the length field counts: Payload bytes only
  // So total = length_field + 2 (start + length bytes)
  return reassembly_buffer_[1] + 2;
}

void Transport::on_notification(const uint8_t* data, size_t len) {
  if (len == 0) {
    return;
  }

  ESP_LOGV(TAG, "BLE notification: %d bytes", len);

  // Check if this is the start of a new packet
  // Frame start bytes: 0x24 (response) or 0x27 (request/echo)
  if (is_frame_start(data[0])) {
    // New packet starting
    ESP_LOGV(TAG, "New packet detected (frame start: 0x%02X)", data[0]);
    
    reassembly_buffer_.clear();
    reassembly_buffer_.insert(reassembly_buffer_.end(), data, data + len);
    reassembling_ = true;

    // Calculate expected length
    if (reassembly_buffer_.size() >= 2) {
      expected_packet_length_ = calculate_expected_length();
      ESP_LOGD(TAG, "Expected packet length: %d bytes", expected_packet_length_);
    }
  } else if (reassembling_) {
    // Continuation of existing packet
    reassembly_buffer_.insert(reassembly_buffer_.end(), data, data + len);
    ESP_LOGV(TAG, "Packet reassembly: %d/%d bytes", 
             reassembly_buffer_.size(), expected_packet_length_);
  } else {
    // Unexpected data (not a frame start, not reassembling)
    ESP_LOGW(TAG, "Unexpected notification data (not frame start, not reassembling)");
    return;
  }

  // Safety: Check buffer overflow
  if (reassembly_buffer_.size() > MAX_PACKET_SIZE) {
    ESP_LOGW(TAG, "Reassembly buffer overflow (%d bytes), clearing", 
             reassembly_buffer_.size());
    reset();
    return;
  }

  // Check if packet is complete
  if (reassembling_ && expected_packet_length_ > 0 && 
      reassembly_buffer_.size() >= expected_packet_length_) {
    ESP_LOGD(TAG, "Packet complete: %d bytes", reassembly_buffer_.size());

    // Try to dispatch to registered response handler first
    bool dispatched = try_dispatch_response(reassembly_buffer_.data(), reassembly_buffer_.size());

    // If not dispatched to a handler, invoke general packet callback
    if (!dispatched && packet_callback_) {
      packet_callback_(reassembly_buffer_.data(), reassembly_buffer_.size());
    } else if (!dispatched) {
      ESP_LOGW(TAG, "Complete packet received but no handler or callback registered");
    }

    // Clear state for next packet
    reassembling_ = false;
    reassembly_buffer_.clear();
    expected_packet_length_ = 0;
  }
}

bool Transport::write_packet(const uint8_t* data, size_t len, WriteCallback write_func) {
  if (!write_func) {
    ESP_LOGE(TAG, "No write callback provided");
    return false;
  }

  if (len == 0) {
    ESP_LOGW(TAG, "Attempted to write empty packet");
    return false;
  }

  // Check if packet needs to be split due to BLE MTU limit
  if (len > BLE_MTU_LIMIT) {
    ESP_LOGD(TAG, "Splitting packet: %d bytes (MTU limit: %d)", len, BLE_MTU_LIMIT);

    // Write first chunk (20 bytes)
    bool success = write_func(data, BLE_MTU_LIMIT);
    if (!success) {
      ESP_LOGE(TAG, "Failed to write first chunk");
      return false;
    }
    ESP_LOGV(TAG, "Wrote chunk 1: %d bytes", BLE_MTU_LIMIT);

    // Small delay between chunks (Python uses 10ms)
    // In ESPHome, we can't do async delay here, so we rely on BLE stack buffering
    // The ESP-IDF BLE stack should handle this appropriately

    // Write remaining bytes
    size_t remaining = len - BLE_MTU_LIMIT;
    success = write_func(data + BLE_MTU_LIMIT, remaining);
    if (!success) {
      ESP_LOGE(TAG, "Failed to write second chunk");
      return false;
    }
    ESP_LOGV(TAG, "Wrote chunk 2: %d bytes", remaining);
    ESP_LOGD(TAG, "Split write complete: %d bytes total", len);
    
    return true;
  } else {
    // Single write for packets <= 20 bytes
    bool success = write_func(data, len);
    if (success) {
      ESP_LOGV(TAG, "Wrote packet: %d bytes", len);
    } else {
      ESP_LOGE(TAG, "Failed to write packet");
    }
    return success;
  }
}

void Transport::reset() {
  ESP_LOGD(TAG, "Resetting transport state");
  reassembling_ = false;
  reassembly_buffer_.clear();
  expected_packet_length_ = 0;
}

void Transport::register_response_handler(uint16_t object_id, uint16_t sub_id, ResponseCallback callback) {
  // Safety: Limit number of pending handlers to prevent memory issues
  if (pending_handlers_.size() >= MAX_PENDING_HANDLERS) {
    ESP_LOGW(TAG, "Too many pending handlers (%d), rejecting new handler for Object %d SubID %d",
             pending_handlers_.size(), object_id, sub_id);
    return;
  }

  // Get current timestamp (millis() equivalent)
  uint32_t now = millis();

  PendingHandler handler;
  handler.object_id = object_id;
  handler.sub_id = sub_id;
  handler.callback = callback;
  handler.timestamp_ms = now;

  pending_handlers_.push_back(handler);

  ESP_LOGD(TAG, "Registered response handler for Object %d SubID %d (total pending: %d)",
           object_id, sub_id, pending_handlers_.size());
}

void Transport::check_timeouts(uint32_t timeout_ms) {
  if (pending_handlers_.empty()) {
    return;
  }

  uint32_t now = millis();
  size_t initial_count = pending_handlers_.size();

  // Remove timed-out handlers
  // Use erase-remove idiom to remove elements while iterating
  pending_handlers_.erase(
    std::remove_if(pending_handlers_.begin(), pending_handlers_.end(),
      [now, timeout_ms](const PendingHandler& handler) {
        uint32_t age = now - handler.timestamp_ms;
        if (age > timeout_ms) {
          ESP_LOGW(TAG, "Response handler timeout for Object %d SubID %d (%d ms)",
                   handler.object_id, handler.sub_id, age);
          return true;  // Remove this handler
        }
        return false;  // Keep this handler
      }),
    pending_handlers_.end()
  );

  size_t removed = initial_count - pending_handlers_.size();
  if (removed > 0) {
    ESP_LOGD(TAG, "Removed %d timed-out handlers (%d remaining)", removed, pending_handlers_.size());
  }
}

bool Transport::try_dispatch_response(const uint8_t* data, size_t len) {
  if (pending_handlers_.empty()) {
    return false;  // No handlers registered
  }

  // Validate packet structure
  // GENI Frame: [STX][LEN][DST][SRC][Class][OpSpec][ObjH][ObjL][SubH][SubL][...DATA...][CRC_H][CRC_L]
  // Minimum size: 12 bytes (header 10 + CRC 2)
  if (len < 12) {
    ESP_LOGV(TAG, "Packet too short for response matching (%d bytes)", len);
    return false;
  }

  // Check if this is a Class 10 response (most common for read operations)
  if (data[4] != 0x0A) {
    ESP_LOGV(TAG, "Not a Class 10 packet (class=0x%02X), skipping response matching", data[4]);
    return false;
  }

  // Extract OpSpec to see what kind of response this is
  uint8_t opspec = (len > 5) ? data[5] : 0x00;

  // CRITICAL: Different OpSpecs have DIFFERENT packet structures!
  // See reference/alpha-hwr/src/alpha_hwr/protocol/frame_parser.py lines 278-294
  //
  // Register-read responses (OpSpec 0x30, 0x2B, 0x14, 0x2E, 0x2D, 0x09):
  //   Format: [Class][OpSpec][Seq(2)][Id(2)][Res(2)][DataLen][Data...]
  //   - Bytes 6-7: Sequence number (NOT SubID)
  //   - Bytes 8-9: Register ID (can be treated as obj_id for routing)
  //   - These are telemetry streaming responses, NOT DataObject responses
  //
  // DataObject responses (OpSpec 0x03, 0x93, 0xB3, etc.):
  //   Format: [Class][OpSpec][SubH][SubL][ObjH][ObjL][Data...]
  //   - Bytes 6-7: Sub-ID (big-endian uint16)
  //   - Bytes 8-9: Object ID (big-endian uint16)
  //   - These are the responses we want to match for schedule reads
  
  uint16_t packet_obj_id = 0;
  uint16_t packet_sub_id = 0;
  
  // Only parse as DataObject format for non-register-read OpSpecs
  bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 || 
                           opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
  
  if (is_register_read) {
    // This is a telemetry register read - don't try to match it
    ESP_LOGV(TAG, "Skipping register-read response (OpSpec=0x%02X) for response matching", opspec);
    return false;
  }
  
  // Parse as DataObject format
  if (len > 9) {
    packet_sub_id = (data[6] << 8) | data[7];  // Sub-ID is 16-bit big-endian
    packet_obj_id = (data[8] << 8) | data[9];  // Object ID is 16-bit big-endian
  } else {
    ESP_LOGV(TAG, "Packet too short to extract Object/SubID");
    return false;
  }

  ESP_LOGV(TAG, "DataObject response: OpSpec=0x%02X, Object %d SubID %d (checking %d handlers)",
           opspec, packet_obj_id, packet_sub_id, pending_handlers_.size());

  // Search for matching handler
  for (auto it = pending_handlers_.begin(); it != pending_handlers_.end(); ++it) {
    if (it->object_id == packet_obj_id && it->sub_id == packet_sub_id) {
      ESP_LOGD(TAG, "Response handler matched for Object %d SubID %d", packet_obj_id, packet_sub_id);

      // Extract payload: skip header (10 bytes) and CRC (2 bytes)
      const uint8_t* payload = data + 10;
      size_t payload_len = len - 12;

      // Invoke callback
      if (it->callback) {
        it->callback(payload, payload_len);
        ESP_LOGD(TAG, "Response handler invoked with %d bytes payload", payload_len);
      }

      // Remove handler (one-shot)
      pending_handlers_.erase(it);
      ESP_LOGD(TAG, "Response handler removed (%d remaining)", pending_handlers_.size());

      return true;  // Handler was found and invoked
    }
  }

  ESP_LOGV(TAG, "No matching response handler for Object %d SubID %d", packet_obj_id, packet_sub_id);
  return false;  // No matching handler found
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
