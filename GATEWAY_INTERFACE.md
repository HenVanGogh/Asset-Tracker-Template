# Gateway Interface Documentation

This document describes the interface between the Gateway Controller (e.g., nRF9160) and the BLE Sensor Hub (nRF5340).

## Hardware Interface (UART)

The communication uses **UART1** with Hardware Flow Control (RTS/CTS).

- **Baud Rate**: 115200 (Default for Zephyr `console` / `uart`)
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: RTS/CTS (Enabled in `app.overlay`)

## Incoming Data (from nRF5340 to Gateway)

The nRF5340 sends line-based ASCII text terminated by `\n`.

### 1. Sensor Reporting
Legacy format for periodic sensor reports (if active):
```
<name>:<temperature>,<humidity>,<battery_mv>
```

### 2. Command Responses
Responses to specific commands sent by the gateway.
- **Status**: `STATUS:UP=<ms>,SENSORS=<count>,LAST_DET_AGO=<ms>`
- **List**:
  ```
  SENSORS_LIST_START
  <index>:<name> addr=<mac> rssi=<rssi> bat=<mv>
  ...
  SENSORS_LIST_END
  ```
- **Connection**:
  - `CONNECTING...`
  - `DISCONNECTING...`
  - `SEND_OK`
  - `RX_FROM_SENSOR:<data>` (Data received from connected sensor via NUS)

### 3. Errors
Error messages start with `ERR:`
- `ERR:UNKNOWN_CMD`
- `ERR:SENSOR_NOT_FOUND`
- `ERR:SEND_FAIL_<code >`

---

## Outgoing Commands (from Gateway to nRF5340)

Send these strings over UART, terminated by `\n`.

| Command | Description | Example |
| :--- | :--- | :--- |
| `status` | Request system status. | `status\n` |
| `list` | List detailed info of discovered sensors. | `list\n` |
| `connect <name>` | Connect to a sensor by name (must be in list). | `connect nRF_Sensor_01\n` |
| `disconnect` | Disconnect from the currently connected sensor. | `disconnect\n` |
| `send <data>` | Send a string to the connected sensor via NUS. | `send LED_ON\n` |
| `reboot` | Reboot the nRF5340. | `reboot\n` |

---

## MQTT Command Structure (Proposal)

To control the nRF5340 via MQTT (e.g., AWS IoT Core), usage of a specific topic is recommended.

**Topic**: `gateway/<device_id>/command`

### Payload Format (JSON)
The nRF9160 should subscribe to the topic and parse JSON payloads.

#### 1. Passthrough Command
Directly forward a command string to UART.
```json
{
  "type": "uart_passthrough",
  "command": "list"
}
```

#### 2. Connect & Interaction (Composite)
Higher-level abstraction to perform a sequence.
```json
{
  "type": "sensor_action",
  "target": "nRF_Sensor_01",
  "action": "write",
  "data": "LED_ON"
}
```
*Implementation on nRF9160:*
1. Send `connect nRF_Sensor_01\n`
2. Wait for `CONNECTING...` (or delay)
3. Send `send LED_ON\n`
4. Wait for `SEND_OK`
5. Send `disconnect\n`

### Proposed Logic for nRF9160

python-like pseudocode for the MQTT callback:

```python
def on_mqtt_message(topic, payload):
    data = json.parse(payload)
    
    if data['type'] == 'uart_passthrough':
        # Append newline if missing
        cmd = data['command'].strip() + '\n'
        uart.write(cmd)
        
    elif data['type'] == 'sensor_action':
        # Simple finite state machine or sequence
        target = data['target']
        payload = data['data']
        
        uart.write(f"connect {target}\n")
        # Wait for connection confirmation is recommended but a simple delay can work for PoC
        sleep_ms(500) 
        
        uart.write(f"send {payload}\n")
        sleep_ms(200)
        
        uart.write("disconnect\n")
```
