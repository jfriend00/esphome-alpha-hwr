#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "esphome/core/log.h"
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include <esp_bt_defs.h>
#include "codec.h"
#include "frame_builder.h"
#include "transport.h"
#include "session.h"
#include "auth.h"
#include "telemetry_service.h"
#include "sensor_publisher.h"
#include "ble_connection_manager.h"
#include "control_service.h"
#include "schedule_service.h"
#include "schedule_entry.h"

namespace esphome {
namespace alpha_hwr {

static const char *TAG = "alpha_hwr";

// ============================================================================
// DISCOVERY METHODS FOR GRUNDFOS ALPHA HWR PUMPS
// ============================================================================
//
// Primary Discovery Method (Most Reliable):
//   Match by Grundfos Company ID (0xFE5D) in BLE manufacturer service data.
//   This is GUARANTEED to be present on all ALPHA HWR pumps.
//   
//   Manufacturer Data Structure:
//     Byte 0-1: Frame header
//     Byte 2:   Product Family (0x34 = ALPHA)
//     Byte 3:   Product Type (0x07 = HWR)
//     Byte 4+:  Additional data
//
// Secondary Discovery Method (Fallback):
//   Check for GENI service UUID in advertised services.
//   UUID: 0000fdd0-0000-1000-8000-00805f9b34fb
//   May not always be advertised depending on firmware version.
//
// Tertiary Discovery Method (User-Friendly):
//   Device name pattern: ALPHA_<serial_number>
//   Most user-friendly but not guaranteed to be present.
//
// Implementation Note:
//   The is_alpha_hwr_device() method implements the primary and secondary
//   methods. When using ble_client with a specific MAC address, the device
//   will be validated upon connection.
//
// ============================================================================

// Grundfos Company ID for BLE manufacturer data (most reliable discovery method)
static const uint16_t GRUNDFOS_COMPANY_ID = 0xFE5D;

// Product identification bytes in manufacturer data
static const uint8_t PRODUCT_FAMILY_ALPHA = 0x34;
static const uint8_t PRODUCT_TYPE_HWR = 0x07;

// GENI Protocol UUIDs - Single bidirectional characteristic
// NOTE: The GENI characteristic is inside the Grundfos service (0xFE5D), not a separate service!
static const esp32_ble_tracker::ESPBTUUID GRUNDFOS_SERVICE_UUID = 
    esp32_ble_tracker::ESPBTUUID::from_uint16(0xFE5D);
static const esp32_ble_tracker::ESPBTUUID GENI_CHAR_UUID = 
    esp32_ble_tracker::ESPBTUUID::from_raw("859cffd1-036e-432a-aa28-1a0085b87ba9");

class AlphaHwrComponent : public PollingComponent, public ble_client::BLEClientNode {
 public:
  explicit AlphaHwrComponent(ble_client::BLEClient *parent) : 
      PollingComponent(10000),
      auth_(transport_),
      telemetry_service_(transport_),
      control_service_(transport_, session_),
      schedule_service_(transport_, session_) {
    parent->register_ble_node(this);
    parent_ = parent;
    ESP_LOGI(TAG, "AlphaHwrComponent constructor");
  }

  void set_flow_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_flow_sensor(sensor); }
  void set_head_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_head_sensor(sensor); }
  void set_power_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_power_sensor(sensor); }
  void set_rpm_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_rpm_sensor(sensor); }
  void set_temp_media_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_temp_media_sensor(sensor); }
  void set_temp_converter_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_temp_converter_sensor(sensor); }
  void set_temp_pcb_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_temp_pcb_sensor(sensor); }
  void set_temp_control_box_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_temp_control_box_sensor(sensor); }
  void set_voltage_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_voltage_sensor(sensor); }
  void set_voltage_dc_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_voltage_dc_sensor(sensor); }
  void set_current_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_current_sensor(sensor); }
  void set_inlet_pressure_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_inlet_pressure_sensor(sensor); }
  void set_outlet_pressure_sensor(sensor::Sensor *sensor) { sensor_publisher_.set_outlet_pressure_sensor(sensor); }
   void set_pairing_status_binary_sensor(binary_sensor::BinarySensor *sensor) { pairing_status_sensor_ = sensor; }
#ifdef USE_TEXT_SENSOR
   void set_alarms_text_sensor(text_sensor::TextSensor *sensor) { sensor_publisher_.set_alarms_text_sensor(sensor); }
   void set_warnings_text_sensor(text_sensor::TextSensor *sensor) { sensor_publisher_.set_warnings_text_sensor(sensor); }
   void set_schedule_text_sensor(text_sensor::TextSensor *sensor) { schedule_text_sensor_ = sensor; }
