/**
 * Sensor Publisher Implementation
 * 
 * Maps decoded telemetry structs to ESPHome sensor components.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 *            (Python updates internal model, we publish to sensors)
 */

#include "sensor_publisher.h"
#include <cmath>

namespace esphome {
namespace alpha_hwr {
namespace services {

SensorPublisher::SensorPublisher() {
  ESP_LOGI(TAG, "SensorPublisher created");
}

void SensorPublisher::publish_motor_state(const protocol::MotorStateTelemetry& motor) {
  // Validate that we have at least some valid data
  if (!motor.has_power && !motor.has_speed) {
    ESP_LOGD(TAG, "Motor state has no valid data, skipping publish");
    return;
  }
  
  // Log summary
  ESP_LOGI(TAG, "✓ Motor: AC=%.1fV, DC=%.1fV, %.2fA, %.1fW, %.0f RPM",
           motor.has_voltage_ac ? motor.voltage_ac_v : 0,
           motor.has_voltage_dc ? motor.voltage_dc_v : 0,
           motor.has_current ? motor.current_a : 0,
           motor.power_w, motor.speed_rpm);
  
  // Publish voltage AC
  if (motor.has_voltage_ac && voltage_sensor_ != nullptr) {
    voltage_sensor_->publish_state(motor.voltage_ac_v);
  }
  
  // Publish voltage DC
  if (motor.has_voltage_dc && voltage_dc_sensor_ != nullptr) {
    voltage_dc_sensor_->publish_state(motor.voltage_dc_v);
  }
  
  // Publish current
  if (motor.has_current && current_sensor_ != nullptr) {
    current_sensor_->publish_state(motor.current_a);
  }
  
  // Publish power
  if (motor.has_power && power_sensor_ != nullptr) {
    power_sensor_->publish_state(motor.power_w);
  }
  
  // Publish RPM
  if (motor.has_speed && rpm_sensor_ != nullptr) {
    rpm_sensor_->publish_state(motor.speed_rpm);
  }
}

void SensorPublisher::publish_flow_pressure(const protocol::FlowPressureTelemetry& flow) {
  // Validate that we have at least some valid data
  if (!flow.has_flow && !flow.has_head) {
    ESP_LOGD(TAG, "Flow/pressure has no valid data, skipping publish");
    return;
  }
  
  // Log summary
  ESP_LOGI(TAG, "✓ Flow/Head: %.3f m³/h, %.2f m, P_in=%.2f bar",
           flow.flow_m3h, flow.head_m,
           flow.has_inlet_pressure ? flow.inlet_pressure_bar : NAN);
  
  // Publish flow rate
  if (flow.has_flow && flow_sensor_ != nullptr) {
    flow_sensor_->publish_state(flow.flow_m3h);
  }
  
  // Publish head pressure
  if (flow.has_head && head_sensor_ != nullptr) {
    head_sensor_->publish_state(flow.head_m);
  }
  
  // Publish inlet pressure (often NaN on HWR models)
  if (flow.has_inlet_pressure && inlet_pressure_sensor_ != nullptr) {
    if (!std::isnan(flow.inlet_pressure_bar)) {
      inlet_pressure_sensor_->publish_state(flow.inlet_pressure_bar);
    }
  }
}

void SensorPublisher::publish_temperature(const protocol::TemperatureTelemetry& temp) {
  // Log summary
  ESP_LOGI(TAG, "✓ Temps: Media=%.1f°C, PCB=%.1f°C, Box=%.1f°C",
           temp.has_media_temp ? temp.media_temperature_c : NAN,
           temp.has_pcb_temp ? temp.pcb_temperature_c : NAN,
           temp.has_control_box_temp ? temp.control_box_temperature_c : NAN);
  
  // Publish media temperature (with range validation)
  if (temp.has_media_temp && temp_media_sensor_ != nullptr) {
    // Validate temperature is in reasonable range (-20°C to 100°C)
    if (temp.media_temperature_c >= -20 && temp.media_temperature_c <= 100) {
      temp_media_sensor_->publish_state(temp.media_temperature_c);
    } else {
      ESP_LOGW(TAG, "Media temperature out of range: %.1f°C", temp.media_temperature_c);
    }
  }
  
  // Publish PCB temperature (with range validation)
  if (temp.has_pcb_temp && temp_pcb_sensor_ != nullptr) {
    // Validate temperature is in reasonable range (-20°C to 150°C)
    if (temp.pcb_temperature_c >= -20 && temp.pcb_temperature_c <= 150) {
      temp_pcb_sensor_->publish_state(temp.pcb_temperature_c);
    } else {
      ESP_LOGW(TAG, "PCB temperature out of range: %.1f°C", temp.pcb_temperature_c);
    }
  }
  
  // Publish control box temperature (with range validation)
  if (temp.has_control_box_temp && temp_control_box_sensor_ != nullptr) {
    // Validate temperature is in reasonable range (-20°C to 150°C)
    if (temp.control_box_temperature_c >= -20 && temp.control_box_temperature_c <= 150) {
      temp_control_box_sensor_->publish_state(temp.control_box_temperature_c);
    } else {
      ESP_LOGW(TAG, "Control box temperature out of range: %.1f°C", temp.control_box_temperature_c);
    }
  }
}

void SensorPublisher::publish_alarms(const std::vector<uint16_t>& codes) {
#ifdef USE_TEXT_SENSOR
  if (alarms_sensor_ == nullptr) {
    return;
  }
  
  std::string codes_str = format_codes(codes);
  ESP_LOGI(TAG, "✓ Alarms: %s", codes_str.c_str());
  alarms_sensor_->publish_state(codes_str);
#endif
}

void SensorPublisher::publish_warnings(const std::vector<uint16_t>& codes) {
#ifdef USE_TEXT_SENSOR
  if (warnings_sensor_ == nullptr) {
    return;
  }
  
  std::string codes_str = format_codes(codes);
  ESP_LOGI(TAG, "✓ Warnings: %s", codes_str.c_str());
  warnings_sensor_->publish_state(codes_str);
#endif
}

std::string SensorPublisher::format_codes(const std::vector<uint16_t>& codes) {
  if (codes.empty()) {
    return "None";
  }
  
  std::string result;
  for (size_t i = 0; i < codes.size(); i++) {
    if (i > 0) {
      result += ", ";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", codes[i]);
    result += buf;
  }
  
  return result;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
