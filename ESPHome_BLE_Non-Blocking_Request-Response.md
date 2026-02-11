# **Architectural Patterns for Synchronous BLE Transactions in Asynchronous ESPHome Environments**

## **1\. Executive Summary**

The implementation of synchronous communication protocols—specifically those requiring a deterministic write \-\> wait \-\> read/notify \-\> commit sequence—within the non-blocking, single-threaded environment of ESPHome represents a significant architectural challenge. Unlike multi-threaded operating systems where a thread can be blocked to await a response, ESPHome operates on a cooperative multitasking model driven by a central loop(). Blocking this loop for the duration of a Bluetooth Low Energy (BLE) transaction, which can span hundreds of milliseconds to several seconds, results in "event loop starvation," causing WiFi disconnections, sensor data loss, and hardware watchdog resets.  
This report provides a comprehensive analysis of the design patterns required to implement robust, non-blocking BLE transactions in C++ custom components. By synthesizing evidence from existing high-complexity components—such as the Grundfos Alpha3 integration, Nextion display drivers, and the core BLE Tracker—this document establishes a canonical reference for developers. The analysis identifies two primary architectural solutions: the **Finite State Machine (FSM)** for rigid, multi-step protocols, and the **Asynchronous Command Queue** for high-throughput, bursty interactions.  
Key findings indicate that successful implementation relies on three critical mechanisms:

1. **State Retention:** The use of class member variables to persist transaction context across disjointed iterations of the main application loop.  
2. **Event Interception:** The utilization of the gattc\_event\_handler callback to drive state transitions based on interrupts from the ESP-IDF Bluetooth stack, rather than temporal polling.  
3. **Safety Scheduling:** The implementation of non-blocking timeout mechanisms via the App.scheduler to prevent state deadlocks when remote devices fail to respond.

The report details the exact C++ implementation strategies for these patterns, offering a theoretical framework supported by code analysis of production-grade ESPHome components.

## **2\. The Architectural Context of ESPHome**

To understand the necessity of specific design patterns such as State Machines and Command Queues, one must first rigorously analyze the execution environment in which ESPHome components reside. The constraints imposed by this environment dictate the valid solution space for any communication protocol.

### **2.1 The Cooperative Multitasking Model**

ESPHome does not run on a pre-emptive operating system in the traditional desktop sense. While the ESP32 chipset utilizes FreeRTOS, the ESPHome application layer abstraction flattens the complexity into a single "Main Thread" of execution. This is the Application::loop().  
In this architecture, the system iterates through a linked list of registered Component objects, calling their loop() method sequentially. This is a **cooperative** model because the system relies on each component to voluntarily yield control back to the central scheduler.

* **The Time Budget:** Ideally, a component's loop() execution should complete in under 10-20 milliseconds.  
* **The Penalty of Blocking:** If a developer implements a naive synchronous wait—for instance, client-\>write(); while(\!available());—the CPU is held hostage by that component. During this "blocked" time, the WiFi stack cannot process keep-alive packets, the API server cannot respond to Home Assistant, and other sensors cannot be polled.  
* **The Watchdog Threat:** If the blocking duration exceeds the hardware watchdog timer (typically a few seconds), the ESP32 will hard-reset, assuming the firmware has frozen.

Given that BLE transactions involve radio latency, connection intervals (often 30ms to 200ms), and potential retransmissions, a single transaction can easily exceed the safe execution budget of a loop() cycle. Therefore, the transaction must be "sliced" across time.

### **2.2 The ESP32 Bluetooth Stack (ESP-IDF)**

The underlying mechanism handling Bluetooth operations is the ESP-IDF Bluetooth stack (Bluedroid or NimBLE). This stack runs in its own FreeRTOS tasks, separate from the ESPHome loop().

* **Asynchronous API:** Almost all BLE operations in ESP-IDF are asynchronous. When a component calls esp\_ble\_gattc\_write\_char, the function places the command into a lower-level queue and returns immediately. It does *not* wait for the data to be transmitted, nor does it wait for an acknowledgment.  
* **The Callback Interface:** The stack communicates back to the application layer via events. When a write is acknowledged by the remote device, the stack triggers an ESP\_GATTC\_WRITE\_CHAR\_EVT. When a notification arrives, it triggers ESP\_GATTC\_NOTIFY\_EVT.  
* **The Bridge:** The gattc\_event\_handler method in ESPHome's BLEClientNode class acts as the bridge between the high-priority Bluetooth task and the component's logic.

