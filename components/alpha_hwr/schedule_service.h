/**
 * Schedule Service for Grundfos ALPHA HWR Pump
 *
 * Manages pump schedule operations including reading, writing, enabling/disabling,
 * and validation. The pump supports up to 5 schedule layers (0-4), with each layer
 * containing one time interval per day of the week.
 *
 * Based on: reference/alpha-hwr/src/alpha_hwr/services/schedule.py
 *
 * Protocol Details:
 * -----------------
 * - Schedule state: Class 10, Object 84, SubID 1 (ClockProgramOverview)
 *   - Byte 7 = enabled flag (0x01=enabled, 0x00=disabled)
 *
 * - Schedule entries: Class 10, Object 84
 *   - SubID: 1000 + layer (1000-1004 for layers 0-4)
 *   - Format: 3-byte header + (7 days × 6 bytes) = 45 bytes total
 *   - Each entry: [Enabled][Action][StartH][StartM][EndH][EndM]
 *
 * - Enable/Disable: Class 10, OpSpec 0x93 (OpSpec 4, Length 19)
 *   - Writes to Object 84, SubID 1
 *   - Modifies complete 10-byte ClockProgramOverview structure
 *
 * - Write entries: Class 10, OpSpec 0xB3 (OpSpec 5, Length varies)
 *   - Writes to Object 84, SubID 1000-1004
 *   - Payload: 42 bytes (7 days × 6 bytes)
 */

#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "schedule_entry.h"
#include <functional>
#include <vector>

namespace esphome {
namespace alpha_hwr {

// Forward declarations
namespace core {
class Transport;
class Session;
}  // namespace core

namespace services {

/**
 * Schedule Service - Manages pump weekly schedules.
 *
 * Provides methods to:
 * - Read schedule entries from pump (by layer or all layers)
 * - Write schedule entries to pump (with validation)
 * - Enable/disable the internal schedule
 * - Validate entries for overlaps and time range errors
 * - Clear individual day entries
 *
 * Example Usage (from lambda in YAML):
 *
 * ```cpp
 * // Enable schedule
 * id(pump).enable_schedule();
 *
 * // Read current schedule
 * std::vector<ScheduleEntry> entries = id(pump).read_schedule_entries();
 * for (const auto &entry : entries) {
 *   ESP_LOGI("schedule", "%s", entry.to_string().c_str());
 * }
 *
 * // Write new schedule
 * std::vector<ScheduleEntry> new_schedule;
 * new_schedule.push_back(ScheduleEntry("Monday", 6, 0, 8, 0));
 * new_schedule.push_back(ScheduleEntry("Tuesday", 6, 0, 8, 0));
 * id(pump).write_schedule_entries(new_schedule, 0);  // Write to layer 0
 * ```
 */
class ScheduleService {
 public:
  /**
   * Callback for scheduling delayed operations.
   * Parameters: (callback_function, delay_ms)
   */
  using ScheduleCallback = std::function<void(std::function<void()>, uint32_t)>;

  /**
   * Callback for writing BLE packets.
   * Parameters: (handle, data, length) -> bool (success)
   */
  using WriteCallback = std::function<bool(uint16_t, const uint8_t *, uint16_t)>;

  // -------------------------------------------------------------------------
  // Constructor
  // -------------------------------------------------------------------------

  ScheduleService(core::Transport &transport, core::Session &session);

  // -------------------------------------------------------------------------
  // Callback Setters (called by main component during setup)
  // -------------------------------------------------------------------------

  /**
   * Set callback for scheduling delayed operations.
   * Required for async write operations and verification delays.
   */
  void set_schedule_callback(ScheduleCallback callback) { this->schedule_callback_ = callback; }

  /**
   * Set callback for writing to BLE characteristic.
   * Required for sending commands to pump.
   */
  void set_write_callback(WriteCallback callback) { this->write_callback_ = callback; }

  // -------------------------------------------------------------------------
  // Schedule State Operations
  // -------------------------------------------------------------------------

