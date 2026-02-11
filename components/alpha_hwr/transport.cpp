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
  this->packet_callback_ = callback;
}

void Transport::loop() {
  if (this->command_queue_.empty()) {
    this->state_ = State::IDLE;
    return;
  }

  uint32_t now = millis();
  auto &cmd = this->command_queue_.front();

  switch (this->state_) {
    case State::IDLE:
      // Check pacing between commands
      if (now - this->last_send_time_ < this->send_pacing_ms_) {
        return;
      }
      this->state_ = State::SENDING_CHUNKS;
      cmd.bytes_sent = 0;
      // Fall through to SENDING_CHUNKS
      [[fallthrough]];

    case State::SENDING_CHUNKS: {
      // Check pacing between chunks
      if (now - this->last_send_time_ < this->send_pacing_ms_) {
        return;
      }

      size_t remaining = cmd.packet.size() - cmd.bytes_sent;
      size_t to_send = std::min(remaining, BLE_MTU_LIMIT);

      ESP_LOGV(TAG, "Sending chunk: %zu bytes (%zu/%zu sent)", 
               to_send, cmd.bytes_sent + to_send, cmd.packet.size());

      if (this->write_callback_(cmd.packet.data() + cmd.bytes_sent, to_send)) {
        cmd.bytes_sent += to_send;
        this->last_send_time_ = now;

        if (cmd.bytes_sent >= cmd.packet.size()) {
          // Finished sending all chunks
          if (cmd.expect_obj_id != 0) {
            this->state_ = State::AWAITING_RESPONSE;
            cmd.timestamp_ms = now;
            cmd.waiting_for_response = true;
            ESP_LOGD(TAG, "Command sent, waiting for response (Obj %d Sub %d)", 
                     cmd.expect_obj_id, cmd.expect_sub_id);
          } else {
            ESP_LOGD(TAG, "Command sent (no response expected)");
            if (cmd.callback) {
              cmd.callback(true, nullptr, 0);
            }
            this->command_queue_.pop_front();
            this->state_ = State::IDLE;
          }
        }
      } else {
        ESP_LOGE(TAG, "Failed to send chunk, dropping command");
        if (cmd.callback) {
          cmd.callback(false, nullptr, 0);
        }
        this->command_queue_.pop_front();
        this->state_ = State::IDLE;
      }
      break;
    }

    case State::AWAITING_RESPONSE:
      if (now - cmd.timestamp_ms > cmd.timeout_ms) {
        ESP_LOGW(TAG, "Command timeout waiting for Obj %d Sub %d", 
                 cmd.expect_obj_id, cmd.expect_sub_id);
        if (cmd.callback) {
          cmd.callback(false, nullptr, 0);
        }
        this->command_queue_.pop_front();
        this->state_ = State::IDLE;
      }
      break;

    default:
      break;
  }
}

