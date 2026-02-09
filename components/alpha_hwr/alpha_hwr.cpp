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

void AlphaHwrComponent::setup() {
  ESP_LOGI(TAG, "Alpha HWR Component setup");
  if (parent_) {
    ESP_LOGI(TAG, "Parent BLE client is available");
  } else {
    ESP_LOGW(TAG, "Parent BLE client is NULL!");
  }
  
  // ===========================================================================
  // EXPERIMENT 4: Configure BLE Security/Pairing
  // ===========================================================================
  // HYPOTHESIS: The Grundfos pump firmware requires BLE encryption/bonding
  // before allowing service discovery. Python/Bleak handles this transparently
  // through the OS Bluetooth stack, but ESP32/ESPHome may need explicit config.
  // ===========================================================================
  
  ESP_LOGI(TAG, "==================================================");
  ESP_LOGI(TAG, "EXPERIMENT 4: Configuring BLE Security");
  ESP_LOGI(TAG, "The pump may require encryption/bonding before discovery");
  ESP_LOGI(TAG, "==================================================");
  
  // Set IO capabilities to "No Input No Output" (Just Works pairing)
  // This is the most permissive mode and doesn't require user interaction
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  esp_err_t ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ Set IO capability: No Input No Output (Just Works)");
  } else {
    ESP_LOGW(TAG, "✗ Failed to set IO capability: 0x%x", ret);
  }
  
  // Set authentication requirements: Bonding + Secure Connections
  // ESP_LE_AUTH_REQ_SC_BOND = Secure Connections with bonding
  // This ensures encrypted connection with key storage
  uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
  ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ Set auth requirement: Secure Connections + Bonding");
  } else {
    ESP_LOGW(TAG, "✗ Failed to set auth requirement: 0x%x", ret);
  }
  
  // Set maximum encryption key size (16 bytes)
  uint8_t key_size = 16;
  ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ Set max encryption key size: 16 bytes");
  } else {
    ESP_LOGW(TAG, "✗ Failed to set key size: 0x%x", ret);
  }
  
  // Set minimum encryption key size (16 bytes - most secure)
  ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MIN_KEY_SIZE, &key_size, sizeof(uint8_t));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ Set min encryption key size: 16 bytes");
  } else {
    ESP_LOGW(TAG, "✗ Failed to set min key size: 0x%x", ret);
  }
  
  // Enable all key distribution for both initiator and responder
  // This ensures we can receive and generate all necessary keys for secure pairing
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  
  ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ Set initiator key distribution");
  } else {
    ESP_LOGW(TAG, "✗ Failed to set initiator keys: 0x%x", ret);
  }
  
  ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "✓ Set responder key distribution");
  } else {
    ESP_LOGW(TAG, "✗ Failed to set responder keys: 0x%x", ret);
  }
  
  ESP_LOGI(TAG, "BLE security configuration complete");
  ESP_LOGI(TAG, "==================================================");
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
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "✓ BLE authentication complete!");
        ESP_LOGI(TAG, "  Device address: %02x:%02x:%02x:%02x:%02x:%02x",
                 param->ble_security.auth_cmpl.bd_addr[0],
                 param->ble_security.auth_cmpl.bd_addr[1],
                 param->ble_security.auth_cmpl.bd_addr[2],
                 param->ble_security.auth_cmpl.bd_addr[3],
                 param->ble_security.auth_cmpl.bd_addr[4],
                 param->ble_security.auth_cmpl.bd_addr[5]);
      } else {
        ESP_LOGW(TAG, "✗ BLE authentication failed!");
        ESP_LOGW(TAG, "  Failure reason: 0x%x", param->ble_security.auth_cmpl.fail_reason);
      }
      break;
      
    case ESP_GAP_BLE_SEC_REQ_EVT:
      ESP_LOGI(TAG, "BLE security request from device - accepting");
      // Initiate pairing response
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
      
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      ESP_LOGI(TAG, "BLE passkey notification: %d", param->ble_security.key_notif.passkey);
      break;
      
    case ESP_GAP_BLE_KEY_EVT:
      ESP_LOGI(TAG, "BLE key event (key exchange)");
      break;
      
    default:
      // Don't log all GAP events to reduce noise
      break;
  }
}

