#include "alpha_hwr.h"
#include "frame_parser.h"
#include "telemetry_decoder.h"

namespace esphome {
namespace alpha_hwr {

// Static method to validate if a BLE device is an ALPHA HWR pump
// Primary method: Check Grundfos Company ID in manufacturer data
// Secondary fallback: Check for GENI service UUID
bool AlphaHwrComponent::is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device) {
  // Primary Discovery Method: Match by Grundfos Company ID in manufacturer data
  // This is the most reliable method and guaranteed to be present on all ALPHA HWR pumps
  const auto &mfg_datas = device.get_manufacturer_datas();
  
  for (const auto &mfg_data : mfg_datas) {
    // Check if this is a 16-bit UUID matching the Grundfos Company ID
    esp_bt_uuid_t uuid = mfg_data.uuid.get_uuid();
    if (uuid.len == ESP_UUID_LEN_16 && uuid.uuid.uuid16 == GRUNDFOS_COMPANY_ID) {
      const auto &service_data = mfg_data.data;
      
      // Validate service data structure:
      // Byte 0-1: Frame header
      // Byte 2: Product Family (0x34 = ALPHA)
      // Byte 3: Product Type (0x07 = HWR)
      // Byte 4+: Additional data
      if (service_data.size() >= 5 &&
          service_data[2] == PRODUCT_FAMILY_ALPHA && 
          service_data[3] == PRODUCT_TYPE_HWR) {
        ESP_LOGI(TAG, "Found ALPHA HWR pump via Grundfos Company ID (primary method)");
        return true;
      }
    }
  }
  
  // Secondary Discovery Method: Check for Grundfos service UUID (0xFE5D)
  // The GENI characteristic is actually inside this service
  for (auto &service_uuid : device.get_service_uuids()) {
    if (service_uuid == GRUNDFOS_SERVICE_UUID) {
      ESP_LOGI(TAG, "Found ALPHA HWR pump via Grundfos service UUID (0xFE5D)");
      return true;
    }
  }
  
  return false;
}

void AlphaHwrComponent::dump_services() {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }
  
  ESP_LOGI(TAG, "========== DISCOVERED SERVICES ==========");
  
  // Check if we can access services vector directly
  // Try iterating through common service UUIDs to see what's available
  ESP_LOGI(TAG, "Checking for known BLE services...");
  
  // Try Generic Access (0x1800)
  auto *gap_service = parent_->get_service(0x1800);
  if (gap_service) {
    ESP_LOGI(TAG, "  ✓ Generic Access (0x1800)");
    ESP_LOGI(TAG, "    Handles: 0x%04x - 0x%04x", gap_service->start_handle, gap_service->end_handle);
  }
  
  // Try Generic Attribute (0x1801)
  auto *gatt_service = parent_->get_service(0x1801);
  if (gatt_service) {
    ESP_LOGI(TAG, "  ✓ Generic Attribute (0x1801)");
    ESP_LOGI(TAG, "    Handles: 0x%04x - 0x%04x", gatt_service->start_handle, gatt_service->end_handle);
  }
  
  // Try Grundfos Service (0xFE5D) - this contains the GENI characteristic
  auto *geni_service = parent_->get_service(GRUNDFOS_SERVICE_UUID);
  if (geni_service) {
    ESP_LOGI(TAG, "  ✓ Grundfos Service (0xFE5D): %s", GRUNDFOS_SERVICE_UUID.to_string().c_str());
    ESP_LOGI(TAG, "    Handles: 0x%04x - 0x%04x", 
             geni_service->start_handle, geni_service->end_handle);
    
    // Try to parse characteristics if not already done
    if (geni_service->characteristics.empty()) {
      ESP_LOGI(TAG, "    Parsing characteristics...");
      geni_service->parse_characteristics();
    }
    
    // List all characteristics
    if (!geni_service->characteristics.empty()) {
      ESP_LOGI(TAG, "    Characteristics (%d found):", geni_service->characteristics.size());
      for (auto *chr : geni_service->characteristics) {
        ESP_LOGI(TAG, "      - UUID: %s, Handle: 0x%04x, Props: 0x%02x", 
                 chr->uuid.to_string().c_str(), chr->handle, chr->properties);
      }
    } else {
      ESP_LOGW(TAG, "    No characteristics found in GENI service!");
    }
  } else {
    ESP_LOGW(TAG, "  ✗ Grundfos Service (0xFE5D) NOT found!");
    ESP_LOGW(TAG, "    Expected UUID: %s", GRUNDFOS_SERVICE_UUID.to_string().c_str());
    
    // Try alternative UUID formats
    // Sometimes UUIDs might be reported differently
    ESP_LOGI(TAG, "  Trying alternative GENI UUID formats...");
    
    // Try as 16-bit UUID (0xFDD0)
    auto *geni_16bit = parent_->get_service(0xFDD0);
    if (geni_16bit) {
      ESP_LOGI(TAG, "  ✓ Found service with 16-bit UUID 0xFDD0!");
      ESP_LOGI(TAG, "    Full UUID: %s", geni_16bit->uuid.to_string().c_str());
      ESP_LOGI(TAG, "    Handles: 0x%04x - 0x%04x", geni_16bit->start_handle, geni_16bit->end_handle);
    }
  }
  
