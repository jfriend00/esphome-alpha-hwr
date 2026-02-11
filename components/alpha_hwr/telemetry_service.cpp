/**
 * Telemetry Service Implementation
 * 
 * Manages all telemetry operations for the ALPHA HWR pump.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 */

#include "telemetry_service.h"
#include "frame_builder.h"
#include "frame_parser.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

TelemetryService::TelemetryService() {
  ESP_LOGI(TAG, "TelemetryService created");
}

void TelemetryService::set_write_callback(WriteCallback callback) {
  write_callback_ = std::move(callback);
}

void TelemetryService::set_scheduler_callback(SchedulerCallback callback) {
  scheduler_callback_ = std::move(callback);
}

void TelemetryService::set_sensor_update_callback(SensorUpdateCallback callback) {
  sensor_update_callback_ = std::move(callback);
}

void TelemetryService::start() {
  if (running_) {
    ESP_LOGW(TAG, "Service already running");
    return;
  }
  
  ESP_LOGI(TAG, "Starting telemetry service");
  running_ = true;
}

void TelemetryService::stop() {
  if (!running_) {
    return;
  }
  
  ESP_LOGI(TAG, "Stopping telemetry service");
  running_ = false;
}

bool TelemetryService::is_running() const {
  return running_;
}

void TelemetryService::send_read_request(uint32_t register_addr) {
  if (!write_callback_) {
    ESP_LOGW(TAG, "Write callback not set, cannot send read request");
    return;
  }
  
  // Build the read packet using frame_builder
  uint8_t packet[11];  // Class 10 read packets are always 11 bytes
  protocol::build_class10_read(register_addr, packet);
  
  // Log what we're sending
  char hex_str[40];
  sprintf(hex_str, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
          packet[0], packet[1], packet[2], packet[3], packet[4], 
          packet[5], packet[6], packet[7], packet[8], packet[9], packet[10]);
  ESP_LOGD(TAG, "Sending READ request for register 0x%06X: %s", register_addr, hex_str);
  
  // Send via write callback
  if (!write_callback_(packet, sizeof(packet))) {
    ESP_LOGW(TAG, "Failed to send read request for register 0x%06X", register_addr);
  }
}

void TelemetryService::poll() {
  if (!running_) {
    ESP_LOGD(TAG, "Service not running, skipping poll");
    return;
  }
  
  if (!write_callback_ || !scheduler_callback_) {
    ESP_LOGW(TAG, "Callbacks not set, cannot poll");
    return;
  }
  
  ESP_LOGI(TAG, "Polling telemetry...");
  
  // Request motor state (0x570045)
  send_read_request(0x570045);
  
  // Small delay between requests
  scheduler_callback_(100, [this]() {
    // Request flow/pressure (0x5D0122)
    send_read_request(0x5D0122);
  });
  
  // Another delay
  scheduler_callback_(200, [this]() {
    // Request temperature (0x5D012C)
    send_read_request(0x5D012C);
  });
  
  // Request alarms (Obj 88, Sub 0 → 0x580000)
  scheduler_callback_(300, [this]() {
    send_read_request(0x580000);
  });
  
  // Request warnings (Obj 88, Sub 11 → 0x58000B)
  scheduler_callback_(400, [this]() {
    send_read_request(0x58000B);
  });
}

void TelemetryService::on_packet(const uint8_t* data, size_t len) {
  // Parse frame using protocol layer
  protocol::ParsedFrame frame = protocol::parse_frame(data, len);
  
  // Validate frame structure
  if (!frame.valid) {
    ESP_LOGD(TAG, "Invalid frame structure, ignoring");
    return;
  }
  
  // Check for response frame (we only process responses)
  if (frame.frame_type != protocol::FrameType::RESPONSE) {
    ESP_LOGD(TAG, "Not a response frame, ignoring");
    return;
  }
  
  // Check for Class 10 (we only handle Class 10 for now)
  if (frame.class_byte != protocol::CLASS_10) {
    ESP_LOGD(TAG, "Not Class 10 (got 0x%02X), ignoring", frame.class_byte);
    return;
  }
  
  uint8_t opspec = frame.opspec;
  ESP_LOGD(TAG, "Class 10 packet, OpSpec: 0x%02X, Sub=0x%04X, Obj=0x%04X", 
           opspec, frame.sub_id, frame.obj_id);
  
  // Route to appropriate handler based on OpSpec
  switch (opspec) {
    case 0x30:  // Motor state response
      handle_motor_state_response(data, len);
      break;
    
    case 0x2B:  // Flow/pressure response
      handle_flow_pressure_response(data, len);
      break;
    
    case 0x14:  // Temperature response
      handle_temperature_response(data, len);
      break;
    
    case 0x13:  // Alarms/warnings response
      if (len >= 10) {
        uint16_t sub_id = (data[6] << 8) | data[7];
        uint16_t obj_id = (data[8] << 8) | data[9];
        
        if (obj_id == 0x0058) {  // Obj 88
          if (sub_id == 0x0000) {  // Alarms
            handle_alarms_response(data, len);
          } else if (sub_id == 0x000B) {  // Warnings
            handle_warnings_response(data, len);
          }
        }
      }
      break;
    
    case 0x0E:  // Passive notification
      handle_passive_notification(data, len);
      break;
    
    default:
      ESP_LOGW(TAG, "Unhandled OpSpec: 0x%02X", opspec);
      break;
  }
}

void TelemetryService::handle_motor_state_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Motor state response (OpSpec 0x30)");
  
  // Sensor update callback handles decoding and publishing
  if (sensor_update_callback_) {
    sensor_update_callback_(data, len);
  }
}

void TelemetryService::handle_flow_pressure_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Flow/pressure response (OpSpec 0x2B)");
  
  // Sensor update callback handles decoding and publishing
  if (sensor_update_callback_) {
    sensor_update_callback_(data, len);
  }
}

void TelemetryService::handle_temperature_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Temperature response (OpSpec 0x14)");
  
  // Sensor update callback handles decoding and publishing
  if (sensor_update_callback_) {
    sensor_update_callback_(data, len);
  }
}

void TelemetryService::handle_alarms_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Alarms response (OpSpec 0x13, Obj 88, Sub 0)");
  
  // Sensor update callback handles decoding and publishing
  if (sensor_update_callback_) {
    sensor_update_callback_(data, len);
  }
}

void TelemetryService::handle_warnings_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Warnings response (OpSpec 0x13, Obj 88, Sub 11)");
  
  // Sensor update callback handles decoding and publishing
  if (sensor_update_callback_) {
    sensor_update_callback_(data, len);
  }
}

void TelemetryService::handle_passive_notification(const uint8_t* data, size_t len) {
  if (len < 10) {
    return;
  }
  
  uint16_t sub_id = (data[6] << 8) | data[7];
  uint16_t obj_id = (data[8] << 8) | data[9];
  
  ESP_LOGD(TAG, "Passive telemetry notification: Sub=0x%04X, Obj=0x%04X", sub_id, obj_id);
  
  // Sensor update callback handles decoding and publishing
  if (sensor_update_callback_) {
    sensor_update_callback_(data, len);
  }
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