This separation of concerns—request issuance in the Application Loop and response handling in the Bluetooth Callback—necessitates a design pattern that can bridge the temporal gap between the two.

## **3\. Design Pattern I: The Finite State Machine (FSM)**

The Finite State Machine is the most robust and deterministic pattern for handling protocols that require a strict sequence of operations. It is particularly suited for scenarios where the ESP32 must perform a handshake, authenticate, or read a sequence of characteristics in a specific order before the device is considered "ready."

### **3.1 Theoretical Structure of a BLE FSM**

In the context of a non-blocking write \-\> wait \-\> notify transaction, the "State" represents the current expectation of the system. The FSM transforms the linear logic of the protocol into a set of discrete, stable conditions.

| State Name | Logical Description | Trigger for Transition |
| :---- | :---- | :---- |
| IDLE | The component is dormant or passively monitoring. | update() interval or user action. |
| CONNECTING | A connection request has been issued; waiting for link establishment. | ESP\_GATTC\_OPEN\_EVT |
| DISCOVERING | resolving Service and Characteristic UUIDs to handles. | ESP\_GATTC\_SEARCH\_CMPL\_EVT |
| SUBSCRIBING | enabling Notifications (writing 0x01 to CCCD). | ESP\_GATTC\_REG\_FOR\_NOTIFY\_EVT |
| SENDING | The request payload is being transmitted to the device. | ESP\_GATTC\_WRITE\_CHAR\_EVT |
| AWAITING | The request is sent; the system is waiting for the device to reply. | ESP\_GATTC\_NOTIFY\_EVT |
| PROCESSING | Data has been received and is being validated/parsed. | Validation success/failure. |
| ERROR | A timeout or protocol violation occurred. | Timer expiration or Disconnect event. |

### **3.2 Component Analysis: Grundfos Alpha3**

The **Grundfos Alpha3** component 1 provides a canonical example of a complex FSM implementation in ESPHome. This component must interface with a pump using the "GENI" protocol, which runs over BLE but mimics a serial request-response structure.

#### **3.2.1 Initialization and Connection**

The component inherits from esphome::ble\_client::BLEClientNode, allowing it to piggyback on the central BLEClient connection manager.

* **Discovery:** Upon connection, the component does not immediately assume it can write. It must first identify the characteristic handles. The source code defines ALPHA3\_GENI\_SERVICE\_UUID and ALPHA3\_GENI\_CHARACTERISTIC\_UUID.2 The FSM remains in a CONNECTING or DISCOVERING phase until get\_characteristic returns a valid pointer.3

#### **3.2.2 The Request-Response Cycle**

The core transaction is triggered by the update() loop (inherited from PollingComponent).

1. **Initiation:** The update() method calls send\_request\_.3 This method constructs the specific byte sequence required by the GENI protocol (e.g., 0x7E...) and calls the ESP-IDF function esp\_ble\_gattc\_write\_char.  
2. **Transition to Wait:** Crucially, send\_request\_ does *not* wait. It returns immediately. Internally, the component implicitly transitions to a state where it expects a response.  
3. **The Event Handler:** The Alpha3::gattc\_event\_handler 2 is the engine of the FSM. It contains a switch statement handling various events.  
   * **Write Acknowledgement:** If the protocol uses "Write with Response," the handler listens for ESP\_GATTC\_WRITE\_CHAR\_EVT. This confirms the pump received the command.  
   * **Notification Reception:** The handler listens for ESP\_GATTC\_NOTIFY\_EVT. This event carries the payload (param-\>notify.value).  
4. **Response Processing:** When the notification arrives, the handle\_geni\_response\_ method 2 is invoked. This method validates the checksum and header of the GENI packet. If valid, it updates the sensors (Flow, Head, Power, etc.).1

#### **3.2.3 Error Handling in the FSM**

The Alpha3 implementation must handle cases where the pump does not respond.

* **Protocol Mismatches:** The code includes logic to check is\_current\_response\_type\_.3 If the response type doesn't match the request, the FSM ignores it, effectively remaining in the waiting state or resetting to IDLE.  
* **Timeouts:** While the specific snippet doesn't show the timeout logic, a robust FSM uses the set\_timeout function 4 upon entering the AWAITING state. If the gattc\_event\_handler does not receive the notification within a defined window (e.g., 2000ms), the timeout callback forces the FSM back to IDLE or ERROR to prevent the component from hanging indefinitely.

### **3.3 Implementation Details for FSMs**

