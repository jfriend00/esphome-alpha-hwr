/**
 * BLE Transport Layer Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/transport.py
 */

#include "transport.h"
#include "esphome/core/log.h"

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

    // Invoke callback with complete packet
    if (packet_callback_) {
      packet_callback_(reassembly_buffer_.data(), reassembly_buffer_.size());
    } else {
      ESP_LOGW(TAG, "Complete packet received but no callback registered");
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

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
