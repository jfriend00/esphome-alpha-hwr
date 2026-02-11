#include "alpha_hwr.h"
#include "frame_parser.h"
#include "telemetry_decoder.h"

namespace esphome {
namespace alpha_hwr {

// Static method to validate if a BLE device is an ALPHA HWR pump
// Delegates to BLE Connection Manager
bool AlphaHwrComponent::is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device) {
  return core::BLEConnectionManager::is_alpha_hwr_device(
      device, GRUNDFOS_COMPANY_ID, PRODUCT_FAMILY_ALPHA, PRODUCT_TYPE_HWR, GRUNDFOS_SERVICE_UUID);
}

void AlphaHwrComponent::setup() {
  ESP_LOGI(TAG, "Alpha HWR Component setup");
  
  // Initialize BLE connection manager
  ble_manager_.set_ble_client(parent_);
  ble_manager_.set_pairing_enabled(pairing_enabled_);
  ble_manager_.set_pairing_status_sensor(pairing_status_sensor_);
  ble_manager_.set_service_uuid(GRUNDFOS_SERVICE_UUID);
  ble_manager_.set_characteristic_uuid(GENI_CHAR_UUID);
  ble_manager_.init_security();
  
  // Set BLE manager callbacks
  ble_manager_.set_scheduler_callback([this](uint32_t delay_ms, std::function<void()> callback) {
    this->set_timeout(delay_ms, std::move(callback));
  });
  
  ble_manager_.set_connection_callback([this]() {
    this->session_.on_connected();
  });
  
  ble_manager_.set_disconnection_callback([this]() {
    this->session_.on_disconnected();
    this->transport_.reset();
  });
  
  ble_manager_.set_service_found_callback([this]() {
    this->session_.on_service_found();
  });
  
  ble_manager_.set_subscribed_callback([this]() {
    this->session_.on_subscribed();
    
    // Wait for pump to stabilize, then authenticate
    this->set_timeout(2000, [this]() {
      ESP_LOGI(TAG, "Pump stabilized. Starting authentication...");
      this->authenticate();
    });
  });
  
  ble_manager_.set_notification_callback([this](const uint8_t* data, size_t len) {
    // Pass to transport for reassembly
    this->transport_.on_notification(data, len);
  });
  
  // Initialize transport callback for complete packets
  transport_.set_packet_callback([this](const uint8_t* data, size_t len) {
    // Route to telemetry service for processing
    this->telemetry_service_.on_packet(data, len);
  });
  
  // Initialize authentication module callbacks
  auth_.set_write_callback([this](const uint8_t* data, size_t len) -> bool {
    // Get GENI service and characteristic
    auto *service = this->parent_->get_service(GRUNDFOS_SERVICE_UUID);
    if (!service) return false;
    
    auto *chr = this->parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
    if (!chr) return false;
    
    // Write to BLE characteristic using write without response
    auto status = esp_ble_gattc_write_char(
        this->parent_->get_gattc_if(),
        this->parent_->get_conn_id(),
        chr->handle,
        len,
        const_cast<uint8_t*>(data),
        ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    return (status == ESP_OK);
  });
  
  auth_.set_scheduler_callback([this](uint32_t delay_ms, std::function<void()> callback) {
    this->set_timeout(delay_ms, std::move(callback));
  });
  
  auth_.set_completion_callback([this]() {
    this->session_.on_authenticated();
    ESP_LOGI(TAG, "✓ Authentication handshake complete - pump ready");
    
    // Start telemetry service when authenticated
    this->telemetry_service_.start();
  });
  
  // Initialize telemetry service callbacks
  telemetry_service_.set_write_callback([this](const uint8_t* data, size_t len) -> bool {
    // Get GENI service and characteristic
    auto *service = this->parent_->get_service(GRUNDFOS_SERVICE_UUID);
    if (!service) return false;
    
    auto *chr = this->parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
    if (!chr) return false;
    
    // Use transport to write packet (handles splitting if needed)
    auto write_func = [this, chr](const uint8_t* pkt_data, size_t pkt_len) -> bool {
      auto status = esp_ble_gattc_write_char(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          chr->handle,
          pkt_len,
          const_cast<uint8_t*>(pkt_data),
          ESP_GATT_WRITE_TYPE_NO_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      return (status == ESP_OK);
    };
    
    return this->transport_.write_packet(data, len, write_func);
  });
  
  telemetry_service_.set_scheduler_callback([this](uint32_t delay_ms, std::function<void()> callback) {
    this->set_timeout(delay_ms, std::move(callback));
  });
  
  telemetry_service_.set_sensor_publisher(&sensor_publisher_);
  
  // Initialize control service callbacks
  control_service_.set_schedule_callback([this](std::function<void()> callback, uint32_t delay_ms) {
    this->set_timeout(delay_ms, std::move(callback));
  });
  
  control_service_.set_write_callback([this](const uint8_t* data, size_t len) -> bool {
    // Get GENI service and characteristic
    auto *service = this->parent_->get_service(GRUNDFOS_SERVICE_UUID);
    if (!service) return false;
    
    auto *chr = this->parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
    if (!chr) return false;
    
    // Use transport to write packet (handles splitting if needed)
    auto write_func = [this, chr](const uint8_t* pkt_data, size_t pkt_len) -> bool {
      auto status = esp_ble_gattc_write_char(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          chr->handle,
          pkt_len,
          const_cast<uint8_t*>(pkt_data),
          ESP_GATT_WRITE_TYPE_NO_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      return (status == ESP_OK);
    };
    
    return this->transport_.write_packet(data, len, write_func);
  });
  
  // Initialize schedule service callbacks
  schedule_service_.set_schedule_callback([this](std::function<void()> callback, uint32_t delay_ms) {
    this->set_timeout(delay_ms, std::move(callback));
  });
  
  schedule_service_.set_write_callback([this](uint16_t handle, const uint8_t* data, uint16_t len) -> bool {
    // Get GENI service and characteristic
    auto *service = this->parent_->get_service(GRUNDFOS_SERVICE_UUID);
    if (!service) return false;
    
    auto *chr = this->parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
    if (!chr) return false;
    
    // Use transport to write packet (handles splitting if needed)
    auto write_func = [this, chr](const uint8_t* pkt_data, size_t pkt_len) -> bool {
      auto status = esp_ble_gattc_write_char(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          chr->handle,
          pkt_len,
          const_cast<uint8_t*>(pkt_data),
          ESP_GATT_WRITE_TYPE_NO_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      return (status == ESP_OK);
    };
    
    return this->transport_.write_packet(data, len, write_func);
  });
}

void AlphaHwrComponent::loop() {
  // Keep-alive or periodic tasks if needed
}

// Called every 10 seconds by PollingComponent
void AlphaHwrComponent::update() {
  ESP_LOGI(TAG, "update() called - ready: %d, parent: %d, conn_id: 0x%02X", 
           session_.is_ready(), parent_ != nullptr, 
           parent_ ? parent_->get_conn_id() : 0xFF);
  
  if (session_.is_ready() && parent_ && parent_->get_conn_id() != 0xFF) {
    // Poll telemetry first
    telemetry_service_.poll();
    
    // CRITICAL FIX: Space out schedule poll to avoid request collision
    // The pump appears to have trouble handling concurrent Class 10 reads.
    // Delay schedule poll by 500ms to ensure telemetry response completes first.
    this->set_timeout("schedule_poll", 500, [this]() {
      schedule_service_.poll_state();
    });
    
    // Check for timed-out response handlers (2 second timeout)
    transport_.check_timeouts(2000);
  } else {
    ESP_LOGW(TAG, "Skipping polls - not ready");
  }
}

void AlphaHwrComponent::authenticate() {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }
  
  session_.on_authenticating();
  auth_.start();
}

void AlphaHwrComponent::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // Delegate to BLE connection manager
  ble_manager_.handle_gap_event(event, param);
}

void AlphaHwrComponent::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                             esp_ble_gattc_cb_param_t *param) {
  // Delegate to BLE connection manager
  ble_manager_.handle_gattc_event(event, gattc_if, param);
}

}  // namespace alpha_hwr
}  // namespace esphome