#endif
   void set_pairing_enabled(bool enabled) { pairing_enabled_ = enabled; }

  void setup() override;
  void loop() override;
  void update() override;  // Called every 10 seconds (PollingComponent interval)
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  
  // Static helper method to validate if a device is an ALPHA HWR pump
  // Returns true if device matches Grundfos ALPHA HWR product signature
  static bool is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device);

 private:
  ble_client::BLEClient *parent_ = nullptr;
  
  bool pairing_enabled_ = false;  // Controls whether to attempt BLE pairing/bonding
  
  void authenticate();
  
  // BLE connection manager (handles all BLE operations)
  core::BLEConnectionManager ble_manager_;
  
  // BLE transport layer (handles packet reassembly)
  core::Transport transport_;
  
  // Session state management (handles connection state machine)
  core::Session session_;
  
  // Authentication module (handles 3-stage handshake)
  core::Authentication auth_;
  
  // Telemetry service (handles all telemetry operations)
  services::TelemetryService telemetry_service_;
  
  // Control service (handles pump start/stop and mode changes)
  services::ControlService control_service_;
  
  // Schedule service (handles weekly schedule management)
  services::ScheduleService schedule_service_;
  
  // Sensor publisher (maps telemetry to ESPHome sensors)
  services::SensorPublisher sensor_publisher_;
  
   // Pairing status sensor (separate from telemetry)
   binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
#ifdef USE_TEXT_SENSOR
   // Schedule display sensor
   text_sensor::TextSensor *schedule_text_sensor_{nullptr};
#endif
  
 public:
  // Control service access methods (for ESPHome switches/buttons)
  bool pump_start() { return control_service_.start(); }
  bool pump_stop() { return control_service_.stop(); }
  bool set_control_mode(services::ControlMode mode) { return control_service_.set_mode(mode); }
  bool enable_remote() { return control_service_.enable_remote_mode(); }
  bool disable_remote() { return control_service_.disable_remote_mode(); }
  
   // Schedule service access methods (for ESPHome buttons/lambdas)
   bool enable_schedule() { return schedule_service_.enable(); }
   bool disable_schedule() { return schedule_service_.disable(); }
   bool get_schedule_state(bool *result) { return schedule_service_.get_state(result); }
   bool read_schedule_entries(std::vector<ScheduleEntry> *entries, int layer = -1) {
     return schedule_service_.read_entries(entries, layer);
   }
   bool read_schedule_entries_async(int layer, std::function<void(bool, const std::vector<ScheduleEntry>&)> on_complete) {
     return schedule_service_.read_entries_async(layer, on_complete);
   }
   bool write_schedule_entries(const std::vector<ScheduleEntry> &entries, uint8_t layer = 0) {
     return schedule_service_.write_entries(entries, layer);
   }
   bool write_schedule_entries_async(const std::vector<ScheduleEntry> &entries, uint8_t layer,
                                     std::function<void(bool)> on_complete) {
     return schedule_service_.write_entries_async(entries, layer, on_complete);
   }
   bool clear_schedule_entry(const std::string &day, uint8_t layer = 0) {
     return schedule_service_.clear_entry(day, layer);
   }
    bool get_schedule_display_string(const std::vector<ScheduleEntry> &entries, std::string *result) {
      return schedule_service_.get_schedule_display_string(entries, result);
    }

    /**
     * Asynchronously read the pump schedule and update the text sensor display.
     * 
     * This is a convenience method for displaying the current schedule in Home Assistant.
     * It reads all schedule layers from the pump and formats them into a readable string,
     * then publishes to the schedule_text_sensor if one is configured.
     * 
     * Usage in YAML button lambda:
     *   on_press:
     *     - lambda: id(pump).update_schedule_display();
     */
    void update_schedule_display() {
      // Read all schedule layers asynchronously
      this->read_schedule_entries_async(-1, [this](bool success, const std::vector<ScheduleEntry> &entries) {
        if (!success) {
          ESP_LOGW(TAG, "Failed to read schedule for display update");
#ifdef USE_TEXT_SENSOR
          if (this->schedule_text_sensor_) {
            this->schedule_text_sensor_->publish_state("Error reading schedule");
          }
#endif
          return;
        }

        // Format and display the schedule
#ifdef USE_TEXT_SENSOR
        if (this->schedule_text_sensor_) {
          std::string display_str;
          if (this->get_schedule_display_string(entries, &display_str)) {
            this->schedule_text_sensor_->publish_state(display_str);
            ESP_LOGI(TAG, "Schedule display updated:\n%s", display_str.c_str());
          } else {
            this->schedule_text_sensor_->publish_state("Error formatting schedule");
          }
        }
#endif
      });
    }
};

}  // namespace alpha_hwr
}  // namespace esphome