  /**
   * Get the current schedule state (enabled/disabled).
   *
   * Returns the cached schedule state. The state is automatically updated
   * by calling poll_state() periodically from the main component update() loop.
   *
   * @param result Output parameter - set to true if enabled, false if disabled
   * @return True if cached state is valid, false if no state has been read yet
   *
   * Protocol: Class 10, Object 84, SubID 1
   * - Response format: [Header(3)][Capabilities(4)][Enabled(1)][DefaultAction(1)][BaseSetpoint(4)]
   * - Byte 4 (payload index) is the enabled flag (0x01=enabled, 0x00=disabled)
   *
   * Note: This method returns the cached state immediately. Call poll_state()
   * to trigger an async read operation to update the cache.
   */
  bool get_state(bool *result);

  /**
   * Poll schedule state from pump (async operation).
   *
   * Triggers an async read operation to fetch the current schedule state
   * from the pump. The result is cached and can be retrieved via get_state().
   *
   * This method should be called periodically (e.g., every 30s) from the
   * main component update() loop to keep the cached state fresh.
   *
   * @return True if read request was sent successfully, false otherwise
   */
  bool poll_state();

  /**
   * Enable the internal schedule.
   *
   * Activates the pump's built-in schedule functionality. When enabled,
   * the pump will automatically start/stop according to the programmed
   * schedule entries.
   *
   * @return True if successfully enabled, false otherwise
   *
   * Protocol: Class 10, OpSpec 0x93, Object 84, SubID 1
   * - Reads current ClockProgramOverview structure
   * - Modifies byte 4 (clock_program_enabled) to 0x01
   * - Writes back complete 10-byte structure
   */
  bool enable();

  /**
   * Disable the internal schedule.
   *
   * Deactivates the pump's built-in schedule functionality. The pump
   * will continue operating according to its current mode, but will
   * not automatically start/stop based on the schedule.
   *
   * @return True if successfully disabled, false otherwise
   *
   * Protocol: Same as enable() but sets byte 4 to 0x00
   */
  bool disable();

  // -------------------------------------------------------------------------
  // Schedule Entry Operations
  // -------------------------------------------------------------------------

  /**
   * Read schedule entries from the pump.
   *
   * Retrieves the current weekly schedule from one or all layers.
   * Each layer can contain up to 7 entries (one per day of the week).
   *
   * @param entries Output vector - populated with schedule entries
   * @param layer Optional specific layer (0-4) to read. If -1, reads all layers.
   * @return True if read succeeded, false on failure
   *
   * Protocol: Class 10, Object 84
   * - SubID: 1000 + layer (1000-1004 for layers 0-4)
   * - Response format: [Header 3 bytes] + [7 days × 6 bytes] = 45 bytes
   * - Each 6-byte entry: [Enabled][Action][StartH][StartM][EndH][EndM]
   * - Days in order: Mon, Tue, Wed, Thu, Fri, Sat, Sun
   * - Only enabled entries are included in output
   */
  bool read_entries(std::vector<ScheduleEntry> *entries, int layer = -1);

  /**
   * Write schedule entries to the pump.
   *
   * Writes a complete weekly schedule to the specified layer. Each layer
   * can contain up to 7 entries (one per day). Entries are validated before
   * writing to ensure no overlaps or invalid time ranges.
   *
   * @param entries Vector of ScheduleEntry objects to write
   * @param layer Schedule layer to write to (0-4)
   * @return True if successfully written, false otherwise
   *
   * Protocol: Class 10, OpSpec 0xB3 (OpSpec 5), Object 84
   * - SubID: 1000 + layer
   * - Payload: 42 bytes (7 days × 6 bytes, no header for writes)
   * - APDU format:
   *   [0x0A]              # Class 10
   *   [0xB3]              # OpSpec 5
   *   [84]                # Object ID
   *   [SubH][SubL]        # SubID (big-endian)
   *   [0x00]              # Reserved
   *   [0xDE][0x01][0x00]  # Type 222 header
   *   [0x00][0x2A]        # Size (42 bytes)
   *   [42 bytes data]     # Schedule entries
   *
   * Validation:
   * - Layer must be 0-4
   * - Time ranges must be valid (non-zero duration)
   * - No overlaps allowed within same day/layer
   * - Only one entry per day per layer (last one wins)
   */
  bool write_entries(const std::vector<ScheduleEntry> &entries, uint8_t layer = 0);

