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
      control_service_(transport_, session_) {
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
  
  // Sensor publisher (maps telemetry to ESPHome sensors)
  services::SensorPublisher sensor_publisher_;
  
  // Pairing status sensor (separate from telemetry)
  binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
  
 public:
  // Control service access methods (for ESPHome switches/buttons)
  bool pump_start() { return control_service_.start(); }
  bool pump_stop() { return control_service_.stop(); }
  bool set_control_mode(services::ControlMode mode) { return control_service_.set_mode(mode); }
  bool enable_remote() { return control_service_.enable_remote_mode(); }
  bool disable_remote() { return control_service_.disable_remote_mode(); }
};

}  // namespace alpha_hwr
}  // namespace esphome
