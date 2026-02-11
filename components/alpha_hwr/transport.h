/**
 * BLE Transport Layer for GENI Protocol Communication
 * 
 * This module handles low-level BLE packet transport including:
 * - Packet fragmentation and reassembly (BLE 20-byte MTU limit)
 * - BLE write operations with automatic packet splitting
 * - Notification handling with callback mechanism
 * 
 * The transport layer sits between the BLE client (ESP-IDF BLE) and the
 * protocol layer, providing a clean abstraction for sending/receiving
 * GENI protocol packets.
 * 
 * Architecture:
 * ┌─────────────────────────────────┐
 * │   Protocol Layer / Services     │
 * │  (sends/receives GENI packets)  │
 * └────────────┬────────────────────┘
 *              │
 *              ▼
 * ┌─────────────────────────────────┐
 * │       Transport Layer           │
 * │  - Packet reassembly            │
 * │  - BLE write with splitting     │
 * │  - Notification callbacks       │
 * └────────────┬────────────────────┘
 *              │
 *              ▼
 * ┌─────────────────────────────────┐
 * │      ESP-IDF BLE GATT           │
 * │  - BLE GATT operations          │
 * │  - Connection management        │
 * └─────────────────────────────────┘
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/transport.py
 */

#pragma once

#include "esphome/core/component.h"
#include <vector>
#include <functional>

namespace esphome {
namespace alpha_hwr {
namespace core {

/**
 * BLE transport for GENI protocol packets.
 * 
 * Manages low-level BLE communication including notification handling
 * and packet reassembly. Provides a clean interface for higher-level
 * protocol operations.
 * 
 * Key Features:
 * - Automatic packet reassembly for fragmented BLE notifications
 * - Automatic packet splitting for writes exceeding 20-byte MTU
 * - Frame start detection (0x24/0x27)
 * - Length-based completion checking
 * - Buffer overflow protection
 * - Callback mechanism for complete packets
 * 
 * Packet Fragmentation:
 * ---------------------
 * BLE limits notifications to ~20 bytes (MTU - headers). GENI packets
 * can be larger, so they arrive in fragments:
 * 
 * Fragment 1: [0x24][LEN][...]           (frame start byte + data)
 * Fragment 2: [...]                       (continuation)
 * Fragment N: [...][CRC_H][CRC_L]         (end of packet)
 * 
 * The transport accumulates fragments until:
 *   received_bytes >= (length_field + 4)
 * 
 * Then delivers the complete packet via callback.
 * 
 * Frame Start Bytes:
 * ------------------
 * 0x24 = Response frame (pump -> client)
 * 0x27 = Request frame (client -> pump, also echoed back)
 * 
 * These bytes indicate a new packet is starting, not a continuation.
 * 
 * Example Usage:
 * --------------
 * ```cpp
 * Transport transport;
 * 
 * // Set callback for complete packets
 * transport.set_packet_callback([this](const uint8_t* data, size_t len) {
 *   decode_packet(data, len);
 * });
 * 
 * // Handle BLE notification (called from ESP-IDF callback)
 * void on_ble_notification(const uint8_t* data, size_t len) {
 *   transport.on_notification(data, len);
 * }
 * 
 * // Write packet (automatically splits if >20 bytes)
 * uint8_t packet[11];
 * build_class10_read(0x570045, packet);
 * transport.write_packet(packet, 11, ble_write_func);
 * ```
 * 
 * Notes for Reimplementation:
 * ---------------------------
 * This C++ implementation differs from Python reference in key ways:
 * 
 * 1. **No Async/Await**: ESP-IDF uses callback-based BLE, not async
 * 2. **No Transaction Lock**: ESPHome runs in single loop context
 * 3. **No Response Queue**: Direct callback delivery instead
 * 4. **No Keep-Alive**: Managed at higher layer (poll_telemetry)
 * 
 * Python's asyncio primitives (Lock, Queue) aren't needed because
 * ESPHome components run sequentially in the main loop.
 */
class Transport {
 public:
  /**
   * Callback type for complete packets.
   * 
   * Called when a complete GENI packet has been reassembled.
   * 
   * Parameters:
   *   data - Pointer to complete packet data
   *   len  - Length of complete packet in bytes
   */
  using PacketCallback = std::function<void(const uint8_t* data, size_t len)>;

