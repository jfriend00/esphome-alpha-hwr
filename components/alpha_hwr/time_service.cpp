/**
 * Time Service Implementation
 * 
 * Manages pump real-time clock (RTC) for schedule execution and event logging.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/time.py
 */

#include "time_service.h"
#include "frame_builder.h"
#include "codec.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace services {

using namespace esphome::alpha_hwr::protocol;
using namespace esphome::alpha_hwr::core;

static const char* TAG = "alpha_hwr.time";

// Object 94 (0x5E) - RTC Management
static const uint16_t OBJECT_ID_RTC = 94;
static const uint16_t SUB_ID_DATETIME_ACTUAL = 101;  // Read current time
static const uint16_t SUB_ID_DATETIME_CONFIG = 100;  // Set time

void TimeService::get_clock_async(std::function<void(ESPTime)> callback) {
  ESP_LOGD(TAG, "Reading pump clock (Object 94, Sub 101)...");
  
  // Send Class 10 GET: Object 94, SubID 101 (DateTimeActual)
  // send_command signature: (packet, expect_obj_id, expect_sub_id, callback, timeout_ms)
  transport_->send_command(
    {},  // Empty packet for GET
    OBJECT_ID_RTC, 
    SUB_ID_DATETIME_ACTUAL,
    [this, callback](bool success, const uint8_t *data, size_t len) {
      if (!success || !data || len == 0) {
        ESP_LOGW(TAG, "Clock read timeout or failed");
        callback(ESPTime());
        return;
      }
      
      ESPTime pump_time = parse_clock_response(data, len);
      
      if (pump_time.is_valid()) {
        ESP_LOGI(TAG, "Pump clock: %04d-%02d-%02d %02d:%02d:%02d", 
                 pump_time.year, pump_time.month, pump_time.day_of_month,
                 pump_time.hour, pump_time.minute, pump_time.second);
      } else {
        ESP_LOGW(TAG, "Pump clock is unset or invalid");
      }
      
      callback(pump_time);
    },
    5000  // 5 second timeout for clock read
  );
}

void TimeService::set_clock_async(std::function<void(bool)> callback) {
  // Get current system time from SNTP/NTP
  time_t current_time = ::time(nullptr);
  if (current_time < 1609459200) {  // Before 2021-01-01 means time not synced
    ESP_LOGE(TAG, "System time not available - cannot sync clock");
    callback(false);
    return;
  }
  
  // Convert to local time struct
  struct tm *timeinfo = localtime(&current_time);
  
  // Create ESPTime from tm struct
  ESPTime now;
  now.year = timeinfo->tm_year + 1900;
  now.month = timeinfo->tm_mon + 1;
  now.day_of_month = timeinfo->tm_mday;
  now.hour = timeinfo->tm_hour;
  now.minute = timeinfo->tm_min;
  now.second = timeinfo->tm_sec;
  now.timestamp = current_time;
  
  ESP_LOGI(TAG, "Syncing pump clock to system time: %04d-%02d-%02d %02d:%02d:%02d",
           now.year, now.month, now.day_of_month, now.hour, now.minute, now.second);
  
  // Build the set clock packet
  std::vector<uint8_t> packet = build_set_clock_packet(now);
  
  ESP_LOGD(TAG, "Clock SET packet: %s (%zu bytes)", 
           format_hex_pretty(packet).c_str(), packet.size());
  
  // Send the write command
  // send_command signature: (packet, expect_obj_id, expect_sub_id, callback, timeout_ms)
  transport_->send_command(
    packet,
    OBJECT_ID_RTC,
    SUB_ID_DATETIME_CONFIG,
    [this, callback, now](bool success, const uint8_t *data, size_t len) {
      if (!success) {
        ESP_LOGW(TAG, "Clock set command timeout");
        callback(false);
        return;
      }
      
      ESP_LOGD(TAG, "Clock set command acknowledged, verifying...");
      
      // Read back to verify
      get_clock_async([callback, now](ESPTime pump_time) {
        if (!pump_time.is_valid()) {
          ESP_LOGW(TAG, "Clock verification failed - pump time invalid");
          callback(false);
          return;
        }
        
        // Check if time is within 5 seconds of what we set
        int32_t time_diff = pump_time.timestamp - now.timestamp;
        if (abs(time_diff) < 5) {
          ESP_LOGI(TAG, "Clock synchronized successfully (diff: %d seconds)", time_diff);
          callback(true);
        } else {
          ESP_LOGW(TAG, "Clock sync may have failed - time diff: %d seconds", time_diff);
          callback(false);
        }
      });
    },
    5000  // 5 second timeout for clock write
  );
}

