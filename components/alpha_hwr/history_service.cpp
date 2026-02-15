/**
 * History Service Implementation
 *
 * Reads trend data (TrendData1B, Type 946) from Object 53 SubIDs 451-454.
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/history.py
 */

#include "history_service.h"
#include "frame_builder.h"
#include "codec.h"
#include "transport.h"
#include "session.h"

#include <cstring>
#include <cmath>
#include <memory>

namespace esphome {
namespace alpha_hwr {
namespace services {

static constexpr uint8_t OBJECT_TRENDS = 53;

// SubID → { name, unit, scale }
struct TrendConfig {
  uint16_t sub_id;
  const char *name;
  const char *unit;
  float scale;
};

static const TrendConfig TREND_CONFIGS[] = {
    {451, "Flow",         "m³/h", 0.1f},
    {452, "Head",         "m",    0.1f},
    {453, "Temperature",  "°C",   1.0f},
    {454, "Power-on Time","h",    1.0f},
};
static constexpr size_t NUM_TRENDS = sizeof(TREND_CONFIGS) / sizeof(TREND_CONFIGS[0]);

TrendData TrendData::from_bytes(const uint8_t *data) {
  TrendData t;
  t.current_value = protocol::decode_float_be(data);
  memcpy(t.last_10, data + 4, 10);
  t.next_counter = data[14];
  t.next_100_value = protocol::decode_float_be(data + 15);
  memcpy(t.last_100, data + 19, 10);
  return t;
}

HistoryService::HistoryService(core::Transport &transport, core::Session &session)
    : transport_(transport), session_(session) {}

void HistoryService::build_geni_frame(uint8_t dst, uint8_t src, const uint8_t *apdu,
                                       size_t apdu_len, uint8_t *frame, size_t *frame_len) {
  *frame_len = protocol::build_geni_packet(dst, src, apdu, apdu_len, frame);
}

void HistoryService::read_trends_async(
    std::function<void(bool, const std::vector<TrendSeries> &)> on_complete) {
  if (!session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read history: session not ready");
    std::vector<TrendSeries> empty;
    if (on_complete) on_complete(false, empty);
    return;
  }

  ESP_LOGI(TAG, "Reading %zu trend channels...", NUM_TRENDS);

  auto trends = std::make_shared<std::vector<TrendSeries>>();
  auto read_next = std::make_shared<std::function<void(size_t)>>();

  *read_next = [this, trends, on_complete, read_next](size_t idx) {
    if (idx >= NUM_TRENDS) {
      cached_trends_ = *trends;
      trends_cached_ = true;
      ESP_LOGI(TAG, "Read %zu trend channels", trends->size());
      if (on_complete) on_complete(true, cached_trends_);
      return;
    }

    const auto &cfg = TREND_CONFIGS[idx];
    uint16_t sub_id = cfg.sub_id;

    uint8_t apdu[5];
    apdu[0] = 0x0A;
    apdu[1] = 0x03;
    apdu[2] = OBJECT_TRENDS;
    apdu[3] = (sub_id >> 8) & 0xFF;
    apdu[4] = sub_id & 0xFF;

    uint8_t frame[64];
    size_t frame_len;
    build_geni_frame(0xE7, 0xF8, apdu, 5, frame, &frame_len);
    std::vector<uint8_t> packet(frame, frame + frame_len);

    // Use wildcard response matching — accept any non-register Class 10 response
    transport_.send_command(packet, 0, 0,
        [this, idx, trends, on_complete, read_next, cfg](
            bool success, const uint8_t *payload, size_t payload_len) {
      if (success && payload_len >= 32) {  // 3 header + 29 data
        const uint8_t *data = payload + 3;
        TrendData td = TrendData::from_bytes(data);

        TrendSeries series;
        series.name = cfg.name;
        series.unit = cfg.unit;
        series.scale = cfg.scale;
        series.current_value = td.current_value;

        // Scale the uint8 values
        for (int i = 0; i < 10; i++) {
          series.recent_10.push_back(td.last_10[i] * cfg.scale);
          series.recent_100.push_back(td.last_100[i] * cfg.scale);
        }

        trends->push_back(series);
        ESP_LOGD(TAG, "Trend %s: current=%.2f %s", cfg.name, td.current_value, cfg.unit);
      } else {
        ESP_LOGW(TAG, "Trend %s: read failed (success=%d, len=%zu)", cfg.name, success, payload_len);
      }
      (*read_next)(idx + 1);
    }, 3000);
  };

  (*read_next)(0);
}

std::string HistoryService::format_display() const {
  if (!trends_cached_ || cached_trends_.empty()) {
    return "No history data";
  }

  std::string result;
  for (const auto &s : cached_trends_) {
    char buf[120];
    if (std::isnan(s.current_value)) {
      snprintf(buf, sizeof(buf), "%s: -- %s", s.name.c_str(), s.unit.c_str());
    } else {
      snprintf(buf, sizeof(buf), "%s: %.1f %s",
               s.name.c_str(), s.current_value, s.unit.c_str());
    }

    if (!result.empty()) result += "\n";
    result += buf;

    // Add recent 10-cycle values summary
    if (!s.recent_10.empty()) {
      result += " [";
      for (size_t i = 0; i < s.recent_10.size(); i++) {
        if (i > 0) result += ",";
        char v[16];
        snprintf(v, sizeof(v), "%.1f", s.recent_10[i]);
        result += v;
      }
      result += "]";
    }
  }
  return result;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
