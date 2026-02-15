/**
 * Event Log Service for Grundfos ALPHA HWR Pumps
 *
 * Reads the pump's event log (start/stop events with timestamps).
 * Object 88, SubID 10199 (metadata), SubIDs 10200-10219 (entries).
 *
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/event_log.py
 */

#pragma once

#include "esphome/core/log.h"
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace core {
class Transport;
class Session;
}

namespace services {

/**
 * Event log entry parsed from pump response.
 *
 * Entry format (16 bytes):
 *   Bytes 0-1:   header (uint16 BE)
 *   Bytes 2-3:   field (uint16 BE)
 *   Byte  4:     cycle_counter (uint8)
 *   Byte  5:     unknown (uint8)
 *   Byte  6:     mode_byte (uint8)
 *   Bytes 7-8:   constants (uint16 BE)
 *   Byte  9:     event_type (0x01=start, 0x02=stop)
 *   Bytes 10-13: timestamp (uint32 BE Unix)
 *   Bytes 14-15: trailing (uint16 BE)
 */
struct EventLogEntry {
  uint8_t cycle_counter{0};
  uint8_t mode_byte{0};
  uint8_t event_type{0};   // 0x01=start, 0x02=stop
  uint32_t timestamp{0};

  static EventLogEntry from_bytes(const uint8_t *data) {
    EventLogEntry e;
    e.cycle_counter = data[4];
    e.mode_byte = data[6];
    e.event_type = data[9];
    e.timestamp = ((uint32_t)data[10] << 24) | ((uint32_t)data[11] << 16) |
                  ((uint32_t)data[12] << 8) | data[13];
    return e;
  }

  const char *event_type_str() const {
    switch (event_type) {
      case 0x01: return "Start";
      case 0x02: return "Stop";
      default: return "Unknown";
    }
  }
};

struct EventLogMetadata {
  uint16_t cycle_counter{0};
  uint16_t available_entries{0};
  uint16_t max_entries{20};
};

class EventLogService {
 public:
  EventLogService(core::Transport &transport, core::Session &session);
  ~EventLogService() = default;

  /**
   * Read event log metadata (Object 88, SubID 10199).
   * Returns cycle counter and number of available entries.
   */
  void read_metadata_async(std::function<void(bool, const EventLogMetadata &)> on_complete);

  /**
   * Read all event log entries from pump.
   * Reads metadata first, then entries sequentially.
   */
  void read_entries_async(std::function<void(bool, const std::vector<EventLogEntry> &)> on_complete);

  /**
   * Format event log as display string.
   */
  std::string format_display() const;

  const std::vector<EventLogEntry> &get_cached_entries() const { return cached_entries_; }
  bool is_cached() const { return entries_cached_; }

 private:
  core::Transport &transport_;
  core::Session &session_;

  std::vector<EventLogEntry> cached_entries_;
  EventLogMetadata metadata_;
  bool entries_cached_{false};

  void build_geni_frame(uint8_t dst, uint8_t src, const uint8_t *apdu, size_t apdu_len,
                        uint8_t *frame, size_t *frame_len);

  static constexpr const char *TAG = "alpha_hwr.event_log";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