void AlphaHwrComponent::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                             esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      ESP_LOGI(TAG, "BLE connection opened");
      
      // Reset discovery and authentication state on new connection
      geni_service_found_ = false;
      discovery_retry_count_ = 0;
      auth_started_ = false;  // Reset auth flag for new connection
      
      // EXPERIMENT 4: Request BLE encryption/pairing BEFORE service discovery
      // This is the key difference - Python/Bleak likely does this transparently
      {
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "EXPERIMENT 4: Requesting BLE Encryption/Pairing");
        ESP_LOGI(TAG, "Pump may require encryption BEFORE service discovery");
        ESP_LOGI(TAG, "==================================================");
        
        // Set encryption requirement
        esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
        esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
        uint8_t key_size = 16;
        uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint32_t passkey = 0;
        uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
        uint8_t oob_support = ESP_BLE_OOB_DISABLE;
        
        // Request encryption on this connection
        esp_err_t ret = esp_ble_set_encryption(parent_->get_remote_bda(), ESP_BLE_SEC_ENCRYPT);
        if (ret == ESP_OK) {
          ESP_LOGI(TAG, "✓ Encryption request sent to pump");
        } else {
          ESP_LOGW(TAG, "✗ Failed to request encryption: 0x%x", ret);
        }
      }
      
      // EXPERIMENT 3: Set BLE connection parameters that the pump expects
      // The pump may be rejecting default ESP32 connection parameters
      {
        ESP_LOGI(TAG, "Setting BLE connection parameters...");
        
        esp_ble_conn_update_params_t conn_params;
        memcpy(conn_params.bda, parent_->get_remote_bda(), 6);
        
        // Try more relaxed connection parameters (similar to what BLE Central typically negotiates)
        // These values are in units of 1.25ms for intervals
        conn_params.min_int = 24;      // 24 * 1.25ms = 30ms (was 6 = 7.5ms)
        conn_params.max_int = 40;      // 40 * 1.25ms = 50ms (was 12 = 15ms) 
        conn_params.latency = 0;       // No latency
        conn_params.timeout = 400;     // 400 * 10ms = 4000ms = 4s supervision timeout
        
        ESP_LOGI(TAG, "  Interval: %.1f-%.1fms, Timeout: %dms", 
                 conn_params.min_int * 1.25, conn_params.max_int * 1.25, conn_params.timeout * 10);
        
        esp_err_t ret = esp_ble_gap_update_conn_params(&conn_params);
        if (ret == ESP_OK) {
          ESP_LOGI(TAG, "✓ Connection parameter update requested");
        } else {
          ESP_LOGW(TAG, "✗ Failed to request connection parameter update: 0x%x", ret);
        }
      }
      
      // CRITICAL FIX: Add delay after connection before starting service discovery
      // Wait for encryption to complete if requested
      ESP_LOGI(TAG, "Waiting %dms for encryption/params before service discovery...", POST_CONNECT_DELAY_MS);
      this->set_timeout(POST_CONNECT_DELAY_MS, [this]() {
        ESP_LOGI(TAG, "Starting initial service discovery...");
        // Service discovery is automatically triggered by BLE client
        // We just need to wait for ESP_GATTC_SEARCH_CMPL_EVT
      });
      break;
      
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
            ESP_LOGI(TAG, "✓ GENI characteristic found!");
            ESP_LOGI(TAG, "  UUID: %s", chr->uuid.to_string().c_str());
            ESP_LOGI(TAG, "  Handle: 0x%04x, Properties: 0x%02x", chr->handle, chr->properties);
            
            // EXPERIMENT 2: Enable notifications and WAIT before doing anything else
            ESP_LOGI(TAG, "==================================================");
            ESP_LOGI(TAG, "EXPERIMENT 2: Enable notifications and WAIT");
            ESP_LOGI(TAG, "Mimicking Python: enable notify, wait 2s, then auth");
            ESP_LOGI(TAG, "==================================================");
            
            // Step 1: Enable notifications (subscribe to characteristic)
            ESP_LOGI(TAG, "Step 1: Enabling notifications on GENI characteristic...");
            subscribe_to_notifications();
            
            // Step 2: Wait 2 seconds for pump to stabilize after notification enable
            // Python does this implicitly - we need to do it explicitly
            ESP_LOGI(TAG, "Step 2: Waiting 2000ms for pump to stabilize after notification enable...");
            this->set_timeout(2000, [this]() {
              ESP_LOGI(TAG, "Step 3: Pump should be stable now. Sending authentication...");
              authenticate();
            });
          } else {
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
        
        // EXPERIMENT: Disable authentication completely to see if pump sends telemetry anyway
        // Some devices send basic telemetry without auth, auth only needed for control operations
        if (!auth_started_) {
          auth_started_ = true;
          ESP_LOGI(TAG, "==================================================");
          ESP_LOGI(TAG, "EXPERIMENT: Authentication DISABLED");
          ESP_LOGI(TAG, "Waiting for telemetry notifications from pump...");
          ESP_LOGI(TAG, "If we receive telemetry, auth is only needed for control");
          ESP_LOGI(TAG, "==================================================");
          authenticated_ = false;  // Mark as not authenticated
        }
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
  if (opspec == 0x30 && len >= 37) {
    // Motor state response: floats start at offset 13
    // [0]=AC voltage, [1]=DC voltage, [2]=current, [3]=power, [5]=RPM
    ESP_LOGI(TAG, "  Motor state response (OpSpec 0x30)");
    
    float power = read_float_be(data, 25);  // Float[3] at offset 13 + 12 = 25
    float rpm = read_float_be(data, 33);    // Float[5] at offset 13 + 20 = 33
    
    if (power >= 0 && power <= 1000 && rpm >= 0 && rpm <= 10000) {
      ESP_LOGI(TAG, "✓ Motor: Power=%.1f W, RPM=%.0f", power, rpm);
      if (power_sensor_ != nullptr) power_sensor_->publish_state(power);
      if (rpm_sensor_ != nullptr) rpm_sensor_->publish_state(rpm);
    } else {
      ESP_LOGW(TAG, "  Invalid motor values (power=%.1f, rpm=%.0f)", power, rpm);
    }
  }
  else if (opspec == 0x2B && len >= 45) {
    // Flow/pressure response: floats start at offset 13
    // [6]=flow at offset 13+24=37, [7]=head at offset 13+28=41
    ESP_LOGI(TAG, "  Flow/pressure response (OpSpec 0x2B)");
    
    float flow = read_float_be(data, 37);
    float head = read_float_be(data, 41);
    
    if (flow >= 0 && flow <= 100 && head >= 0 && head <= 50) {
      ESP_LOGI(TAG, "✓ Flow/Head: %.3f m³/h, %.2f m", flow, head);
      if (flow_sensor_ != nullptr) flow_sensor_->publish_state(flow);
      if (head_sensor_ != nullptr) head_sensor_->publish_state(head);
    } else {
      ESP_LOGW(TAG, "  Invalid flow/head values (flow=%.3f, head=%.2f)", flow, head);
    }
  }
  else if (opspec == 0x14 && len >= 21) {
    // Temperature response: single float at offset 13
    ESP_LOGI(TAG, "  Temperature response (OpSpec 0x14)");
    
    float temp = read_float_be(data, 13);
    
    if (temp >= -50 && temp <= 150) {
      ESP_LOGI(TAG, "✓ Temp: %.1f°C", temp);
      if (temp_media_sensor_ != nullptr) temp_media_sensor_->publish_state(temp);
    } else {
      ESP_LOGW(TAG, "  Invalid temperature value (temp=%.1f)", temp);
    }
  }
  // OpSpec 0x0E = Passive Notifications (streaming telemetry - legacy format)
  else if (opspec == 0x0E && len >= 10) {
    uint16_t sub_id = (data[6] << 8) | data[7];
    uint16_t obj_id = (data[8] << 8) | data[9];
    
    ESP_LOGD(TAG, "  Passive telemetry notification: Sub=0x%04X, Obj=0x%04X", sub_id, obj_id);
    
    // Standard Motor State (Sub 0x0045, Obj 0x0057 = Decimal Sub 69, Obj 87)
    if (sub_id == 0x0045 && obj_id == 0x0057 && len >= 34) {
      float power = read_float_be(data, 26);
      float rpm = read_float_be(data, 30);
      ESP_LOGI(TAG, "✓ Motor (passive): Power=%.1f W, RPM=%.0f", power, rpm);
      if (power_sensor_ != nullptr) power_sensor_->publish_state(power);
      if (rpm_sensor_ != nullptr) rpm_sensor_->publish_state(rpm);
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
    else if (sub_id == 0x012C && obj_id == 0x005D && len >= 14) {
      float temp = read_float_be(data, 10);
      ESP_LOGI(TAG, "✓ Temp (passive): %.1f°C", temp);
      if (temp_media_sensor_ != nullptr) temp_media_sensor_->publish_state(temp);
    }
    else {
      ESP_LOGD(TAG, "  Unknown passive notification (Sub=0x%04X, Obj=0x%04X)", sub_id, obj_id);
    }
  }
  else {
    ESP_LOGD(TAG, "  Unhandled packet type (OpSpec=0x%02X, len=%d)", opspec, len);
  }
}

}  // namespace alpha_hwr
}  // namespace esphome