  ESP_LOGI(TAG, "========================================");
}

void AlphaHwrComponent::init_security() {
  if (!pairing_enabled_) {
    ESP_LOGI(TAG, "BLE pairing disabled - using passive telemetry only");
    return;
  }
  
  ESP_LOGI(TAG, "Configuring BLE Security for Pairing/Bonding...");
  
  // Set IO capabilities to "No Input No Output" (Just Works pairing)
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
  
  // Set authentication requirements: Bonding + Secure Connections
  uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  
  // Set maximum/minimum encryption key size (16 bytes)
  uint8_t key_size = 16;
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MIN_KEY_SIZE, &key_size, sizeof(uint8_t));
  
  // Enable key distribution for encryption and identity
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
  
  ESP_LOGI(TAG, "BLE security configuration complete");
}

void AlphaHwrComponent::setup() {
  ESP_LOGI(TAG, "Alpha HWR Component setup");
  init_security();
  
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
    telemetry_service_.poll();
  } else {
    ESP_LOGW(TAG, "Skipping telemetry poll - not ready");
  }
}

void AlphaHwrComponent::subscribe_to_notifications() {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }
  
  // Get Grundfos service (0xFE5D)
  auto *service = parent_->get_service(GRUNDFOS_SERVICE_UUID);
  
  if (!service) {
    ESP_LOGW(TAG, "Grundfos service not found for notification subscription");
    return;
  }
  
  auto *chr = parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
  if (!chr) {
    ESP_LOGW(TAG, "GENI characteristic not found");
    return;
  }
  
  // Register for notifications (tells ESP-IDF we want to receive them)
  auto status = esp_ble_gattc_register_for_notify(parent_->get_gattc_if(), 
                                                   parent_->get_remote_bda(), 
                                                   chr->handle);
  if (status) {
    ESP_LOGW(TAG, "Failed to register for notifications: %d", status);
    return;
  }
  
  ESP_LOGI(TAG, "Registered for GENI notifications (local)");
  
  // Now write to the CCCD descriptor to enable notifications on the server side
  // CCCD handle is typically characteristic handle + 1
  uint16_t cccd_handle = chr->handle + 1;
  uint8_t notify_enable[] = {0x01, 0x00};  // 0x0001 = enable notifications
  
  ESP_LOGI(TAG, "Writing to CCCD descriptor (handle 0x%04x) to enable notifications...", cccd_handle);
  
  status = esp_ble_gattc_write_char_descr(
      parent_->get_gattc_if(),
      parent_->get_conn_id(),
      cccd_handle,
      sizeof(notify_enable),
      notify_enable,
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);
  
  if (status) {
    ESP_LOGW(TAG, "Failed to write CCCD descriptor: %d", status);
  } else {
    ESP_LOGI(TAG, "CCCD write successful - notifications should now be enabled");
  }
  
  session_.on_subscribed();
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
  switch (event) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      auto &auth_cmpl = param->ble_security.auth_cmpl;
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              auth_cmpl.bd_addr[0], auth_cmpl.bd_addr[1], auth_cmpl.bd_addr[2],
              auth_cmpl.bd_addr[3], auth_cmpl.bd_addr[4], auth_cmpl.bd_addr[5]);
      
      if (auth_cmpl.success) {
        ESP_LOGI(TAG, "✓ BLE authentication complete (Pairing/Bonding successful)!");
        ESP_LOGI(TAG, "  Device: %s", addr_str);
        ESP_LOGI(TAG, "  Auth mode: 0x%02x", auth_cmpl.auth_mode);
        ESP_LOGD(TAG, "  Key present: 0x%02x", auth_cmpl.key_present);
        ESP_LOGD(TAG, "  Key type: 0x%02x", auth_cmpl.key_type);
        if (pairing_status_sensor_ != nullptr) {
          pairing_status_sensor_->publish_state(true);
        }
      } else {
        // Decode failure reason for better debugging
        const char *fail_reason = "Unknown";
        switch (auth_cmpl.fail_reason) {
          case ESP_AUTH_SMP_PASSKEY_FAIL: fail_reason = "Passkey Entry Failed"; break;
          case ESP_AUTH_SMP_OOB_FAIL: fail_reason = "OOB Data Not Available"; break;
          case ESP_AUTH_SMP_PAIR_AUTH_FAIL: fail_reason = "Authentication Requirements Not Met"; break;
          case ESP_AUTH_SMP_CONFIRM_VALUE_FAIL: fail_reason = "Confirm Value Mismatch"; break;
          case ESP_AUTH_SMP_PAIR_NOT_SUPPORT: fail_reason = "Pairing Not Supported"; break;
          case ESP_AUTH_SMP_ENC_KEY_SIZE: fail_reason = "Encryption Key Size Too Small"; break;
          case ESP_AUTH_SMP_INVALID_CMD: fail_reason = "Invalid SMP Command"; break;
          case ESP_AUTH_SMP_UNKNOWN_ERR: fail_reason = "Unspecified Error"; break;
          case ESP_AUTH_SMP_REPEATED_ATTEMPT: fail_reason = "Repeated Pairing Attempts"; break;
          case ESP_AUTH_SMP_INVALID_PARAMETERS: fail_reason = "Invalid Parameters"; break;
          case ESP_AUTH_SMP_DHKEY_CHK_FAIL: fail_reason = "DHKey Check Failed"; break;
          case ESP_AUTH_SMP_NUM_COMP_FAIL: fail_reason = "Numeric Comparison Failed"; break;
          case ESP_AUTH_SMP_BR_PARING_IN_PROGR: fail_reason = "BR/EDR Pairing In Progress"; break;
          case ESP_AUTH_SMP_XTRANS_DERIVE_NOT_ALLOW: fail_reason = "Cross-Transport Key Derivation Not Allowed"; break;
          case ESP_AUTH_SMP_INTERNAL_ERR: fail_reason = "Internal Error"; break;
          case ESP_AUTH_SMP_UNKNOWN_IO: fail_reason = "Unknown IO Capability"; break;
          case ESP_AUTH_SMP_INIT_FAIL: fail_reason = "Pairing Initiation Failed"; break;
          case ESP_AUTH_SMP_CONFIRM_FAIL: fail_reason = "Confirmation Failed"; break;
          case ESP_AUTH_SMP_BUSY: fail_reason = "Security Manager Busy"; break;
          case ESP_AUTH_SMP_ENC_FAIL: fail_reason = "Encryption Start Failed"; break;
          case ESP_AUTH_SMP_STARTED: fail_reason = "Pairing Already Started"; break;
          case ESP_AUTH_SMP_RSP_TIMEOUT: fail_reason = "Response Timeout"; break;
          case ESP_AUTH_SMP_DIV_NOT_AVAIL: fail_reason = "Diversifier Not Available"; break;
          case ESP_AUTH_SMP_UNSPEC_ERR: fail_reason = "Unspecified Failure"; break;
          case ESP_AUTH_SMP_CONN_TOUT: fail_reason = "Connection Timeout"; break;
          default: fail_reason = "Other"; break;
        }
        ESP_LOGW(TAG, "✗ BLE authentication failed!");
        ESP_LOGW(TAG, "  Device: %s", addr_str);
        ESP_LOGW(TAG, "  Failure reason: %s (0x%02x)", fail_reason, auth_cmpl.fail_reason);
        ESP_LOGW(TAG, "  Auth mode: 0x%02x", auth_cmpl.auth_mode);
        if (pairing_status_sensor_ != nullptr) {
          pairing_status_sensor_->publish_state(false);
        }
      }
      break;
    }
      
    case ESP_GAP_BLE_SEC_REQ_EVT: {
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      ESP_LOGI(TAG, "BLE security request from device %s - accepting", addr_str);
      // Initiate pairing response
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    }
      
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.key_notif.bd_addr[0], param->ble_security.key_notif.bd_addr[1],
              param->ble_security.key_notif.bd_addr[2], param->ble_security.key_notif.bd_addr[3],
              param->ble_security.key_notif.bd_addr[4], param->ble_security.key_notif.bd_addr[5]);
      ESP_LOGI(TAG, "BLE passkey notification from %s: %06d", addr_str, param->ble_security.key_notif.passkey);
      ESP_LOGI(TAG, "  Note: Using 'Just Works' pairing - passkey is for display only");
      break;
    }
      
    case ESP_GAP_BLE_KEY_EVT:
      ESP_LOGD(TAG, "BLE key event (key exchange in progress)");
      ESP_LOGD(TAG, "  Key type: 0x%02x", param->ble_security.ble_key.key_type);
      break;
      
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
      if (param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "BLE bond removed successfully");
      } else {
        ESP_LOGW(TAG, "BLE bond removal failed: status=%d", param->remove_bond_dev_cmpl.status);
      }
      break;
      
    case ESP_GAP_BLE_NC_REQ_EVT: {
      // Numeric Comparison request (for Numeric Comparison pairing method)
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      ESP_LOGI(TAG, "BLE numeric comparison request from %s", addr_str);
      ESP_LOGI(TAG, "  Auto-accepting (Just Works mode)");
      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
      break;
    }
      
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
      // Passkey Entry request - should not happen with Just Works
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      ESP_LOGW(TAG, "BLE passkey entry request from %s - unexpected in Just Works mode!", addr_str);
      break;
    }
      
    default:
      // Don't log all GAP events to reduce noise
      break;
  }
}

