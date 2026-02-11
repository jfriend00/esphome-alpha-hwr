#pragma once

#include <cstdint>
#include <functional>
#include "transport.h"
#include "session.h"
#include "codec.h"
#include "frame_builder.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * Control Mode Enumeration.
 * 
 * Matches ControlMode from GENI Profile (id=112) used by ALPHA HWR pumps.
 * Each mode defines how the pump regulates its operation.
 * 
 * Most Common Modes for ALPHA HWR:
 * - CONSTANT_PRESSURE (0): Maintains constant outlet pressure
 * - CONSTANT_SPEED (2): Runs at fixed RPM
 * - AUTO_ADAPT_* (13-15): Automatically adapts to system demand
 * 
 * Reference: alpha_hwr/constants.py::ControlMode
 */
enum class ControlMode : uint8_t {
  CONSTANT_PRESSURE = 0,              // Constant outlet pressure (meters)
  PROPORTIONAL_PRESSURE = 1,          // Pressure varies linearly with flow
  CONSTANT_SPEED = 2,                 // Fixed RPM operation
  AUTO_ADAPT = 5,                     // Generic AutoAdapt (limited support)
  CONSTANT_FLOW = 8,                  // Constant flow rate (m³/h)
  AUTO_ADAPT_RADIATOR = 13,           // AutoAdapt for radiator systems
  AUTO_ADAPT_UNDERFLOOR = 14,         // AutoAdapt for underfloor heating
  AUTO_ADAPT_COMBINED = 15,           // AutoAdapt for mixed systems
  DHW_ON_OFF = 25,                    // Domestic hot water on/off control
  TEMPERATURE_RANGE = 27,             // Temperature range control (min/max)
  NONE = 254,                         // No control mode active
};

/**
 * Operation Mode Enumeration.
 * 
 * Defines the pump's operational state (running, stopped, etc.).
 * 
 * Reference: alpha_hwr/constants.py::OperationMode
 */
enum class OperationMode : uint8_t {
  AUTO = 0,           // Automatic operation (normal mode)
  STOP = 1,           // Pump stopped
  USER_DEFINED = 4,   // User-defined operation
};

/**
 * Control Service for pump start/stop and mode management.
 * 
 * This service handles all pump control operations:
 * - Starting and stopping the pump
 * - Changing control modes (constant pressure, speed, flow, etc.)
 * - Setting setpoints for each mode
 * - Reading current control mode and operation state
 * 
 * The service abstracts the complexity of Class 10 and Class 3 protocol
 * operations, providing a clean API for pump control.
 * 
 * Architecture:
 * - Uses Transport layer for BLE packet I/O
 * - Validates session state before operations
 * - Builds control packets using FrameBuilder
 * - Sends configuration commits after state changes
 * 
 * Example Usage:
 *   ControlService control(transport, session);
 *   
 *   // Start pump with current mode
 *   control.start();
 *   
 *   // Change to constant pressure mode
 *   control.set_mode(ControlMode::CONSTANT_PRESSURE);
 *   
 *   // Stop pump
 *   control.stop();
 * 
 * Protocol Reference:
 * - Control commands use Class 10 Sub 0x5600, Obj 0x0601
 * - Payload format: [Header][Flag][Mode][Suffix]
 * - Flag: 0x00 = Start/Run, 0x01 = Stop
 * - Configuration commit required after state changes
 * 
 * Python Reference: alpha_hwr/services/control.py
 */
class ControlService {
 public:
  /**
   * Initialize control service.
   * 
   * @param transport BLE transport layer for packet I/O
   * @param session Session manager for state tracking
   */
  ControlService(core::Transport &transport, core::Session &session);
  
  /**
   * Set callback for scheduling delayed operations.
   * 
   * The service needs to schedule configuration commits and retries.
   * The main component provides this callback to delegate scheduling.
   * 
   * @param callback Function to call set_timeout on the component
   */
  void set_schedule_callback(std::function<void(std::function<void()>, uint32_t)> callback);
  
  /**
   * Start the pump.
   * 
   * Sends the start command using Class 10 DataObject method.
   * Optionally switches to a different mode before starting.
   * 
   * @param mode Optional control mode (255 = use current mode)
   * @return True if start command sent successfully
   * 
   * Protocol Notes:
   * - Uses Class 10 Sub 0x5600, Obj 0x0601
   * - Flag byte = 0x00 for start operation
   * - Sends configuration commit after start
   * - Requires authenticated session
   * 
   * Example:
   *   control.start();  // Start with current mode
   *   control.start(static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE));  // Start with specific mode
   * 
   * Reference: control.py::start() lines 165-234
   */
  bool start(uint8_t mode = 255);
  