To implement this pattern, the developer must define the states clearly in the header file.

C++

// Header (.h)  
enum State {  
  STATE\_IDLE,  
  STATE\_AWAITING\_NOTIFY,  
};  
State state\_{STATE\_IDLE};

In the implementation (.cpp), the logic is split:  
**In loop():**

C++

void MyComponent::loop() {  
  // Only initiate if IDLE. Never initiate if already waiting.  
  if (this-\>state\_ \== STATE\_IDLE && should\_send\_command()) {  
    send\_ble\_command();  
    this-\>state\_ \= STATE\_AWAITING\_NOTIFY;  
    // Set a safety net  
    this-\>set\_timeout("reply\_timeout", 1000, \[this\]() {  
      ESP\_LOGW(TAG, "Timeout waiting for notification");  
      this-\>state\_ \= STATE\_IDLE;  
    });  
  }  
}

**In gattc\_event\_handler():**

C++

void MyComponent::gattc\_event\_handler(esp\_gattc\_cb\_event\_t event,...) {  
  if (event \== ESP\_GATTC\_NOTIFY\_EVT) {  
    if (this-\>state\_ \== STATE\_AWAITING\_NOTIFY) {  
      // Success\! Cancel the failure timeout.  
      this-\>cancel\_timeout("reply\_timeout");  
      process\_data(param-\>notify.value);  
      this-\>state\_ \= STATE\_IDLE;   
    }  
  }  
}

This strict separation ensures that the loop() is never blocked. The "Wait" is purely logical—the state\_ variable prevents new commands from being sent, but the processor is free to handle other tasks.

## **4\. Design Pattern II: The Asynchronous Command Queue**

While the FSM is ideal for periodic, predictable polling (like reading a sensor every 60 seconds), it fails when facing **bursty, user-driven interactions**. Consider a scenario where a user toggles a switch in Home Assistant, then immediately changes a dimmer level, and then changes a color temperature—all within 500ms.

* An FSM in the AWAITING state would typically reject the subsequent commands ("System Busy").  
* A naive implementation might try to blast all three commands to the BLE stack instantly. This often leads to the bta\_gattc\_enqueue error 5, where the internal ESP-IDF queue overflows, causing packet loss or stack crashes.

The **Command Queue** pattern solves this by buffering requests in the application layer and feeding them to the BLE stack serially, waiting for each transaction to complete (or timeout) before sending the next.

### **4.1 Theoretical Structure of a Command Queue**

The Command Queue acts as a buffer between the high-speed generation of commands (from the API or Automation engine) and the low-speed consumption of commands (by the BLE peripheral).  
**Key Components:**

1. **The Container:** A FIFO (First-In-First-Out) structure, typically std::deque or std::vector, storing command objects.  
2. **The Command Object:** A struct containing the data to be sent, the target characteristic, the type of write (With/Without Response), and a callback to execute upon completion.  
3. **The Consumer:** A logic block (usually in loop()) that checks if the "Line is Clear" before creating a transaction from the head of the queue.  
4. **The Pacer:** A timing mechanism to ensure minimum spacing between commands, preventing receiver buffer overflows.

### **4.2 Component Analysis: Nextion Display Driver**

Although the **Nextion Display** component 6 controls a UART display, the problem it solves is identical to the BLE Request-Response challenge. The display cannot handle a flood of serial commands; it requires pacing and sometimes acknowledgment. The ESPHome implementation of Nextion serves as the "Gold Standard" for this pattern in C++.

#### **4.2.1 Queue Management**

The component defines a protected member std::deque\<NextionQueue \*\> nextion\_queue\_.7

* **Adding Commands:** When a user action occurs (e.g., set\_component\_text), the method does not write to the UART immediately. Instead, it calls add\_no\_result\_to\_queue\_ 6, which pushes a new NextionQueue object onto the deque.  
* **Queue Limits:** To prevent memory exhaustion (a critical concern on ESP8266/ESP32), the component enforces a max\_queue\_size\_.7 If the queue is full, new commands may be dropped or oldest commands discarded, depending on the strategy.

#### **4.2.2 The Pacer Mechanism**

The NextionCommandPacer class 7 is a sophisticated addition. It tracks the timestamp of the last sent command.

* **Spacing:** In the loop(), the consumer checks command\_pacer\_-\>can\_send(). This function returns true only if millis() \- last\_command\_time\_ \> spacing\_ms\_. This enforces a mandatory "dead time" between transactions, allowing the remote device (display or BLE peripheral) to process the previous frame.  
* **Relevance to BLE:** This is crucial for BLE devices that process writes in firmware (e.g., writing to flash). Flooding them causes them to disconnect.

