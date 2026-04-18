# MQTT to BLE Device Communication Guide

> **This document is superseded.** See the new comprehensive documentation:
> - [MQTT_COMMANDS.md](MQTT_COMMANDS.md) — Master reference and quick-reference table
> - [MQTT_COMMANDS_ESL.md](MQTT_COMMANDS_ESL.md) — ESL/BLE tag and UART sensor commands
> - [MQTT_COMMANDS_DEVICE.md](MQTT_COMMANDS_DEVICE.md) — Device system commands (reboot, status, location)
> - [MQTT_COMMANDS_FOTA.md](MQTT_COMMANDS_FOTA.md) — Firmware update via MQTT
> - [MQTT_COMMANDS_MQTT_CONFIG.md](MQTT_COMMANDS_MQTT_CONFIG.md) — Runtime MQTT broker configuration

---

This guide explains how to send MQTT commands to communicate with BLE devices through the gateway device.

## Overview

The gateway device subscribes to an MQTT topic and forwards commands to connected BLE devices via UART. There are two main approaches for sending commands:

1. **Silent Passthrough** - Commands forwarded without MQTT acknowledgment
2. **Acknowledged Commands** - Commands forwarded with MQTT response confirmation

## MQTT Configuration

### Connection Details
- **Broker**: Configured via `CONFIG_APP_CUSTOM_MQTT_BROKER_HOSTNAME`
- **Port**: Configured via `CONFIG_APP_CUSTOM_MQTT_BROKER_PORT`
- **Subscribe Topic**: Configured via `CONFIG_APP_CUSTOM_MQTT_SUBSCRIBE_TOPIC`
- **Publish Topic**: Configured via `CONFIG_APP_CUSTOM_MQTT_PUBLISH_TOPIC`

Default configuration example:
```
Subscribe Topic: device/<device_id>/commands
Publish Topic: device/<device_id>/data
```

---

## Command Format

### 1. Silent Passthrough (No MQTT Response)

Use this format when you want to forward commands directly to BLE devices without receiving an MQTT acknowledgment from the gateway.

#### JSON Format
```json
{
  "type": "uart_passthrough",
  "command": "<your_command_here>"
}
```

#### Examples

**Send command to specific BLE address:**
```json
{
  "type": "uart_passthrough",
  "command": "BLE_SEND:AA:BB:CC:DD:EE:FF:DATA_PAYLOAD"
}
```

**Send command to BLE device by name:**
```json
{
  "type": "uart_passthrough",
  "command": "BLE_NAME:SensorDevice01:GET_TEMPERATURE"
}
```

**Broadcast to all BLE devices:**
```json
{
  "type": "uart_passthrough",
  "command": "BLE_BROADCAST:SYNC_TIME"
}
```

#### Characteristics
- ✅ Fast execution - no gateway acknowledgment overhead
- ✅ Ideal for high-frequency commands
- ❌ No confirmation that gateway received the command
- ❌ No MQTT response from gateway

---

### 2. Acknowledged Commands (With MQTT Response)

Use this format when you need confirmation that the gateway received and forwarded your command.

#### JSON Format
```json
{
  "command": "uart_command",
  "args": "<your_command_here>"
}
```

#### Examples

**Send command to specific BLE address:**
```json
{
  "command": "uart_command",
  "args": "BLE_SEND:AA:BB:CC:DD:EE:FF:SET_LED_ON"
}
```

**Send command to BLE device by name:**
```json
{
  "command": "uart_command",
  "args": "BLE_NAME:TempSensor:READ_DATA"
}
```

**Query BLE device status:**
```json
{
  "command": "uart_command",
  "args": "BLE_STATUS:12:34:56:78:9A:BC"
}
```

#### Expected Response
```json
{
  "device_id": "gateway_device_001",
  "timestamp": 123456789,
  "received_message": "{\"command\":\"uart_command\",\"args\":\"BLE_SEND:...\"}",
  "response_sequence": 42,
  "status": "command_forwarded",
  "command_processed": "uart_command"
}
```

#### Characteristics
- ✅ Confirmation from gateway that command was received
- ✅ Includes response sequence number for tracking
- ❌ Slightly higher latency due to MQTT response
- ✅ Better for critical commands requiring confirmation