void AlphaHwrComponent::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                             esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      ESP_LOGI(TAG, "BLE connection opened. Pairing enabled: %s", pairing_enabled_ ? "YES" : "NO");
      
      // Notify session of connection
      session_.on_connected();
      
      // Reset discovery retry counter
      discovery_retry_count_ = 0;
      
      // Only request encryption/pairing if explicitly enabled
      if (pairing_enabled_) {
        ESP_LOGI(TAG, "Requesting encryption/pairing...");
        // Request encryption on this connection (triggers pairing if not already bonded)
        esp_err_t ret = esp_ble_set_encryption(parent_->get_remote_bda(), ESP_BLE_SEC_ENCRYPT);
        if (ret != ESP_OK) {
          ESP_LOGW(TAG, "✗ Failed to request encryption: 0x%x", ret);
        }
      } else {
        ESP_LOGI(TAG, "Skipping encryption request - pairing disabled");
      }
      
      // Update connection parameters for better stability
      {
        esp_ble_conn_update_params_t conn_params;
        memcpy(conn_params.bda, parent_->get_remote_bda(), 6);
        conn_params.min_int = 24;      // 30ms
        conn_params.max_int = 40;      // 50ms 
        conn_params.latency = 0;
        conn_params.timeout = 400;     // 4s
        esp_ble_gap_update_conn_params(&conn_params);
      }
      
      // Wait for encryption/pairing to stabilize before discovery
      this->set_timeout(POST_CONNECT_DELAY_MS, [this]() {
        ESP_LOGI(TAG, "Starting service discovery...");
      });
      break;
    }
      
    case ESP_GATTC_SEARCH_RES_EVT: {
      // Log ALL services as they are discovered
      auto *search_res = &param->search_res;
      char uuid_buf[64];
      if (search_res->srvc_id.uuid.len == ESP_UUID_LEN_16) {
        sprintf(uuid_buf, "0x%04X", search_res->srvc_id.uuid.uuid.uuid16);
      } else if (search_res->srvc_id.uuid.len == ESP_UUID_LEN_32) {
        sprintf(uuid_buf, "0x%08X", search_res->srvc_id.uuid.uuid.uuid32);
      } else {
        sprintf(uuid_buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                search_res->srvc_id.uuid.uuid.uuid128[15], search_res->srvc_id.uuid.uuid.uuid128[14],
                search_res->srvc_id.uuid.uuid.uuid128[13], search_res->srvc_id.uuid.uuid.uuid128[12],
                search_res->srvc_id.uuid.uuid.uuid128[11], search_res->srvc_id.uuid.uuid.uuid128[10],
                search_res->srvc_id.uuid.uuid.uuid128[9], search_res->srvc_id.uuid.uuid.uuid128[8],
                search_res->srvc_id.uuid.uuid.uuid128[7], search_res->srvc_id.uuid.uuid.uuid128[6],
                search_res->srvc_id.uuid.uuid.uuid128[5], search_res->srvc_id.uuid.uuid.uuid128[4],
                search_res->srvc_id.uuid.uuid.uuid128[3], search_res->srvc_id.uuid.uuid.uuid128[2],
                search_res->srvc_id.uuid.uuid.uuid128[1], search_res->srvc_id.uuid.uuid.uuid128[0]);
      }
      ESP_LOGI(TAG, ">>> RAW SERVICE DISCOVERED: %s (handles 0x%04x-0x%04x)", 
               uuid_buf, search_res->start_handle, search_res->end_handle);
      break;
    }
      
    case ESP_GATTC_SEARCH_CMPL_EVT:
      ESP_LOGI(TAG, "Service discovery complete (attempt %d/%d). Checking for GENI service...", 
               discovery_retry_count_ + 1, MAX_DISCOVERY_RETRIES);
      
      // Dump all services for debugging
      dump_services();
      
      // Check for our expected Grundfos service (0xFE5D)
      if (parent_) {
        auto *service = parent_->get_service(GRUNDFOS_SERVICE_UUID);
        
        if (service) {
          session_.on_service_found();
          ESP_LOGI(TAG, "✓ Grundfos service found!");
          ESP_LOGI(TAG, "  UUID: %s", service->uuid.to_string().c_str());
          ESP_LOGI(TAG, "  Start Handle: 0x%04x, End Handle: 0x%04x", 
                   service->start_handle, service->end_handle);
          
          // Check for GENI characteristic
          auto *chr = parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
                    if (chr) {
                      ESP_LOGI(TAG, "✓ GENI characteristic found. Enabling notifications...");
                      
                      // Step 1: Enable notifications (subscribe)
                      subscribe_to_notifications();
                      
                      // Step 2: Wait for pump to stabilize, then authenticate
                      this->set_timeout(2000, [this]() {
                        ESP_LOGI(TAG, "Pump stabilized. Starting authentication...");
                        authenticate();
                      });
                    }
           else {
            ESP_LOGW(TAG, "✗ GENI characteristic NOT found!");
            ESP_LOGW(TAG, "Expected UUID: %s", GENI_CHAR_UUID.to_string().c_str());
          }
        } else {
          // Grundfos service NOT found - implement retry logic
          ESP_LOGW(TAG, "✗ Grundfos service (0xFE5D) NOT found!");
          
          if (discovery_retry_count_ < MAX_DISCOVERY_RETRIES) {
            discovery_retry_count_++;
            ESP_LOGW(TAG, "Retrying service discovery in %dms (attempt %d/%d)...", 
                     DISCOVERY_RETRY_DELAY_MS, discovery_retry_count_ + 1, MAX_DISCOVERY_RETRIES);
            
            // Schedule a retry
            this->set_timeout(DISCOVERY_RETRY_DELAY_MS, [this, gattc_if]() {
              ESP_LOGI(TAG, "Triggering service discovery retry...");
              esp_ble_gattc_search_service(gattc_if, this->parent_->get_conn_id(), nullptr);
            });
          } else {
            ESP_LOGE(TAG, "Failed to find Grundfos service after %d attempts!", MAX_DISCOVERY_RETRIES);
            ESP_LOGW(TAG, "This may indicate:");
            ESP_LOGW(TAG, "  1. Device is not an ALPHA HWR pump");
            ESP_LOGW(TAG, "  2. Different firmware version with different service layout");
            ESP_LOGW(TAG, "  3. BLE connection/discovery issue");
          }
        }
      }
      break;
    
    case ESP_GATTC_WRITE_DESCR_EVT: {
      auto *write_descr = &param->write;
      if (write_descr->status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "✓ CCCD descriptor write successful - notifications enabled on server");
        // NOTE: Don't authenticate here - wait for REG_FOR_NOTIFY_EVT which confirms
        // that the notification system is fully ready
      } else {
        ESP_LOGW(TAG, "✗ CCCD descriptor write failed: status=%d", write_descr->status);
      }
      break;
    }
    
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      auto *reg_notify = &param->reg_for_notify;
      if (reg_notify->status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "✓ Notification registration confirmed (handle 0x%04x)", reg_notify->handle);
      } else {
        ESP_LOGW(TAG, "✗ Notification registration failed: status=%d", reg_notify->status);
      }
      break;
    }
    
    case ESP_GATTC_NOTIFY_EVT: {
      auto *notify_evt = &param->notify;
      if (notify_evt->value_len > 0) {
        ESP_LOGD(TAG, "Received notification, %d bytes", notify_evt->value_len);
        // Pass to transport for reassembly (callback will invoke decode_packet when complete)
        transport_.on_notification(notify_evt->value, notify_evt->value_len);
      }
      break;
    }
    
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "Disconnected (reason: 0x%02x)", param->disconnect.reason);
      session_.on_disconnected();
      discovery_retry_count_ = 0;
      transport_.reset();  // Reset transport state on disconnect
      break;
    
    default:
      break;
  }
}

}  // namespace alpha_hwr
}  // namespace esphome