#### **4.2.3 Processing Loop**

The Nextion::loop() method acts as the queue consumer:

1. **Check Condition:** Is the queue empty? Is the device connected? Is the pacer ready?  
2. **Fetch:** queue\_item \= nextion\_queue\_.front().  
3. **Execute:** Transmit the data.  
4. **Wait State:** If the command requires a response (like a query), the component sets a flag waiting\_for\_response\_. The loop will *skip* processing further queue items until this flag is cleared.  
5. **Completion:** When data arrives (via UART interrupt), the waiting\_for\_response\_ flag is cleared, allowing the next iteration of loop() to process the next command.

### **4.3 Adapting the Queue for BLE**

To adapt this specifically for BLE, we replace the UART Write/Read with BLE Write/Notify.  
**The Queue Item Struct:**

C++

struct BLECommand {  
  uint16\_t handle;  
  std::vector\<uint8\_t\> payload;  
  bool require\_response;  
  std::function\<void(bool)\> on\_complete; // Callback for success/fail  
};

**The Loop Logic:**

C++

void MyBLEComponent::loop() {  
  if (this-\>command\_queue\_.empty()) return;  
  if (this-\>is\_awaiting\_notify\_) return; // BUSY

  auto \&cmd \= this-\>command\_queue\_.front();  
    
  // Send to stack  
  auto write\_type \= cmd.require\_response? ESP\_GATTC\_WRITE\_TYPE\_RSP : ESP\_GATTC\_WRITE\_TYPE\_NO\_RSP;  
  esp\_ble\_gattc\_write\_char(..., cmd.handle, cmd.payload.size(), cmd.payload.data(), write\_type,...);

  if (cmd.require\_response) {  
    this-\>is\_awaiting\_notify\_ \= true;  
    this-\>set\_timeout("cmd\_timeout", 2000, \[this\](){  
       // Timeout logic: drop command, clear flag  
       this-\>command\_queue\_.pop\_front();  
       this-\>is\_awaiting\_notify\_ \= false;  
    });  
  } else {  
    // Fire-and-forget: pop immediately or wait for Pacer  
    this-\>command\_queue\_.pop\_front();  
  }  
}

This pattern ensures that no matter how many times the user mashes the button in Home Assistant, the BLE stack sees a clean, spaced stream of commands.

## **5\. Advanced Implementation Details**

Implementing these patterns requires mastery of several specific ESPHome and ESP-IDF primitives.

### **5.1 The gattc\_event\_handler Central Nervous System**

The gattc\_event\_handler 8 is the single most important method for any custom BLE component. It is the only thread-safe place to receive feedback from the stack.