  /**
   * Stop the pump.
   * 
   * Sends the stop command using Class 10 DataObject method.
   * 
   * @param mode Optional control mode (255 = use current mode)
   * @return True if stop command sent successfully
   * 
   * Protocol Notes:
   * - Uses Class 10 Sub 0x5600, Obj 0x0601
   * - Flag byte = 0x01 for stop operation
   * - Sends configuration commit after stop
   * - Telemetry stream may pause when stopped
   * 
   * Example:
   *   control.stop();
   * 
   * Reference: control.py::stop() lines 236-303
   */
  bool stop(uint8_t mode = 255);
  
  /**
   * Set control mode.
   * 
   * Changes the pump's control mode without changing the setpoint.
   * Tries Class 10 method first, falls back to Class 3 if needed.
   * 
   * @param mode Control mode to set
   * @return True if mode set successfully
   * 
   * Example:
   *   control.set_mode(ControlMode::CONSTANT_PRESSURE);
   *   control.set_mode(ControlMode::AUTO_ADAPT_RADIATOR);
   * 
   * Reference: control.py::set_mode() lines 364-436
   */
  bool set_mode(ControlMode mode);
  
  /**
   * Enable remote control mode.
   * 
   * Enables remote control (Class 3 command ID 7), allowing external
   * control of the pump via BLE/API. When enabled, pump ignores local controls.
   * 
   * @return True if remote mode enabled successfully
   * 
   * Protocol Notes:
   * - Uses Class 3 command: [0x03, 0xC1, 0x07]
   * 
   * Reference: control.py::enable_remote_mode() lines 305-333
   */
  bool enable_remote_mode();
  
  /**
   * Disable remote control mode.
   * 
   * Returns pump to automatic operation based on internal logic.
   * 
   * @return True if remote mode disabled successfully
   * 
   * Protocol Notes:
   * - Uses Class 3 command: [0x03, 0xC1, 0x06]
   * 
   * Reference: control.py::disable_remote_mode() lines 335-362
   */
  bool disable_remote_mode();
   
   /**
    * Get current control mode name as string.
    * 
    * @param mode Control mode value
    * @return Human-readable mode name
    */
   static const char *get_mode_name(ControlMode mode);
   
   /**
    * Get the current control mode.
    * 
    * @return Current ControlMode value
    */
   ControlMode get_current_mode() const { return current_mode_; }
   
   /**
    * Get whether remote mode is enabled.
    * 
    * @return True if remote control is enabled, false if in auto mode
    */
   bool get_remote_enabled() const { return is_remote_mode_enabled_; }
   
  private:
   core::Transport &transport_;
   core::Session &session_;
   ControlMode current_mode_{ControlMode::CONSTANT_SPEED};
   bool is_remote_mode_enabled_{false};  // Track remote mode state
   std::function<void(std::function<void()>, uint32_t)> schedule_callback_;
  
  /**
   * Build GENI protocol packet with CRC.
   * 
   * @param source Source address (typically 0xF8)
   * @param service_id Service ID (typically 0xE7)
   * @param apdu Application Protocol Data Unit
   * @param apdu_len Length of APDU
   * @param packet_out Output buffer
   * @return Total packet length including CRC
   */
  size_t build_geni_packet(uint8_t source, uint8_t service_id, 
                           const uint8_t *apdu, size_t apdu_len,
                           uint8_t *packet_out);
  
  /**
   * Send configuration commit packet.
   * 
   * Required after control operations to persist state changes.
   * 
   * Protocol Notes:
   * - Sub 0x5400, Obj 0xDA01
   * - Hex: 0A9354000100DA0100000A02050005000100000000
   * 
   * Reference: control.py::_send_configuration_commit() lines 1038-1048
   */
  void send_configuration_commit();
  
  /**
   * Class 10 Control Mode Mapping.
   * 
   * Maps ControlMode values to (ModeByte, SuffixBytes) tuples.
   * Used for building Class 10 control packets.
   * 
   * Payload format: 2F 01 00 00 07 00 [Flag] [Mode] [Suffix bytes]
   * - Flag: 00=Start/Run, 01=Stop
   * - Suffix is invariant to Flag
   * 
   * Reference: control.py::_CLASS10_CONTROL_MAP lines 137-145
   */
  struct ControlModeMapping {
    uint8_t mode_byte;
    uint8_t suffix[4];
  };
  
  static const ControlModeMapping CLASS10_CONTROL_MAP[];
  
  /**
   * Get Class 10 control mapping for a mode.
   * 
   * @param mode Control mode
   * @param mapping Output mapping (mode byte + suffix)
   * @return True if mode supported in Class 10
   */
  bool get_class10_mapping(ControlMode mode, ControlModeMapping &mapping);
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
