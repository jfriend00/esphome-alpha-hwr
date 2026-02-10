#include "alpha_hwr.h"

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
}

void AlphaHwrComponent::loop() {
  // Keep-alive or periodic tasks if needed
}

// CRC-16-CCITT calculation (base function, no final XOR)
uint16_t AlphaHwrComponent::calc_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;  // Initial value
  for (size_t i = 0; i < len; i++) {
    crc = ((crc << 8) ^ CRC_TABLE[((crc >> 8) ^ data[i]) & 0xFF]) & 0xFFFF;
  }
  return crc;
}

// CRC-16-CCITT calculation with final XOR (used for READ operations)
uint16_t AlphaHwrComponent::calc_crc16_read(const uint8_t *data, size_t len) {
  return calc_crc16(data, len) ^ 0xFFFF;
}

// Build a Class 10 READ request packet
void AlphaHwrComponent::build_class10_read_packet(uint32_t register_addr, uint8_t *packet_out) {
  // Frame structure: [27] [07] [E7] [F8] [0A] [03] [Reg-H] [Reg-M] [Reg-L] [CRC-H] [CRC-L]
  packet_out[0] = 0x27;  // FRAME_START
  packet_out[1] = 0x07;  // Length (7 bytes: E7 F8 0A 03 + 3 register bytes)
  packet_out[2] = 0xE7;  // SERVICE_ID_HIGH
  packet_out[3] = 0xF8;  // Source address
  packet_out[4] = 0x0A;  // CLASS_10
  packet_out[5] = 0x03;  // OpSpec (READ)
  packet_out[6] = (register_addr >> 16) & 0xFF;  // Register high byte
  packet_out[7] = (register_addr >> 8) & 0xFF;   // Register middle byte
  packet_out[8] = register_addr & 0xFF;          // Register low byte
  
  // Calculate CRC over bytes 1-8 (everything after start byte, before CRC)
  uint16_t crc = calc_crc16_read(&packet_out[1], 8);
  packet_out[9] = (crc >> 8) & 0xFF;   // CRC high byte
  packet_out[10] = crc & 0xFF;          // CRC low byte
}

// Send a telemetry read request
void AlphaHwrComponent::send_read_request(uint32_t register_addr) {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }
  
  if (!authenticated_) {
    ESP_LOGD(TAG, "Not authenticated yet, skipping telemetry poll");
    return;
  }
  
  // Get Grundfos service (0xFE5D)
  auto *service = parent_->get_service(GRUNDFOS_SERVICE_UUID);
  if (!service) {
    ESP_LOGW(TAG, "Grundfos service not found for read request");
    return;
  }
  
  auto *chr = parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
  if (!chr) {
    ESP_LOGW(TAG, "GENI characteristic not found");
    return;
  }
  
  // Build the read packet
  uint8_t packet[11];
  build_class10_read_packet(register_addr, packet);
  
  // Log what we're sending
  char hex_str[40];
  sprintf(hex_str, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
          packet[0], packet[1], packet[2], packet[3], packet[4], 
          packet[5], packet[6], packet[7], packet[8], packet[9], packet[10]);
  ESP_LOGD(TAG, "Sending READ request for register 0x%06X: %s", register_addr, hex_str);
  
  // Send the packet
  auto status = esp_ble_gattc_write_char(
      parent_->get_gattc_if(),
      parent_->get_conn_id(),
      chr->handle,
      sizeof(packet),
      packet,
      ESP_GATT_WRITE_TYPE_NO_RSP,
      ESP_GATT_AUTH_REQ_NONE);
  
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send read request: status=%d", status);
  }
}

