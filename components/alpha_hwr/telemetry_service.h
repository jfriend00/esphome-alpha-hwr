/**
 * Telemetry Service for Grundfos ALPHA HWR Pumps
 * 
 * This service handles all telemetry operations including:
 * - Reading current telemetry snapshot
 * - Managing periodic telemetry polling
 * - Parsing Class 10 DataObject notifications
 * - Updating sensor values
 * 
 * The service coordinates between the transport layer (BLE communication)
 * and the protocol layer (frame parsing/telemetry decoding) to provide
 * telemetry management.
 * 
 * Architecture:
 * - Transport Layer: Handles BLE communication and raw packets
 * - Protocol Layer: Parses frames and decodes telemetry
 * - Service Layer (this): Coordinates operations and manages state
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 */

#pragma once

#include "esphome/core/log.h"
#include <functional>
#include <cstdint>
#include <cstddef>

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * Callback for sending BLE packets.
 * 
 * @param data Packet data to send
 * @param len Length of packet data
 * @return true if write was successful, false otherwise
 */
using WriteCallback = std::function<bool(const uint8_t* data, size_t len)>;

/**
 * Callback for scheduling delayed tasks.
 * 
 * @param delay_ms Delay in milliseconds before executing callback
 * @param callback Function to execute after delay
 */
using SchedulerCallback = std::function<void(uint32_t delay_ms, std::function<void()> callback)>;

// Forward declaration for SensorPublisher
class SensorPublisher;

/**
 * Telemetry Service
 * 
 * Manages all telemetry-related operations for the ALPHA HWR pump.
 * 
 * Usage:
 * 1. Create instance
 * 2. Set callbacks (write, scheduler, sensor_update)
 * 3. Call start() when pump is ready
 * 4. Call poll() periodically to request telemetry
 * 5. Route incoming packets to on_packet()
 * 6. Call stop() when disconnecting
 * 
 * Example:
 * ```cpp
 * services::TelemetryService telemetry_service_;
 * 
 * // In setup():
 * telemetry_service_.set_write_callback([this](const uint8_t* data, size_t len) -> bool {
 *     return this->write_ble_packet(data, len);
 * });
 * 
 * telemetry_service_.set_scheduler_callback([this](uint32_t delay_ms, auto cb) {
 *     this->set_timeout(delay_ms, std::move(cb));
 * });
 * 
 * telemetry_service_.set_sensor_update_callback([this](const uint8_t* data, size_t len) {
 *     this->update_sensors(data, len);
 * });
 * 
 * // When ready:
 * telemetry_service_.start();
 * 
 * // In update() (every 10 seconds):
 * telemetry_service_.poll();
 * 
 * // In notification handler:
 * telemetry_service_.on_packet(data, len);
 * ```
 */
class TelemetryService {
 public:
  /**
   * Constructor
   */
  TelemetryService();

  /**
   * Destructor
   */
  ~TelemetryService() = default;

  /**
   * Set callback for writing BLE packets.
   * 
   * @param callback Function to call when sending packets
   */
  void set_write_callback(WriteCallback callback);

  /**
   * Set callback for scheduling delayed tasks.
   * 
   * @param callback Function to call when scheduling tasks
   */
  void set_scheduler_callback(SchedulerCallback callback);

  /**
   * Set sensor publisher for updating ESPHome sensors.
   * 
   * @param publisher Pointer to SensorPublisher instance
   */
  void set_sensor_publisher(SensorPublisher* publisher);

  /**
   * Start telemetry service.
   * 
   * Call this when the pump is authenticated and ready for telemetry.
   */
  void start();

  /**
   * Stop telemetry service.
   * 
   * Call this when disconnecting or shutting down.
   */
  void stop();

  /**
   * Check if service is running.
   * 
   * @return true if service is active
   */
  bool is_running() const;

  /**
   * Poll telemetry data.
   * 
   * Sends read requests for all telemetry registers:
   * - Motor state (0x570045)
   * - Flow/pressure (0x5D0122)
   * - Temperature (0x5D012C)
   * - Alarms (0x580000)
   * - Warnings (0x58000B)
   * 
   * This should be called periodically (e.g., every 10 seconds) by the
   * main component's update() method.
   */
  void poll();

  /**
   * Process incoming packet.
   * 
   * Routes the packet to the appropriate handler based on OpSpec:
   * - 0x30: Motor state response
   * - 0x2B: Flow/pressure response
   * - 0x14: Temperature response
   * - 0x13: Alarms/warnings response
   * - 0x0E: Passive notification
   * 
   * @param data Packet data
   * @param len Length of packet data
   */
  void on_packet(const uint8_t* data, size_t len);

 private:
  // Callbacks
  WriteCallback write_callback_;
  SchedulerCallback scheduler_callback_;
  
  // Sensor publisher (for publishing telemetry to ESPHome sensors)
  SensorPublisher* sensor_publisher_{nullptr};

  // State
  bool running_ = false;

  // Handler methods for different telemetry types
  void handle_motor_state_response(const uint8_t* data, size_t len);
  void handle_flow_pressure_response(const uint8_t* data, size_t len);
  void handle_temperature_response(const uint8_t* data, size_t len);
  void handle_alarms_response(const uint8_t* data, size_t len);
  void handle_warnings_response(const uint8_t* data, size_t len);
  void handle_passive_notification(const uint8_t* data, size_t len);

  // Helper to send read requests
  void send_read_request(uint32_t register_addr);

  // Logging tag
  static constexpr const char* TAG = "alpha_hwr.telemetry";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