---

## BLE Command Patterns

### Addressing BLE Devices

#### By MAC Address
```
BLE_SEND:<MAC_ADDRESS>:<COMMAND>
```
Example:
```json
{
  "type": "uart_passthrough",
  "command": "BLE_SEND:A4:C1:38:12:34:56:READ_SENSOR"
}
```

#### By Device Name
```
BLE_NAME:<DEVICE_NAME>:<COMMAND>
```
Example:
```json
{
  "type": "uart_passthrough",
  "command": "BLE_NAME:ProbeTemp01:GET_BATTERY"
}
```

#### By Connection Handle (if supported)
```
BLE_HANDLE:<HANDLE_ID>:<COMMAND>
```
Example:
```json
{
  "type": "uart_passthrough",
  "command": "BLE_HANDLE:0x0001:DISCONNECT"
}
```

---

## Common BLE Commands

### 1. Read Sensor Data
```json
{
  "type": "uart_passthrough",
  "command": "BLE_NAME:TempProbe01:READ_ALL"
}
```

### 2. Write Configuration
```json
{
  "command": "uart_command",
  "args": "BLE_SEND:AA:BB:CC:DD:EE:FF:CONFIG:INTERVAL=60"
}
```

### 3. Check Device Status
```json
{
  "command": "uart_command",
  "args": "BLE_NAME:Sensor_Room_A:STATUS"
}
```

### 4. Pair New Device
```json
{
  "command": "uart_command",
  "args": "BLE_PAIR:11:22:33:44:55:66"
}
```

### 5. Disconnect Device
```json
{
  "type": "uart_passthrough",
  "command": "BLE_DISCONNECT:AA:BB:CC:DD:EE:FF"
}
```

### 6. Scan for Devices
```json
{
  "command": "uart_command",
  "args": "BLE_SCAN:30"
}
```
*Note: Argument is scan duration in seconds*

---

## Gateway Control Commands

### 1. Get Gateway Status
```json
{
  "command": "get_status"
}
```

**Response:**
```json
{
  "device_id": "gateway_001",
  "timestamp": 123456789,
  "status": "online",
  "uptime_ms": 3600000,
  "mqtt_state": 2,
  "network_connected": true
}
```

### 2. Trigger Location Update
```json
{
  "command": "get_location"
}
```

**Response:**
```json
{
  "status": "location_requested"
}
```

---

## Best Practices

### 1. **Choose the Right Command Type**
- Use **uart_passthrough** for:
  - High-frequency sensor readings
  - Non-critical commands
  - Commands where gateway acknowledgment isn't needed
  
- Use **uart_command** for:
  - Configuration changes
  - Critical operations
  - When you need confirmation of delivery

### 2. **Error Handling**
Always implement retry logic in your MQTT client:
```python
import paho.mqtt.client as mqtt
import json
import time

def send_ble_command(client, device_address, command, max_retries=3):
    payload = {
        "command": "uart_command",
        "args": f"BLE_SEND:{device_address}:{command}"
    }
    
    for attempt in range(max_retries):
        try:
            result = client.publish("device/gateway_001/commands", 
                                   json.dumps(payload), 
                                   qos=1)
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                print(f"Command sent successfully on attempt {attempt + 1}")
                return True
        except Exception as e:
            print(f"Attempt {attempt + 1} failed: {e}")
            time.sleep(2 ** attempt)  # Exponential backoff
    
    return False
```

### 3. **Command Timeout**
Implement timeouts when waiting for BLE device responses:
```python
import time

def wait_for_ble_response(timeout=10):
    start_time = time.time()
    while (time.time() - start_time) < timeout:
        # Check for UART sensor data on MQTT
        if response_received:
            return response_data
        time.sleep(0.1)
    raise TimeoutError("BLE device did not respond")
```

### 4. **QoS Settings**
- Use **QoS 1** (at least once) for important commands
- Use **QoS 0** (at most once) for high-frequency, non-critical data

---

## Complete Example: Python MQTT Client

