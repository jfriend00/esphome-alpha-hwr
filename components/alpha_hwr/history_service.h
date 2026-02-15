/**
 * History Service for Grundfos ALPHA HWR Pumps
 *
 * Reads trend data (flow, head, temperature, power-on time).
 * Object 88, SubIDs 13300/13301 (timestamps)
 * Object 53, SubIDs 451-454 (TrendData1B, 29 bytes each)
 *
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/history.py
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
 * TrendData1B — 29-byte trend record (Type 946).
 *
 *   Bytes 0-3:    current_value (float32 BE)
 *   Bytes 4-13:   last_10_cycle (10× uint8)
 *   Byte  14:     next_counter (uint8)
 *   Bytes 15-18:  next_100_value (float32 BE)
 *   Bytes 19-28:  last_10_of_100_cycle (10× uint8)
 */
struct TrendData {
  float current_value{0};
  uint8_t last_10[10]{};
  uint8_t next_counter{0};
  float next_100_value{0};
  uint8_t last_100[10]{};

  static TrendData from_bytes(const uint8_t *data);
};

/**
 * Decoded trend series with scaling applied.
 */
struct TrendSeries {
  std::string name;
  std::string unit;
  float current_value{0};
  float scale{1.0f};
  std::vector<float> recent_10;    // Last 10 cycle values (scaled)
  std::vector<float> recent_100;   // Last 10 of 100-cycle values (scaled)
};

class HistoryService {
 public:
  HistoryService(core::Transport &transport, core::Session &session);
  ~HistoryService() = default;

  /**
   * Read all trend data from pump.
   * Reads SubIDs 451-454 from Object 53.
   */
  void read_trends_async(std::function<void(bool, const std::vector<TrendSeries> &)> on_complete);

  /**
   * Format trend data as display string.
   */
  std::string format_display() const;

  const std::vector<TrendSeries> &get_cached_trends() const { return cached_trends_; }
  bool is_cached() const { return trends_cached_; }

 private:
  core::Transport &transport_;
  core::Session &session_;

  std::vector<TrendSeries> cached_trends_;
  bool trends_cached_{false};

  void build_geni_frame(uint8_t dst, uint8_t src, const uint8_t *apdu, size_t apdu_len,
                        uint8_t *frame, size_t *frame_len);

  static constexpr const char *TAG = "alpha_hwr.history";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