* **ESP\_GATTC\_WRITE\_CHAR\_EVT:** This event confirms that a "Write with Response" has been acknowledged by the remote device. In a Queue pattern, receiving this event (if notifications aren't used for data) is the signal to advance the queue.  
* **ESP\_GATTC\_NOTIFY\_EVT:** This is the standard "Response" carrier. The data is located in param-\>notify.value and param-\>notify.value\_len.  
  * **Warning:** The data pointer in param-\>notify.value is valid *only* for the duration of the callback. You **must** copy this data to a local buffer or std::string if you intend to process it later or pass it to a lambda.  
* **ESP\_GATTC\_DISCONNECT\_EVT:** If the device disconnects mid-transaction, the handler must clear the waiting\_for\_response\_ flag and potentially flush the queue to prevent the system from getting stuck in a "Waiting" state upon reconnection.

### **5.2 Scheduler and Timeouts**

The mechanism for preventing deadlocks is the ESPHome Scheduler, accessed via App.scheduler or the helper methods in Component.

* **Evolution of API:** Older ESPHome versions used string-based names for timeouts (e.g., set\_timeout("my\_timeout",...)). Recent updates (2025.x) 10 have moved toward integer-based IDs or specific method overrides to reduce heap allocation and lookup times.  
* **Best Practice:**  
  * Use set\_timeout(uint32\_t timeout, std::function\<void()\> &\&f) for fire-and-forget safety nets.  
  * Store the timeout logic in a variable if you need to cancel it.  
  * **Crucially:** Always call cancel\_timeout in the success path. If the notification arrives in 100ms, the 2000ms timeout sitting in the scheduler must be removed, otherwise it will trigger later and potentially mess up the state of a *future* transaction.

### **5.3 Write Types: RSP vs. NO\_RSP**

The choice between ESP\_GATT\_WRITE\_TYPE\_RSP and ESP\_GATT\_WRITE\_TYPE\_NO\_RSP 12 fundamentally changes the "Wait" logic.

* **With Response (RSP):** The protocol guarantees delivery. The transaction time \= Round Trip Time (RTT). This is safer for queues but slower. The completion trigger is ESP\_GATTC\_WRITE\_CHAR\_EVT.  
* **Without Response (NO\_RSP):** The transaction time is effectively the serial transmission time. It is fast but unreliable. The stack may drop the packet if the queue is full. The completion trigger is effectively "immediate," but the application should enforce a "pacing" delay (e.g., 20-30ms) to allow the air interface to clear.

## **6\. Failure Modes and Mitigation strategies**

A robust report must address what happens when things go wrong.

### **6.1 The bta\_gattc\_enqueue Error**

Multiple research snippets 5 highlight the error: BT\_APPL: bta\_gattc\_enqueue(), the gattc command queue is full.

* **Cause:** The application layer is pushing commands into the ESP-IDF stack faster than the stack can transmit them over the air.  
* **Solution:** The Command Queue pattern *is* the mitigation. By strictly limiting the application to one pending write at a time (via the waiting\_for\_response flag), you ensure the underlying stack queue never fills up.

### **6.2 The "Ghost" Notification**

Sometimes, a device may send a notification that wasn't requested (e.g., a periodic heartbeat).

* **Impact:** If the FSM is in AWAITING state for a specific command, and a generic heartbeat notification arrives, the FSM might mistake it for the answer.  
* **Mitigation:** The parse\_response logic must validate the payload. If the data doesn't match the expected structure of the response, the FSM should ignore it and remain in the AWAITING state (until the correct data arrives or the timeout fires).

### **6.3 Race Conditions**

A common race condition occurs when update() (from PollingComponent) fires while a user-initiated command is in progress.

* **Scenario:** The user toggles a switch. The Queue sends the "Toggle" command. The FSM enters BUSY. 10ms later, update() fires and tries to send a "Read Status" command.  
* **Mitigation:** The update() method must check the state. if (this-\>state\_\!= IDLE) return;. It is better to skip one polling cycle than to corrupt an active transaction.

## **7\. Comparative Summary of Patterns**

| Feature | Finite State Machine (FSM) | Command Queue |
| :---- | :---- | :---- |
| **Best For** | Rigid, multi-step sequences (e.g., Handshakes). | Bursty, user-driven actions (e.g., UI controls). |
| **Complexity** | Moderate. Logic is explicit. | High. Requires memory management and containers. |
| **Throughput** | Low. Strictly serial and typically synchronous-logic. | High. Can buffer bursts and smooth out traffic. |
| **Flow Control** | Implicit (State blocks transition). | Explicit (Pacer and Ack waiting). |
| **Example Component** | Grundfos Alpha3 1 | Nextion Display 7 |

## **8\. Conclusion**

Implementing synchronous "Write \-\> Wait \-\> Notify" transactions in ESPHome requires a paradigm shift from linear, blocking code to event-driven architectures. The naive approach of using delay() is incompatible with the cooperative multitasking nature of the ESPHome loop().  
Through the analysis of the **Grundfos Alpha3** and **Nextion** components, we have identified that the **Finite State Machine** is the optimal pattern for predictable, periodic protocol interactions, while the **Asynchronous Command Queue** is essential for handling unpredictable, high-volume user interactions.  
Success depends on utilizing the gattc\_event\_handler as the primary driver of state transitions, enabling the application to "sleep" (yield execution) while waiting for the slow BLE radio operations to complete. By coupling this with defensive programming techniques—specifically the App.scheduler for timeouts and strict command pacing—developers can build robust C++ custom components that maintain high reliability even in complex, noisy RF environments. These patterns ensure that the ESPHome node remains responsive to WiFi and API events, preserving the stability of the entire home automation node.  
---

**Data Sources and Citations:**

* **State Machine Logic:** 2 (Alpha3 Component Analysis).  
* **Command Queue & Pacing:** 6 (Nextion Component Analysis).  
* **BLE Client/Notify:** 8 (BLE Client Architecture).  
* **Timeout & Scheduling:** 4 (Scheduler API).  
* **Protocol Considerations:** 5 (Stack limitations, Write Types, and Errors).

#### **Works cited**

1. Grundfos Alpha3 \- ESPHome \- Smart Home Made Simple, accessed February 11, 2026, [https://esphome.io/components/sensor/alpha3/](https://esphome.io/components/sensor/alpha3/)  
2. esphome/components/alpha3/alpha3.h Source File, accessed February 11, 2026, [https://api-docs.esphome.io/alpha3\_8h\_source](https://api-docs.esphome.io/alpha3_8h_source)  
3. esphome/components/alpha3/alpha3.cpp Source File, accessed February 11, 2026, [https://api-docs.esphome.io/alpha3\_8cpp\_source](https://api-docs.esphome.io/alpha3_8cpp_source)  
4. Code \- ESPHome Developer Documentation, accessed February 11, 2026, [https://developers.esphome.io/contributing/code/](https://developers.esphome.io/contributing/code/)  
5. Delay in controlling BLE switch due to gattc queue full · Issue \#4205 \- GitHub, accessed February 11, 2026, [https://github.com/esphome/issues/issues/4205](https://github.com/esphome/issues/issues/4205)  
6. Nextion Class Reference \- ESPHome, accessed February 11, 2026, [https://api-docs.esphome.io/classesphome\_1\_1nextion\_1\_1\_nextion](https://api-docs.esphome.io/classesphome_1_1nextion_1_1_nextion)  
7. esphome/components/nextion/nextion.h Source File, accessed February 11, 2026, [https://api-docs.esphome.io/nextion\_8h\_source](https://api-docs.esphome.io/nextion_8h_source)  
8. esphome/components/radon\_eye\_rd200 ... \- ESPHome, accessed February 11, 2026, [https://api-docs.esphome.io/radon\_\_eye\_\_rd200\_8cpp\_source](https://api-docs.esphome.io/radon__eye__rd200_8cpp_source)  
9. esp32\_ble\_tracker.cpp \- ESPHome, accessed February 11, 2026, [https://api-docs.esphome.io/esp32\_\_ble\_\_tracker\_8cpp\_source](https://api-docs.esphome.io/esp32__ble__tracker_8cpp_source)  
10. esphome::ble\_client::BLEBinaryOutput Class Reference, accessed February 11, 2026, [https://api-docs.esphome.io/classesphome\_1\_1ble\_\_client\_1\_1\_b\_l\_e\_binary\_output](https://api-docs.esphome.io/classesphome_1_1ble__client_1_1_b_l_e_binary_output)  
11. esphome::Component Class Reference, accessed February 11, 2026, [https://api-docs.esphome.io/classesphome\_1\_1\_component](https://api-docs.esphome.io/classesphome_1_1_component)  
12. esphome/components/bluetooth\_proxy/bluetooth\_connection.cpp Source File, accessed February 11, 2026, [https://api-docs.esphome.io/bluetooth\_\_connection\_8cpp\_source](https://api-docs.esphome.io/bluetooth__connection_8cpp_source)  
13. esphome/components/ble\_client/automation.h Source File, accessed February 11, 2026, [https://api-docs.esphome.io/components\_2ble\_\_client\_2automation\_8h\_source](https://api-docs.esphome.io/components_2ble__client_2automation_8h_source)  
14. ESPHome 2025.11.0 \- November 2025, accessed February 11, 2026, [https://esphome.io/changelog/2025.11.0/](https://esphome.io/changelog/2025.11.0/)  
15. ESP32 BLE Server and Client (Bluetooth Low Energy) \- Random Nerd Tutorials, accessed February 11, 2026, [https://randomnerdtutorials.com/esp32-ble-server-client/](https://randomnerdtutorials.com/esp32-ble-server-client/)  
16. BLE Client Sensor \- ESPHome \- Smart Home Made Simple, accessed February 11, 2026, [https://esphome.io/components/sensor/ble\_client/](https://esphome.io/components/sensor/ble_client/)  
17. BLE notify truncated to 20 bytes \#4041 \- esphome/issues \- GitHub, accessed February 11, 2026, [https://github.com/esphome/issues/issues/4041](https://github.com/esphome/issues/issues/4041)  
18. Write characteristics to BLE tag? \- Esphome \- Reddit, accessed February 11, 2026, [https://www.reddit.com/r/Esphome/comments/qel1we/write\_characteristics\_to\_ble\_tag/](https://www.reddit.com/r/Esphome/comments/qel1we/write_characteristics_to_ble_tag/)