```python
import paho.mqtt.client as mqtt
import json
import time

# MQTT Configuration
BROKER = "mqtt.example.com"
PORT = 1883
SUBSCRIBE_TOPIC = "device/gateway_001/data"
PUBLISH_TOPIC = "device/gateway_001/commands"
CLIENT_ID = "control_center_001"

# Callback when connected
def on_connect(client, userdata, flags, rc):
    print(f"Connected with result code {rc}")
    client.subscribe(SUBSCRIBE_TOPIC, qos=1)

# Callback when message received
def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        print(f"Received: {json.dumps(payload, indent=2)}")
        
        # Handle UART sensor responses
        if payload.get("type") == "uart_sensor":
            print(f"BLE Sensor Data: {payload['data']}")
    except json.JSONDecodeError:
        print(f"Invalid JSON: {msg.payload}")

# Initialize client
client = mqtt.Client(CLIENT_ID)
client.on_connect = on_connect
client.on_message = on_message

# Connect to broker
client.connect(BROKER, PORT, 60)
client.loop_start()

# Example 1: Send command to specific BLE device (silent)
def send_silent_command(ble_address, command):
    payload = {
        "type": "uart_passthrough",
        "command": f"BLE_SEND:{ble_address}:{command}"
    }
    client.publish(PUBLISH_TOPIC, json.dumps(payload), qos=0)
    print(f"Silent command sent to {ble_address}")

# Example 2: Send command with acknowledgment
def send_ack_command(ble_address, command):
    payload = {
        "command": "uart_command",
        "args": f"BLE_SEND:{ble_address}:{command}"
    }
    result = client.publish(PUBLISH_TOPIC, json.dumps(payload), qos=1)
    result.wait_for_publish()
    print(f"Acknowledged command sent to {ble_address}")

# Example 3: Send command by device name
def send_to_device_name(device_name, command):
    payload = {
        "type": "uart_passthrough",
        "command": f"BLE_NAME:{device_name}:{command}"
    }
    client.publish(PUBLISH_TOPIC, json.dumps(payload), qos=0)
    print(f"Command sent to device '{device_name}'")

# Usage Examples
try:
    # Wait for connection
    time.sleep(2)
    
    # Send commands to different BLE devices
    send_silent_command("AA:BB:CC:DD:EE:FF", "READ_TEMP")
    time.sleep(1)
    
    send_ack_command("11:22:33:44:55:66", "SET_LED_ON")
    time.sleep(1)
    
    send_to_device_name("TempProbe01", "GET_ALL_DATA")
    
    # Keep running to receive responses
    while True:
        time.sleep(1)
        
except KeyboardInterrupt:
    print("Disconnecting...")
    client.loop_stop()
    client.disconnect()
```

---

## Troubleshooting

### Issue: Commands not reaching BLE devices

**Possible causes:**
1. Gateway not connected to MQTT broker
2. Incorrect MQTT topic
3. BLE device not paired/connected
4. UART communication issue

**Solutions:**
- Check gateway status: `{"command": "get_status"}`
- Verify MQTT topics match configuration
- Ensure BLE device is in range and powered on
- Check UART sensor module logs

### Issue: No response from BLE device

**Possible causes:**
1. BLE device out of range
2. Command syntax error
3. Device address/name incorrect
4. Device not responding to commands

**Solutions:**
- Verify BLE device address/name is correct
- Check device battery level
- Try scanning for devices: `BLE_SCAN:30`
- Use acknowledged commands to confirm gateway received request

### Issue: High command latency

**Possible causes:**
1. Network congestion
2. Using QoS 2 (not recommended)
3. Too many concurrent commands

**Solutions:**
- Use `uart_passthrough` for non-critical commands
- Reduce QoS to 1 or 0
- Implement command queuing with delays

---

## Additional Resources

- **Main Documentation**: [README.md](../README.md)
- **MQTT Production Improvements**: [mqtt_production_improvements.md](mqtt_production_improvements.md)
- **UART Sensor Module**: [modules/uart_sensor.md](modules/uart_sensor.md)
- **Configuration Guide**: [common/configuration.md](common/configuration.md)

---

## Support

For issues or questions:
1. Check gateway logs for error messages
2. Verify network connectivity: `{"command": "get_status"}`
3. Test with simple commands first
4. Review UART sensor module documentation

## License

Copyright (c) 2024 Nordic Semiconductor ASA
SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
