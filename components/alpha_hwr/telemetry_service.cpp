/**
 * Telemetry Service Implementation
 * 
 * Manages all telemetry operations for the ALPHA HWR pump.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 */

#include "telemetry_service.h"
#include "transport.h"
#include "frame_builder.h"
#include "frame_parser.h"
#include "telemetry_decoder.h"
#include "sensor_publisher.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

TelemetryService::TelemetryService(core::Transport &transport) : transport_(transport) {
  ESP_LOGI(TAG, "TelemetryService created");
}

void TelemetryService::set_sensor_publisher(SensorPublisher* publisher) {
  sensor_publisher_ = publisher;
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
  // Build the read packet using frame_builder
  uint8_t packet_raw[11];  // Class 10 read packets are always 11 bytes
  protocol::build_class10_read(register_addr, packet_raw);
  
  std::vector<uint8_t> packet(packet_raw, packet_raw + 11);
  
  ESP_LOGD(TAG, "Queueing READ request for register 0x%06X", register_addr);
  
  // Send via transport queue (no response matching here as telemetry uses various OpSpecs)
  this->transport_.send_command(packet);
}

void TelemetryService::poll() {
  if (!running_) {
    ESP_LOGD(TAG, "Service not running, skipping poll");
    return;
  }
  
  ESP_LOGI(TAG, "Polling telemetry registers...");
  
  // Queue all read requests. The transport layer will pace them.
  send_read_request(0x570045); // Motor state
  send_read_request(0x5D0122); // Flow/pressure
  send_read_request(0x5D012C); // Temperature
  send_read_request(0x580000); // Alarms
  send_read_request(0x58000B); // Warnings
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
  
  // Decode motor state using telemetry decoder
  protocol::MotorStateTelemetry motor = protocol::decode_motor_state_response(data, len);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_motor_state(motor);
  }
}

void TelemetryService::handle_flow_pressure_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Flow/pressure response (OpSpec 0x2B)");
  
  // Decode flow/pressure using telemetry decoder
  protocol::FlowPressureTelemetry flow = protocol::decode_flow_pressure_response(data, len);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_flow_pressure(flow);
  }
}

void TelemetryService::handle_temperature_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Temperature response (OpSpec 0x14)");
  
  // Decode temperature using telemetry decoder
  protocol::TemperatureTelemetry temp = protocol::decode_temperature_response(data, len);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_temperature(temp);
  }
}

void TelemetryService::handle_alarms_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Alarms response (OpSpec 0x13, Obj 88, Sub 0)");
  
  // Decode alarms using telemetry decoder
  protocol::AlarmWarningTelemetry alarms = protocol::decode_alarms_warnings_response(data, len, 0x13);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_alarms(alarms.codes);
  }
}

void TelemetryService::handle_warnings_response(const uint8_t* data, size_t len) {
  ESP_LOGI(TAG, "Warnings response (OpSpec 0x13, Obj 88, Sub 11)");
  
  // Decode warnings using telemetry decoder
  protocol::AlarmWarningTelemetry warnings = protocol::decode_alarms_warnings_response(data, len, 0x13);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_warnings(warnings.codes);
  }
}

void TelemetryService::handle_passive_notification(const uint8_t* data, size_t len) {
  if (len < 10) {
    return;
  }
  
  uint16_t sub_id = (data[6] << 8) | data[7];
  uint16_t obj_id = (data[8] << 8) | data[9];
  
  ESP_LOGD(TAG, "Passive telemetry notification: Sub=0x%04X, Obj=0x%04X", sub_id, obj_id);
  
  // Standard Motor State (Sub 0x0045, Obj 0x0057 = Decimal Sub 69, Obj 87)
  if (sub_id == 0x0045 && obj_id == 0x0057) {
    protocol::MotorStateTelemetry motor = protocol::decode_passive_motor_state(data, len);
    if (sensor_publisher_) {
      sensor_publisher_->publish_motor_state(motor);
    }
  }
  // Standard Flow/Head (Sub 0x0122, Obj 0x005D = Decimal Sub 290, Obj 93)
  else if (sub_id == 0x0122 && obj_id == 0x005D) {
    protocol::FlowPressureTelemetry flow = protocol::decode_passive_flow_pressure(data, len);
    if (sensor_publisher_) {
      sensor_publisher_->publish_flow_pressure(flow);
    }
  }
  // Standard Temperature (Sub 0x012C, Obj 0x005D = Decimal Sub 300, Obj 93)
  else if (sub_id == 0x012C && obj_id == 0x005D) {
    protocol::TemperatureTelemetry temp = protocol::decode_passive_temperature(data, len);
    if (sensor_publisher_) {
      sensor_publisher_->publish_temperature(temp);
    }
  }
  else {
    ESP_LOGD(TAG, "Unknown passive notification (Sub=0x%04X, Obj=0x%04X)", sub_id, obj_id);
  }
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