  /**
   * Callback type for BLE write operations.
   * 
   * This abstracts the actual BLE write call, allowing the transport
   * to be independent of ESP-IDF details.
   * 
   * Parameters:
   *   data - Pointer to data to write
   *   len  - Length of data in bytes
   * 
   * Returns:
   *   true if write was successful, false otherwise
   */
  using WriteCallback = std::function<bool(const uint8_t* data, size_t len)>;

  Transport();

  /**
   * Set callback for complete packets.
   * 
   * The callback will be invoked whenever a complete GENI packet
   * has been reassembled from BLE notification fragments.
   * 
   * @param callback Function to call with complete packets
   */
  void set_packet_callback(PacketCallback callback);

  /**
   * Handle incoming BLE notification data.
   * 
   * This should be called from the ESP-IDF BLE notification event handler.
   * It accumulates packet fragments and invokes the packet callback when
   * a complete packet is ready.
   * 
   * Packet Reassembly Logic:
   * 1. If data[0] is 0x24 or 0x27 (frame start), start new packet
   * 2. Otherwise, append to current buffer
   * 3. Check if complete: buffer_size >= (length_field + 4)
   * 4. If complete, invoke callback and clear buffer
   * 
   * @param data Pointer to notification data
   * @param len Length of notification data
   */
  void on_notification(const uint8_t* data, size_t len);

  /**
   * Write packet to BLE characteristic.
   * 
   * Automatically splits packets exceeding 20 bytes into multiple writes.
   * This is required because BLE MTU limits individual writes.
   * 
   * For packets <= 20 bytes:
   *   - Single write
   * 
   * For packets > 20 bytes:
   *   - Write first 20 bytes
   *   - Write remaining bytes
   * 
   * @param data Pointer to packet data
   * @param len Length of packet
   * @param write_func Callback to perform actual BLE write
   * @return true if all writes succeeded, false otherwise
   */
  bool write_packet(const uint8_t* data, size_t len, WriteCallback write_func);

  /**
   * Reset transport state.
   * 
   * Clears reassembly buffer and flags. Should be called on:
   * - BLE disconnection
   * - Fatal errors
   * - Manual reset request
   */
  void reset();

  /**
   * Check if currently reassembling a packet.
   * 
   * @return true if reassembly in progress
   */
  bool is_reassembling() const { return reassembling_; }

  /**
   * Get current reassembly buffer size.
   * 
   * Useful for debugging and monitoring.
   * 
   * @return Number of bytes in reassembly buffer
   */
  size_t get_buffer_size() const { return reassembly_buffer_.size(); }

  /**
   * Get expected packet length.
   * 
   * @return Expected total packet length in bytes (0 if not reassembling)
   */
  uint16_t get_expected_length() const { return expected_packet_length_; }

 private:
  /**
   * Check if buffer contains a frame start byte.
   * 
   * Frame start bytes:
   *   0x24 = Response frame
   *   0x27 = Request frame (echo)
   * 
   * @param data First byte of notification
   * @return true if this is a frame start byte
   */
  bool is_frame_start(uint8_t byte) const;

  /**
   * Extract expected packet length from buffer.
   * 
   * GENI packet structure:
   *   [Frame Start][Length][Payload...][CRC_H][CRC_L]
   * 
   * Total length = Length field + 4 bytes (start + length + 2-byte CRC)
   * 
   * @return Expected total packet length
   */
  uint16_t calculate_expected_length() const;

  // Reassembly state
  bool reassembling_;                         ///< True if currently accumulating packet fragments
  std::vector<uint8_t> reassembly_buffer_;    ///< Buffer for accumulating packet fragments
  uint16_t expected_packet_length_;           ///< Expected total packet length

  // Callback for complete packets
  PacketCallback packet_callback_;            ///< Called when packet is complete

  // Constants
  static constexpr size_t MAX_PACKET_SIZE = 256;  ///< Maximum GENI packet size (safety limit)
  static constexpr size_t BLE_MTU_LIMIT = 20;     ///< BLE write size limit (MTU - headers)
  static constexpr uint8_t FRAME_START_RESPONSE = 0x24;  ///< Response frame start byte
  static constexpr uint8_t FRAME_START_REQUEST = 0x27;   ///< Request frame start byte (echo)
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
