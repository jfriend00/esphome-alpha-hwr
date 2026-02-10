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

// Authentication packets from Python library source (alpha_hwr/core/authentication.py)
// These match the Python implementation EXACTLY - do not use documentation packets!
static const uint8_t AUTH_LEGACY[] = {0x27, 0x07, 0xE7, 0xF8, 0x02, 0x03, 0x94, 0x95, 0x96, 0xEB, 0x47};
static const uint8_t AUTH_CLASS10[] = {0x27, 0x07, 0xE7, 0xF8, 0x0A, 0x03, 0x56, 0x00, 0x06, 0xC5, 0x5A};
// IMPORTANT: EXTEND_1 is Class 0x0B, EXTEND_2 is Class 0x05 (matching Python source)
static const uint8_t AUTH_EXT_1[] = {0x27, 0x05, 0xE7, 0xF8, 0x0B, 0xC1, 0x0F, 0xD0, 0xC3};
static const uint8_t AUTH_EXT_2[] = {0x27, 0x05, 0xE7, 0xF8, 0x05, 0xC1, 0x4B, 0xC3, 0x82};

// CRC-16-CCITT lookup table for GENI protocol
static const uint16_t CRC_TABLE[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

class AlphaHwrComponent : public PollingComponent, public ble_client::BLEClientNode {
 public:
  explicit AlphaHwrComponent(ble_client::BLEClient *parent) : PollingComponent(10000) {
    parent->register_ble_node(this);
    parent_ = parent;
    ESP_LOGI(TAG, "AlphaHwrComponent constructor");
  }

  void set_flow_sensor(sensor::Sensor *sensor) { flow_sensor_ = sensor; }
  void set_head_sensor(sensor::Sensor *sensor) { head_sensor_ = sensor; }
  void set_power_sensor(sensor::Sensor *sensor) { power_sensor_ = sensor; }
  void set_rpm_sensor(sensor::Sensor *sensor) { rpm_sensor_ = sensor; }
  void set_temp_media_sensor(sensor::Sensor *sensor) { temp_media_sensor_ = sensor; }
  void set_temp_converter_sensor(sensor::Sensor *sensor) { temp_converter_sensor_ = sensor; }
  void set_temp_pcb_sensor(sensor::Sensor *sensor) { temp_pcb_sensor_ = sensor; }
  void set_temp_control_box_sensor(sensor::Sensor *sensor) { temp_control_box_sensor_ = sensor; }
  void set_voltage_sensor(sensor::Sensor *sensor) { voltage_sensor_ = sensor; }
  void set_voltage_dc_sensor(sensor::Sensor *sensor) { voltage_dc_sensor_ = sensor; }
  void set_current_sensor(sensor::Sensor *sensor) { current_sensor_ = sensor; }
  void set_inlet_pressure_sensor(sensor::Sensor *sensor) { inlet_pressure_sensor_ = sensor; }
  void set_outlet_pressure_sensor(sensor::Sensor *sensor) { outlet_pressure_sensor_ = sensor; }
  void set_pairing_status_binary_sensor(binary_sensor::BinarySensor *sensor) { pairing_status_sensor_ = sensor; }
#ifdef USE_TEXT_SENSOR
  void set_alarms_text_sensor(text_sensor::TextSensor *sensor) { alarms_sensor_ = sensor; }
  void set_warnings_text_sensor(text_sensor::TextSensor *sensor) { warnings_sensor_ = sensor; }
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
  
  // Debug helper to dump all discovered services and characteristics
  void dump_services();

 private:
  ble_client::BLEClient *parent_ = nullptr;
  sensor::Sensor *flow_sensor_{nullptr};
  sensor::Sensor *head_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *rpm_sensor_{nullptr};
  sensor::Sensor *temp_media_sensor_{nullptr};
  sensor::Sensor *temp_converter_sensor_{nullptr};
  sensor::Sensor *temp_pcb_sensor_{nullptr};
  sensor::Sensor *temp_control_box_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *voltage_dc_sensor_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *inlet_pressure_sensor_{nullptr};
  sensor::Sensor *outlet_pressure_sensor_{nullptr};
  binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *alarms_sensor_{nullptr};
  text_sensor::TextSensor *warnings_sensor_{nullptr};
#endif
  
  bool pairing_enabled_ = false;  // Controls whether to attempt BLE pairing/bonding
  
  float read_float_be(uint8_t *data, size_t offset);
  void decode_packet(uint8_t *data, size_t len);
  void authenticate();
  void send_auth_packet(const uint8_t *data, size_t len);
  void subscribe_to_notifications();
  void init_security();
  
  // Telemetry polling functions
  void poll_telemetry();
  void build_class10_read_packet(uint32_t register_addr, uint8_t *packet_out);
  uint16_t calc_crc16(const uint8_t *data, size_t len);
  uint16_t calc_crc16_read(const uint8_t *data, size_t len);
  void send_read_request(uint32_t register_addr);
  
  // Non-blocking authentication stages
  void auth_stage1_legacy_burst(int repeat_count);
  void auth_stage2_class10_burst(int repeat_count);
  void auth_stage3_extensions();
  
  bool authenticated_ = false;
  bool subscribed_ = false;
  bool auth_started_ = false;  // Prevent multiple auth attempts
  uint32_t last_auth_time_ = 0;
  
  // Track current authentication stage for async execution
  int auth_stage1_count_ = 0;
  int auth_stage2_count_ = 0;
  
  // Service discovery retry mechanism
  bool geni_service_found_ = false;
  uint8_t discovery_retry_count_ = 0;
  static const uint8_t MAX_DISCOVERY_RETRIES = 3;
  static const uint32_t DISCOVERY_RETRY_DELAY_MS = 1000;
  static const uint32_t POST_CONNECT_DELAY_MS = 500;
  
  // Packet reassembly for multi-packet BLE responses
  std::vector<uint8_t> reassembly_buffer_;
  uint8_t expected_packet_length_ = 0;
  bool reassembling_ = false;
};

}  // namespace alpha_hwr
}  // namespace esphome
