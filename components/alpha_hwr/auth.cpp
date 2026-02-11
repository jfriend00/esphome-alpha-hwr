#include "auth.h"

namespace esphome {
namespace alpha_hwr {
namespace core {

static const char *TAG = "alpha_hwr.auth";

Authentication::Authentication() {}

void Authentication::set_write_callback(WriteCallback callback) {
  write_callback_ = std::move(callback);
}

void Authentication::set_scheduler_callback(SchedulerCallback callback) {
  scheduler_callback_ = std::move(callback);
}

void Authentication::set_completion_callback(CompletionCallback callback) {
  completion_callback_ = std::move(callback);
}

void Authentication::start() {
  if (running_) {
    ESP_LOGW(TAG, "Authentication already in progress");
    return;
  }
  
  running_ = true;
  ESP_LOGI(TAG, "Starting 3-stage authentication handshake");
  
  // Start Stage 1 immediately
  stage1_legacy_burst(0);
}

void Authentication::cancel() {
  if (running_) {
    ESP_LOGW(TAG, "Authentication cancelled");
    running_ = false;
  }
}

bool Authentication::send_packet(const uint8_t* data, size_t len) {
  if (!write_callback_) {
    ESP_LOGE(TAG, "No write callback configured");
    return false;
  }
  
  if (!running_) {
    ESP_LOGD(TAG, "Authentication cancelled, skipping packet send");
    return false;
  }
  
  bool success = write_callback_(data, len);
  if (success) {
    ESP_LOGD(TAG, "Sent auth packet (%zu bytes)", len);
  } else {
    ESP_LOGW(TAG, "Failed to send auth packet");
  }
  
  return success;
}

void Authentication::stage1_legacy_burst(int repeat_count) {
  if (!running_) return;
  
  if (repeat_count < 3) {
    // Send legacy magic packet
    ESP_LOGD(TAG, "Stage 1: Sending legacy magic packet %d/3", repeat_count + 1);
    send_packet(AUTH_LEGACY, sizeof(AUTH_LEGACY));
    
    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    if (scheduler_callback_) {
      scheduler_callback_(50, [this, repeat_count]() {
        this->stage1_legacy_burst(repeat_count + 1);
      });
    }
  } else {
    // Stage 1 complete, wait 100ms then start Stage 2 (Python uses 0.1s)
    ESP_LOGD(TAG, "Stage 1 complete, waiting 100ms before Stage 2");
    if (scheduler_callback_) {
      scheduler_callback_(100, [this]() {
        this->stage2_class10_burst(0);
      });
    }
  }
}

void Authentication::stage2_class10_burst(int repeat_count) {
  if (!running_) return;
  
  if (repeat_count < 5) {
    // Send Class 10 unlock packet
    ESP_LOGD(TAG, "Stage 2: Sending Class 10 unlock packet %d/5", repeat_count + 1);
    send_packet(AUTH_CLASS10, sizeof(AUTH_CLASS10));
    
    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    if (scheduler_callback_) {
      scheduler_callback_(50, [this, repeat_count]() {
        this->stage2_class10_burst(repeat_count + 1);
      });
    }
  } else {
    // Stage 2 complete, wait 200ms then start Stage 3 (Python uses 0.2s)
    ESP_LOGD(TAG, "Stage 2 complete, waiting 200ms before Stage 3");
    if (scheduler_callback_) {
      scheduler_callback_(200, [this]() {
        this->stage3_extensions();
      });
    }
  }
}

void Authentication::stage3_extensions() {
  if (!running_) return;
  
  ESP_LOGD(TAG, "Stage 3: Sending extension packets");
  
  // Python sends EXTEND_2 then EXTEND_1 (note the order)
  // See authentication.py lines 344-351
  send_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2));
  send_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1));
  
  // Wait 500ms for final stabilization (Python uses 0.5s)
  if (scheduler_callback_) {
    scheduler_callback_(500, [this]() {
      this->complete();
    });
  }
}

void Authentication::complete() {
  if (!running_) return;
  
  ESP_LOGI(TAG, "Authentication handshake complete");
  running_ = false;
  
  // Notify completion
  if (completion_callback_) {
    completion_callback_();
  }
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