// Poll telemetry data (called every 10 seconds by update())
void AlphaHwrComponent::poll_telemetry() {
  ESP_LOGI(TAG, "Polling telemetry...");
  
  // Request motor state (0x570045)
  send_read_request(0x570045);
  
  // Small delay between requests
  this->set_timeout(100, [this]() {
    // Request flow/pressure (0x5D0122)
    send_read_request(0x5D0122);
  });
  
  // Another delay
  this->set_timeout(200, [this]() {
    // Request temperature (0x5D012C)
    send_read_request(0x5D012C);
  });
  
  // Request alarms (Obj 88, Sub 0 → 0x580000)
  this->set_timeout(300, [this]() {
    send_read_request(0x580000);
  });
  
  // Request warnings (Obj 88, Sub 11 → 0x58000B)
  this->set_timeout(400, [this]() {
    send_read_request(0x58000B);
  });
}

// Called every 10 seconds by PollingComponent
void AlphaHwrComponent::update() {
  ESP_LOGI(TAG, "update() called - authenticated: %d, parent: %d, conn_id: 0x%02X", 
           authenticated_, parent_ != nullptr, 
           parent_ ? parent_->get_conn_id() : 0xFF);
  
  if (authenticated_ && parent_ && parent_->get_conn_id() != 0xFF) {
    poll_telemetry();
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
  
  subscribed_ = true;
}

void AlphaHwrComponent::authenticate() {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }
  
  ESP_LOGI(TAG, "Starting non-blocking authentication handshake (3-stage sequence)...");
  
  // Reset authentication stage counters
  auth_stage1_count_ = 0;
  auth_stage2_count_ = 0;
  
  // Start Stage 1 immediately
  auth_stage1_legacy_burst(0);
}

void AlphaHwrComponent::auth_stage1_legacy_burst(int repeat_count) {
  if (repeat_count < 3) {
    // Send legacy magic packet
    ESP_LOGD(TAG, "Stage 1: Sending legacy magic packet %d/3", repeat_count + 1);
    send_auth_packet(AUTH_LEGACY, sizeof(AUTH_LEGACY));
    
    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    this->set_timeout(50, [this, repeat_count]() {
      auth_stage1_legacy_burst(repeat_count + 1);
    });
  } else {
    // Stage 1 complete, wait 100ms then start Stage 2 (Python uses 0.1s)
    ESP_LOGD(TAG, "Stage 1 complete, waiting 100ms before Stage 2...");
    this->set_timeout(100, [this]() {
      auth_stage2_class10_burst(0);
    });
  }
}

void AlphaHwrComponent::auth_stage2_class10_burst(int repeat_count) {
  if (repeat_count < 5) {
    // Send Class 10 unlock packet
    ESP_LOGD(TAG, "Stage 2: Sending Class 10 unlock packet %d/5", repeat_count + 1);
    send_auth_packet(AUTH_CLASS10, sizeof(AUTH_CLASS10));
    
    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    this->set_timeout(50, [this, repeat_count]() {
      auth_stage2_class10_burst(repeat_count + 1);
    });
  } else {
    // Stage 2 complete, wait 200ms then start Stage 3 (Python uses 0.2s)
    ESP_LOGD(TAG, "Stage 2 complete, waiting 200ms before Stage 3...");
    this->set_timeout(200, [this]() {
      auth_stage3_extensions();
    });
  }
}

void AlphaHwrComponent::auth_stage3_extensions() {
  ESP_LOGD(TAG, "Stage 3: Sending extension packets...");
  
  // Python sends EXTEND_2 then EXTEND_1 (note the order)
  // See authentication.py lines 344-351
  send_auth_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2));
  send_auth_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1));
  
  // Wait 500ms for final stabilization (Python uses 0.5s)
  this->set_timeout(500, [this]() {
    authenticated_ = true;
    ESP_LOGI(TAG, "✓ Authentication handshake complete - waiting for telemetry...");
  });
}