void Transport::send_command(const std::vector<uint8_t>& packet, uint16_t expect_obj_id, 
                            uint16_t expect_sub_id, CommandCallback callback, 
                            uint32_t timeout_ms) {
  Command cmd;
  cmd.packet = packet;
  cmd.expect_obj_id = expect_obj_id;
  cmd.expect_sub_id = expect_sub_id;
  cmd.callback = callback;
  cmd.timeout_ms = timeout_ms;
  
  this->command_queue_.push_back(cmd);
  ESP_LOGD(TAG, "Command queued (queue size: %zu)", this->command_queue_.size());
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

     // Log first 12 bytes for debugging packet structure
     if (reassembly_buffer_.size() >= 12) {
       ESP_LOGV(TAG, "Packet bytes [0-11]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                reassembly_buffer_[0], reassembly_buffer_[1], reassembly_buffer_[2], reassembly_buffer_[3],
                reassembly_buffer_[4], reassembly_buffer_[5], reassembly_buffer_[6], reassembly_buffer_[7],
                reassembly_buffer_[8], reassembly_buffer_[9], reassembly_buffer_[10], reassembly_buffer_[11]);
     }

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
  // Extract payload: skip header (10 bytes) and CRC (2 bytes)
  const uint8_t* payload = data + 10;
  size_t payload_len = len - 12;

  // Extract identifiers from the response packet
  uint8_t opspec = (len > 5) ? data[5] : 0x00;
  uint16_t packet_sub_id = (len > 7) ? (data[6] << 8) | data[7] : 0;
  uint16_t packet_obj_id = (len > 9) ? (data[8] << 8) | data[9] : 0;

  ESP_LOGV(TAG, "Parsing response: OpSpec=0x%02X, Sub=%d, Obj=%d", opspec, packet_sub_id, packet_obj_id);

  // 1. Check if we are waiting for a command response
  if (this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    
    // For Class 10 DataObject reads, we need to check if this is a valid response
    // by checking the packet structure, not just Object/Sub ID matching
    // because the pump may send telemetry responses while we're waiting
    
    // First, validate basic packet structure
    if (len < 12) {
      ESP_LOGV(TAG, "Packet too short, discarding");
      return false;  // Too short, discard it
    }
    
    // Check if this is a Class 10 packet (0x0A at byte 4)
    bool is_class10 = (data[4] == 0x0A);
    
    if (!is_class10) {
      ESP_LOGV(TAG, "Not a Class 10 packet (class=0x%02X), discarding for command response matching", data[4]);
      return false;  // Not Class 10, let it go to packet callback or discard
    }
    
    // This IS a Class 10 response. Now check if it matches our expected Object/Sub ID
    // Extract OpSpec
    opspec = (len > 5) ? data[5] : 0x00;
    
    // Determine packet structure based on OpSpec
    bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 || 
                             opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
    
     if (is_register_read) {
       // This is telemetry register-read response, not a DataObject response
       // Discard it for command matching purposes
       ESP_LOGD(TAG, "Class 10 register-read (OpSpec=0x%02X), skipping for command response (waiting for Obj %d Sub %d)", 
                opspec, cmd.expect_obj_id, cmd.expect_sub_id);
       return false;
     }
    
    // This is a Class 10 DataObject response. Extract Object/Sub IDs
    if (len > 9) {
      packet_sub_id = (data[6] << 8) | data[7];  // Sub-ID is at bytes 6-7
      packet_obj_id = (data[8] << 8) | data[9];  // Object ID is at bytes 8-9
    } else {
      ESP_LOGV(TAG, "DataObject packet too short to extract IDs");
      return false;
    }
    
    // Now check if this matches our expected Object/Sub ID
    bool matched = (packet_obj_id == cmd.expect_obj_id && (packet_sub_id == cmd.expect_sub_id || packet_sub_id == 0));
    
    // BACKUP MATCH: If ObjID doesn't match but SubID matches our expected ObjID (swapped)
    if (!matched && packet_sub_id == cmd.expect_obj_id) {
      matched = true;
    }

    if (matched) {
      ESP_LOGD(TAG, "Command response matched for Obj %d (Sub %d -> %d)", 
               packet_obj_id, cmd.expect_sub_id, packet_sub_id);
      if (cmd.callback) {
        cmd.callback(true, payload, payload_len);
      }
      this->command_queue_.pop_front();
      this->state_ = State::IDLE;
      return true;
     } else {
       // This is a Class 10 DataObject response but doesn't match what we're waiting for
       // This means the pump sent us a different object's response
       // Log it but DON'T consume it - let it pass to registered handlers or telemetry processing
       ESP_LOGD(TAG, "Class 10 DataObject received (Obj %d Sub %d) but doesn't match expected (Obj %d Sub %d), continuing to wait",
                packet_obj_id, packet_sub_id, cmd.expect_obj_id, cmd.expect_sub_id);
       // Return false so it can be processed by other handlers
       // We stay in AWAITING_RESPONSE state and keep waiting
       return false;
     }
  }

  // 2. Check registered response handlers
  if (pending_handlers_.empty()) {
    return false;  // No handlers registered
  }

  // Validate packet structure
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
  opspec = (len > 5) ? data[5] : 0x00;

  packet_obj_id = 0;
  packet_sub_id = 0;
  
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