std::vector<uint8_t> TimeService::build_set_clock_packet(const ESPTime &dt) {
  // Build datetime payload: [Year(2BE)][Month][Day][Hour][Minute][Second] + 13 padding bytes = 19 total
  std::vector<uint8_t> datetime_bytes(19, 0);
  
  // Year as big-endian uint16
  datetime_bytes[0] = (dt.year >> 8) & 0xFF;
  datetime_bytes[1] = dt.year & 0xFF;
  
  // Month, Day, Hour, Minute, Second
  datetime_bytes[2] = dt.month;
  datetime_bytes[3] = dt.day_of_month;
  datetime_bytes[4] = dt.hour;
  datetime_bytes[5] = dt.minute;
  datetime_bytes[6] = dt.second;
  
  // Bytes 7-18 are padding (already initialized to 0)
  
  ESP_LOGD(TAG, "DateTime bytes: %04d-%02d-%02d %02d:%02d:%02d",
           dt.year, dt.month, dt.day_of_month, dt.hour, dt.minute, dt.second);
  
  // Build APDU: [UnknownByte][Object][SubID][OpSpec][Data...]
  // From iOS captures:
  // - UnknownByte: 0x07 (purpose unknown, possibly address/routing)
  // - Object: 0x5E (94)
  // - SubID: 0x64 (100 = DateTimeConfig)
  // - OpSpec: 0x70 (SET operation)
  std::vector<uint8_t> apdu;
  apdu.push_back(0x07);  // Unknown byte
  apdu.push_back(0x5E);  // Object 94
  apdu.push_back(0x64);  // SubID 100
  apdu.push_back(0x70);  // OpSpec: SET
  apdu.insert(apdu.end(), datetime_bytes.begin(), datetime_bytes.end());
  
  // Build GENI frame: [Delimiter][Length][APDU][CRC]
  uint8_t length = apdu.size();
  std::vector<uint8_t> frame;
  frame.push_back(0x27);  // Delimiter
  frame.push_back(length);
  frame.insert(frame.end(), apdu.begin(), apdu.end());
  
  // Calculate and append CRC
  uint16_t crc = calc_crc16(frame.data() + 1, frame.size() - 1);
  frame.push_back((crc >> 8) & 0xFF);
  frame.push_back(crc & 0xFF);
  
  return frame;
}

ESPTime TimeService::parse_clock_response(const uint8_t *data, size_t len) {
  // Response format: [Status(2)][Length(1)][Year(2BE)][Month][Day][Hour][Minute][Second]
  // Minimum length: 10 bytes
  
  if (len < 10) {
    ESP_LOGW(TAG, "Clock response too short: %zu bytes (expected >= 10)", len);
    return ESPTime();
  }
  
  ESP_LOGD(TAG, "Raw clock data: %s (%zu bytes)", format_hex_pretty(data, len).c_str(), len);
  
  // Parse Status (2 bytes)
  uint16_t status = (data[0] << 8) | data[1];
  
  // Skip Status (2 bytes) and Length (1 byte), payload starts at byte 3
  const uint8_t *payload = data + 3;
  size_t payload_len = len - 3;
  
  if (payload_len < 7) {
    ESP_LOGW(TAG, "Clock payload too short: %zu bytes (expected >= 7)", payload_len);
    return ESPTime();
  }
  
  // Year is big-endian uint16
  uint16_t year = (payload[0] << 8) | payload[1];
  uint8_t month = payload[2];
  uint8_t day = payload[3];
  uint8_t hour = payload[4];
  uint8_t minute = payload[5];
  uint8_t second = payload[6];
  
  ESP_LOGD(TAG, "Parsed clock: %04d-%02d-%02d %02d:%02d:%02d, status=0x%04X",
           year, month, day, hour, minute, second, status);
  
  // Handle unset/invalid clock
  if (year < 1970 || year > 2100 || month == 0 || month > 12 || day == 0 || day > 31) {
    ESP_LOGW(TAG, "Pump clock is unset or invalid: %04d-%02d-%02d", year, month, day);
    return ESPTime();  // Return invalid time
  }
  
  // Create ESPTime
  ESPTime pump_time;
  pump_time.year = year;
  pump_time.month = month;
  pump_time.day_of_month = day;
  pump_time.hour = hour;
  pump_time.minute = minute;
  pump_time.second = second;
  pump_time.recalc_timestamp_utc(false);  // Calculate Unix timestamp
  
  return pump_time;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