void AlphaHwrComponent::send_auth_packet(const uint8_t *data, size_t len) {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }
  
  // Get Grundfos service (0xFE5D)
  auto *service = parent_->get_service(GRUNDFOS_SERVICE_UUID);
  
  if (!service) {
    ESP_LOGW(TAG, "Grundfos service not found for auth packet");
    return;
  }
  
  auto *chr = parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
  if (!chr) {
    ESP_LOGW(TAG, "GENI characteristic not found");
    return;
  }
  
  // CRITICAL FIX: Use write without response (ESP_GATT_WRITE_TYPE_NO_RSP)
  // This matches the Python library's behavior (response=False)
  auto status = esp_ble_gattc_write_char(
      parent_->get_gattc_if(),
      parent_->get_conn_id(),
      chr->handle,
      len,
      (uint8_t *)data,
      ESP_GATT_WRITE_TYPE_NO_RSP,  // Write without response - faster, matches Python
      ESP_GATT_AUTH_REQ_NONE);
  
  if (status == ESP_OK) {
    ESP_LOGD(TAG, "Sent auth packet (%zu bytes)", len);
  } else {
    ESP_LOGW(TAG, "Failed to send auth packet: status=%d", status);
  }
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
      
      // Reset discovery and authentication state
      geni_service_found_ = false;
      discovery_retry_count_ = 0;
      auth_started_ = false;
      
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
          geni_service_found_ = true;
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
        
        // Check if this is the start of a new packet (frame byte 0x24 or 0x27)
        if (notify_evt->value[0] == 0x24 || notify_evt->value[0] == 0x27) {
          // Start of new packet - check if we need length info
          if (notify_evt->value_len >= 2) {
            expected_packet_length_ = notify_evt->value[1] + 2;  // Length field + Frame + Length bytes
            ESP_LOGD(TAG, "New packet started, expected total length: %d bytes", expected_packet_length_);
          }
          
          // Clear buffer and start fresh
          reassembly_buffer_.clear();
          reassembly_buffer_.insert(reassembly_buffer_.end(), 
                                   notify_evt->value, 
                                   notify_evt->value + notify_evt->value_len);
          reassembling_ = true;
        } else if (reassembling_) {
          // Continuation of previous packet
          reassembly_buffer_.insert(reassembly_buffer_.end(), 
                                   notify_evt->value, 
                                   notify_evt->value + notify_evt->value_len);
          ESP_LOGD(TAG, "Reassembly: %d/%d bytes", reassembly_buffer_.size(), expected_packet_length_);
        } else {
          // Single packet or unknown format
          decode_packet(notify_evt->value, notify_evt->value_len);
          break;
        }
        
        // Check if we have the complete packet
        if (reassembling_ && reassembly_buffer_.size() >= expected_packet_length_) {
          ESP_LOGD(TAG, "Packet complete, decoding %d bytes", reassembly_buffer_.size());
          decode_packet(reassembly_buffer_.data(), reassembly_buffer_.size());
          reassembling_ = false;
          reassembly_buffer_.clear();
        }
      }
      break;
    }
    
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "Disconnected (reason: 0x%02x)", param->disconnect.reason);
      authenticated_ = false;
      subscribed_ = false;
      auth_started_ = false;  // Reset auth flag on disconnect
      geni_service_found_ = false;
      discovery_retry_count_ = 0;
      reassembling_ = false;  // Reset reassembly state
      reassembly_buffer_.clear();
      break;
    
    default:
      break;
  }
}

float AlphaHwrComponent::read_float_be(uint8_t *data, size_t offset) {
  if (offset + 4 > 255) return 0.0f;
  uint32_t temp = (data[offset] << 24) | (data[offset + 1] << 16) | 
                  (data[offset + 2] << 8) | data[offset + 3];
  float val;
  memcpy(&val, &temp, 4);
  return val;
}