  /**
   * Clear (disable) a schedule entry for a specific day.
   *
   * This disables the schedule for a specific day on the specified layer,
   * but does not affect other days or layers.
   *
   * Implementation: Reads current schedule for layer, removes entry for
   * specified day, and writes back the filtered schedule.
   *
   * @param day Day name (Monday-Sunday)
   * @param layer Schedule layer (0-4)
   * @return True if successfully cleared, false otherwise
   */
  bool clear_entry(const std::string &day, uint8_t layer = 0);

  // -------------------------------------------------------------------------
  // Validation Methods
  // -------------------------------------------------------------------------

  /**
   * Validate a list of schedule entries for conflicts and errors.
   *
   * Performs comprehensive validation including:
   * - Time range validity (not zero duration)
   * - No overlaps within same day/layer
   * - Valid day names
   * - Valid layer values (0-4)
   *
   * @param entries Vector of ScheduleEntry instances to validate
   * @param errors Output vector - populated with error messages if validation fails
   * @return True if all entries are valid, false if any errors found
   *
   * Example error messages:
   * - "Entry 2 (Monday 06:00-06:00): Invalid time range: begin and end times are identical (06:00)"
   * - "Overlap detected: Monday layer 0: 06:00-08:00 overlaps with 07:00-09:00"
   */
  bool validate_entries(const std::vector<ScheduleEntry> &entries, std::vector<std::string> *errors);

 protected:
  // -------------------------------------------------------------------------
  // Internal Helper Methods
  // -------------------------------------------------------------------------

  // NOTE: read_class10_object() removed - requires async implementation
  // For Phase 7, only write operations are supported

  /**
   * Write a Class 10 command to the pump.
   *
   * @param apdu Complete APDU bytes (Class + OpSpec + data)
   * @param apdu_len Length of APDU
   * @return True if write succeeded, false otherwise
   */
  bool write_class10_command(const uint8_t *apdu, size_t apdu_len);

  /**
   * Internal helper to enable/disable schedule.
   *
   * @param enable True to enable, false to disable
   * @return True if operation succeeded, false otherwise
   */
  bool set_state(bool enable);

  /**
   * Build a GENI frame with CRC.
   *
   * @param dst Destination address (typically 0xE7)
   * @param src Source address (typically 0xF8)
   * @param apdu APDU bytes
   * @param apdu_len Length of APDU
   * @param frame Output buffer for complete frame
   * @param frame_len Output parameter for frame length
   */
  void build_geni_frame(uint8_t dst, uint8_t src, const uint8_t *apdu, size_t apdu_len, uint8_t *frame,
                        size_t *frame_len);

  /**
   * Send configuration commit after schedule write operations.
   *
   * CRITICAL: This must be called after any schedule write (OpSpec 0xB3) to persist changes.
   * Sends a Class 10 SET command (OpSpec 0x93) to Object 84, SubID 1 with the current
   * ClockProgramOverview structure.
   *
   * See: reference/alpha-hwr/src/alpha_hwr/services/control.py:_send_configuration_commit
   *
   * @return True if commit sent successfully, false otherwise
   */
  bool send_configuration_commit();

  // -------------------------------------------------------------------------
  // Member Variables
  // -------------------------------------------------------------------------

  core::Transport &transport_;
  core::Session &session_;

  ScheduleCallback schedule_callback_;
  WriteCallback write_callback_;

  // Cached state for async read operations
  bool schedule_state_cached_;       ///< True if schedule_enabled_ contains valid data
  bool schedule_enabled_;             ///< Cached schedule enabled state
  uint32_t last_state_poll_ms_;       ///< Timestamp of last state poll request
  
  // Cached ClockProgramOverview structure (10 bytes) for read-modify-write
  bool overview_cached_;              ///< True if overview_structure_ contains valid data
  uint8_t overview_structure_[10];    ///< Complete 10-byte ClockProgramOverview structure

  static constexpr const char *TAG = "schedule_service";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