void AlphaHwrComponent::decode_packet(uint8_t *data, size_t len) {
  // Log ALL packets for debugging
  ESP_LOGD(TAG, "Packet received: %d bytes", len);
  if (len > 0 && len <= 50) {
    // Log packet hex for packets up to 50 bytes
    char hex_str[150];
    int pos = 0;
    for (size_t i = 0; i < len && i < 50; i++) {
      pos += sprintf(hex_str + pos, "%02X ", data[i]);
    }
    ESP_LOGD(TAG, "  Hex: %s", hex_str);
  }
  
  if (len < 6) {
    ESP_LOGD(TAG, "  Packet too short (< 6 bytes), ignoring");
    return;
  }
  
  // Log packet structure
  ESP_LOGD(TAG, "  Frame: 0x%02X, Class: 0x%02X, OpSpec: 0x%02X", 
           data[0], data[4], len >= 6 ? data[5] : 0);
  
  // Check frame start (0x24 for response)
  if (data[0] != 0x24) {
    ESP_LOGD(TAG, "  Not a response frame (expected 0x24), ignoring");
    return;
  }
  
  // Check class (0x0A for Class 10)
  if (len < 5 || data[4] != 0x0A) {
    ESP_LOGD(TAG, "  Not Class 10 (got 0x%02X), ignoring", data[4]);
    return;
  }

  uint8_t opspec = data[5];
  ESP_LOGD(TAG, "  Class 10 packet, OpSpec: 0x%02X", opspec);

  // Register READ responses (OpSpec 0x30 = motor, 0x2B = flow, 0x14 = temp)
  if (opspec == 0x30 && len >= 41) {
    // Motor state response: floats start at offset 13
    // [0]=AC voltage, [1]=DC voltage, [2]=current, [3]=power, [5]=RPM, [6]=converter temp
    ESP_LOGI(TAG, "  Motor state response (OpSpec 0x30)");
    
    float voltage_ac = read_float_be(data, 13); // Float[0] at offset 13
    float voltage_dc = read_float_be(data, 17); // Float[1] at offset 17
    float current = read_float_be(data, 21);    // Float[2] at offset 21
    float power = read_float_be(data, 25);      // Float[3] at offset 25
    float rpm = read_float_be(data, 33);        // Float[5] at offset 33
    float converter_temp = read_float_be(data, 37); // Float[6] at offset 37
    
    if (power >= 0 && power <= 1000 && rpm >= 0 && rpm <= 10000) {
      ESP_LOGI(TAG, "✓ Motor: AC=%.1fV, DC=%.1fV, %.2fA, %.1fW, %.0f RPM, %.1f°C", 
               voltage_ac, voltage_dc, current, power, rpm, converter_temp);
      if (voltage_sensor_ != nullptr) voltage_sensor_->publish_state(voltage_ac);
      if (voltage_dc_sensor_ != nullptr && voltage_dc >= 0 && voltage_dc <= 500) {
        voltage_dc_sensor_->publish_state(voltage_dc);
      }
      if (current_sensor_ != nullptr) current_sensor_->publish_state(current);
      if (power_sensor_ != nullptr) power_sensor_->publish_state(power);
      if (rpm_sensor_ != nullptr) rpm_sensor_->publish_state(rpm);
      // Publish converter temperature - even if NaN (shows as "Unavailable" in HA)
      if (temp_converter_sensor_ != nullptr) {
        if (std::isnan(converter_temp) || (converter_temp >= -20 && converter_temp <= 120)) {
          temp_converter_sensor_->publish_state(converter_temp);
        }
      }
    }
  }
  else if (opspec == 0x2B && len >= 45) {
    // Flow/pressure response: floats start at offset 13
    // [6]=flow at offset 37, [7]=head at offset 41, [8]=inlet_pressure at offset 45, [9]=outlet_pressure at offset 49
    ESP_LOGI(TAG, "  Flow/pressure response (OpSpec 0x2B)");
    
    float flow = read_float_be(data, 37);
    float head = read_float_be(data, 41);
    
    // Inlet and outlet pressure may not always be present
    float inlet_pressure = NAN;
    float outlet_pressure = NAN;
    if (len >= 49) {
      inlet_pressure = read_float_be(data, 45);
    }
    if (len >= 53) {
      outlet_pressure = read_float_be(data, 49);
    }
    
    if (flow >= 0 && flow <= 100 && head >= 0 && head <= 50) {
      ESP_LOGI(TAG, "✓ Flow/Head: %.3f m³/h, %.2f m, P_in=%.2f bar, P_out=%.2f bar", 
               flow, head, inlet_pressure, outlet_pressure);
      if (flow_sensor_ != nullptr) flow_sensor_->publish_state(flow);
      if (head_sensor_ != nullptr) head_sensor_->publish_state(head);
      
      // Publish pressure sensors - even if NaN (shows as "Unavailable" in HA)
      if (inlet_pressure_sensor_ != nullptr) {
        if (std::isnan(inlet_pressure) || (inlet_pressure >= 0 && inlet_pressure <= 20)) {
          inlet_pressure_sensor_->publish_state(inlet_pressure);
        }
      }
      if (outlet_pressure_sensor_ != nullptr) {
        if (std::isnan(outlet_pressure) || (outlet_pressure >= 0 && outlet_pressure <= 20)) {
          outlet_pressure_sensor_->publish_state(outlet_pressure);
        }
      }
    } else {
      ESP_LOGW(TAG, "  Invalid flow/head values (flow=%.3f, head=%.2f)", flow, head);
    }
  }
  else if (opspec == 0x14 && len >= 25) {
    // Temperature response: floats start at offset 13
    // [0]=media temp, [1]=PCB temp, [2]=control box temp
    ESP_LOGI(TAG, "  Temperature response (OpSpec 0x14)");
    
    float media_temp = read_float_be(data, 13);     // Float[0] at offset 13
    float pcb_temp = read_float_be(data, 17);       // Float[1] at offset 17
    float control_box_temp = read_float_be(data, 21); // Float[2] at offset 21
    
    ESP_LOGI(TAG, "✓ Temps: Media=%.1f°C, PCB=%.1f°C, Box=%.1f°C", media_temp, pcb_temp, control_box_temp);
    
    if (media_temp >= -20 && media_temp <= 100 && temp_media_sensor_ != nullptr) {
      temp_media_sensor_->publish_state(media_temp);
    }
    if (pcb_temp >= -20 && pcb_temp <= 150 && temp_pcb_sensor_ != nullptr) {
      temp_pcb_sensor_->publish_state(pcb_temp);
    }
    if (control_box_temp >= -20 && control_box_temp <= 150 && temp_control_box_sensor_ != nullptr) {
      temp_control_box_sensor_->publish_state(control_box_temp);
    }
  }
  // OpSpec 0x13 = Alarms/Warnings response
  else if (opspec == 0x13 && len >= 10) {
    // Frame format: [STX][LEN][DST][SRC][Class=0x0A][OpSpec=0x13][Sub_H][Sub_L][Obj_H][Obj_L][...DATA...][CRC]
    // Extract Sub ID and Obj ID to determine if this is alarms or warnings
    uint16_t sub_id = (data[6] << 8) | data[7];
    uint16_t obj_id = (data[8] << 8) | data[9];
    
    // Obj 88 (0x0058), Sub 0 = Alarms
    // Obj 88 (0x0058), Sub 11 (0x000B) = Warnings
    if (obj_id == 0x0058) {
      bool is_alarms = (sub_id == 0x0000);
      bool is_warnings = (sub_id == 0x000B);
      
      if (is_alarms || is_warnings) {
        const char* type_str = is_alarms ? "Alarms" : "Warnings";
        ESP_LOGI(TAG, "  %s response (OpSpec 0x13, Obj 88, Sub %d)", type_str, sub_id);
        
        // Parse uint16 array starting at offset 10
        std::vector<uint16_t> codes;
        for (size_t i = 10; i + 1 < len - 2; i += 2) {  // -2 for CRC at end
          uint16_t code = (data[i] << 8) | data[i + 1];
          if (code != 0) {  // Filter out zero codes
            codes.push_back(code);
          }
        }
        
        // Build comma-separated string of codes
        std::string codes_str;
        if (codes.empty()) {
          codes_str = "None";
        } else {
          for (size_t i = 0; i < codes.size(); i++) {
            if (i > 0) codes_str += ", ";
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", codes[i]);
            codes_str += buf;
          }
        }
        
        ESP_LOGI(TAG, "✓ %s: %s", type_str, codes_str.c_str());
        
        // Publish to appropriate text sensor
#ifdef USE_TEXT_SENSOR
        if (is_alarms && alarms_sensor_ != nullptr) {
          alarms_sensor_->publish_state(codes_str);
        }
        if (is_warnings && warnings_sensor_ != nullptr) {
          warnings_sensor_->publish_state(codes_str);
        }
#endif
      } else {
        ESP_LOGD(TAG, "  Unknown Obj 88 Sub ID: 0x%04X", sub_id);
      }
    } else {
      ESP_LOGD(TAG, "  OpSpec 0x13 with unexpected Obj ID: 0x%04X (expected 0x0058)", obj_id);
    }
  }
  // OpSpec 0x0E = Passive Notifications (streaming telemetry - legacy format)
  else if (opspec == 0x0E && len >= 10) {
    uint16_t sub_id = (data[6] << 8) | data[7];
    uint16_t obj_id = (data[8] << 8) | data[9];
    
    ESP_LOGD(TAG, "  Passive telemetry notification: Sub=0x%04X, Obj=0x%04X", sub_id, obj_id);
    
    // Standard Motor State (Sub 0x0045, Obj 0x0057)
    if (sub_id == 0x0045 && obj_id == 0x0057 && len >= 38) {
      float voltage = read_float_be(data, 10);     // Offset 10 (Float[0])
      float current = read_float_be(data, 18);     // Offset 18 (Float[2])
      float power = read_float_be(data, 26);       // Offset 26 (Float[4])
      float rpm = read_float_be(data, 30);         // Offset 30 (Float[5])
      float converter_temp = read_float_be(data, 34); // Offset 34 (Float[6])
      ESP_LOGI(TAG, "✓ Motor (passive): %.1fV, %.2fA, %.1fW, %.0f RPM, %.1f°C", voltage, current, power, rpm, converter_temp);
      if (voltage_sensor_ != nullptr) voltage_sensor_->publish_state(voltage);
      if (current_sensor_ != nullptr) current_sensor_->publish_state(current);
      if (power_sensor_ != nullptr) power_sensor_->publish_state(power);
      if (rpm_sensor_ != nullptr) rpm_sensor_->publish_state(rpm);
      if (converter_temp >= -20 && converter_temp <= 120 && temp_converter_sensor_ != nullptr) {
        temp_converter_sensor_->publish_state(converter_temp);
      }
    }
    // Standard Flow/Head (Sub 0x0122, Obj 0x005D = Decimal Sub 290, Obj 93)
    else if (sub_id == 0x0122 && obj_id == 0x005D && len >= 18) {
      float flow = read_float_be(data, 10);
      float head = read_float_be(data, 14);
      ESP_LOGI(TAG, "✓ Flow/Head (passive): %.3f m³/h, %.2f m", flow, head);
      if (flow_sensor_ != nullptr) flow_sensor_->publish_state(flow);
      if (head_sensor_ != nullptr) head_sensor_->publish_state(head);
    }
    // Standard Temperature (Sub 0x012C, Obj 0x005D = Decimal Sub 300, Obj 93)
    else if (sub_id == 0x012C && obj_id == 0x005D && len >= 22) {
      float media_temp = read_float_be(data, 10);      // Offset 10 (Float[0])
      float pcb_temp = read_float_be(data, 14);        // Offset 14 (Float[1])
      float control_box_temp = read_float_be(data, 18); // Offset 18 (Float[2])
      ESP_LOGI(TAG, "✓ Temps (passive): Media=%.1f°C, PCB=%.1f°C, Box=%.1f°C", media_temp, pcb_temp, control_box_temp);
      if (media_temp >= -20 && media_temp <= 100 && temp_media_sensor_ != nullptr) {
        temp_media_sensor_->publish_state(media_temp);
      }
      if (pcb_temp >= -20 && pcb_temp <= 150 && temp_pcb_sensor_ != nullptr) {
        temp_pcb_sensor_->publish_state(pcb_temp);
      }
      if (control_box_temp >= -20 && control_box_temp <= 150 && temp_control_box_sensor_ != nullptr) {
        temp_control_box_sensor_->publish_state(control_box_temp);
      }
    }
    else {
      ESP_LOGD(TAG, "  Unknown passive notification (Sub=0x%04X, Obj=0x%04X)", sub_id, obj_id);
    }
  }
  // OpSpec 0x09 = Alarm/Warning response (Active Query Response format)
  // This uses the same format as other active query responses (0x30, 0x2B, 0x14)
  // Structure: [OpSpec][Seq (2B)][ID (2B)][Res (2B)][DataLen (1B)][Data (uint16 array)]
  else if (opspec == 0x09) {
    ESP_LOGD(TAG, "  Alarm/Warning response (OpSpec 0x09)");
    
    if (len >= 13) {  // Minimum: OpSpec + Seq + ID + Res + DataLen = 7 bytes + 4-byte header + 2-byte CRC
      uint8_t data_len = data[12];  // DataLen byte at offset 12
      
      // Parse alarm codes from uint16 array (big-endian)
      std::vector<uint16_t> codes;
      for (size_t i = 13; i + 1 < len - 2 && i < 13 + data_len; i += 2) {
        uint16_t code = (data[i] << 8) | data[i + 1];
        if (code != 0) {  // Filter out zero codes (mean "no alarm/warning")
          codes.push_back(code);
        }
      }
      
      if (codes.empty()) {
        ESP_LOGI(TAG, "  ✓ No active alarms/warnings");
        // Update text sensors with empty string
        #ifdef USE_TEXT_SENSOR
        if (this->alarms_sensor_ != nullptr) {
          this->alarms_sensor_->publish_state("");
        }
        if (this->warnings_sensor_ != nullptr) {
          this->warnings_sensor_->publish_state("");
        }
        #endif
      } else {
        // Format codes as comma-separated string
        std::string codes_str;
        for (size_t i = 0; i < codes.size(); i++) {
          if (i > 0) codes_str += ",";
          codes_str += std::to_string(codes[i]);
        }
        ESP_LOGW(TAG, "  ⚠️ Active alarm/warning codes: %s", codes_str.c_str());
        
        // Update appropriate text sensor based on which register was queried
        // Note: We'd need to track which query this is responding to in production
        // For now, just log - full implementation would require request tracking
        #ifdef USE_TEXT_SENSOR
        // TODO: Differentiate between alarm (0x580000) and warning (0x58000B) responses
        #endif
      }
    }
  }
  else {
    // Log unhandled packets prominently for reverse engineering
    ESP_LOGW(TAG, "  ⚠️ UNHANDLED PACKET: OpSpec=0x%02X, len=%d", opspec, len);
    
    // Log full hex for analysis
    if (len <= 50) {
      char hex_str[150];
      int pos = 0;
      for (size_t i = 0; i < len && i < 50; i++) {
        pos += sprintf(hex_str + pos, "%02X ", data[i]);
      }
      ESP_LOGW(TAG, "  Full Hex: %s", hex_str);
    }
    
    // If longer, log first 50 bytes
    if (len > 50) {
      char hex_str[150];
      int pos = 0;
      for (size_t i = 0; i < 50; i++) {
        pos += sprintf(hex_str + pos, "%02X ", data[i]);
      }
      ESP_LOGW(TAG, "  First 50 bytes: %s... (%d total)", hex_str, len);
    }
  }
}

}  // namespace alpha_hwr
}  // namespace esphome
