# MQTT Sensor Whitelist Management Commands

Commands for managing the BLE sensor whitelist on the nRF5340 gateway.
The nRF91 forwards these commands to the gateway over SPI; responses and
unsolicited events are published back on the MQTT data topic.

→ Back to [MQTT_COMMANDS.md](MQTT_COMMANDS.md)

---

## Overview

The gateway maintains a **sensor whitelist** that controls which BLE sensors
it connects to. Two operating modes are supported:

| Mode | Description |
|------|-------------|
| `whitelist` | **Default.** Only sensors whose BLE address has been explicitly added are connected. Others are rejected with a `sensor_blocked` event. |
| `open` | The gateway connects to any ESL-advertising sensor within range. |

The whitelist is stored in flash on the nRF5340 and survives reboots and
firmware updates. On first boot (empty flash) the gateway defaults to
`whitelist` mode with an empty list — no sensors connect until provisioned.

### Boot behavior

On every boot the nRF91 queries the gateway's whitelist status. If the
gateway is already provisioned (`PROVISIONED=1`), no action is needed.
If not provisioned (first boot or factory reset), the cloud can push
the sensor list via the commands below.

---

## `sensor_mgmt_status`

Query the current whitelist state.

```json
{"command": "sensor_mgmt_status"}
```

### Response (command acknowledgment)

```json
{
  "status": "sensor_mgmt_status_requested",
  "command_processed": "sensor_mgmt_status"
}
```

### Asynchronous event (published on data topic)

```json
{
  "type": "sensor_mgmt",
  "event": "STATUS,MODE=whitelist,COUNT=3,PROVISIONED=1"
}
```

---

## `sensor_mgmt_mode`

Set the filtering mode.

| Parameter | Type | Required | Values |
|-----------|------|----------|--------|
| `mode` | string | yes | `"whitelist"` or `"open"` |

```json
{"command": "sensor_mgmt_mode", "mode": "whitelist"}
```

### Response

```json
{
  "status": "ok",
  "mode": "whitelist",
  "command_processed": "sensor_mgmt_mode"
}
```

### Asynchronous event

```json
{
  "type": "sensor_mgmt",
  "event": "MODE=whitelist"
}
```

---

## `sensor_mgmt_add`

Add a sensor BLE address to the whitelist.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `addr` | string | yes | BLE address in `XX:XX:XX:XX:XX:XX` format |

```json
{"command": "sensor_mgmt_add", "addr": "AA:BB:CC:DD:EE:FF"}
```

### Response

```json
{
  "status": "ok",
  "addr": "AA:BB:CC:DD:EE:FF",
  "command_processed": "sensor_mgmt_add"
}
```

### Asynchronous event

```json
{
  "type": "sensor_mgmt",
  "event": "ADDED=AA:BB:CC:DD:EE:FF,COUNT=3"
}
```

> Adding an address that is already in the list returns success (idempotent).

---

## `sensor_mgmt_remove`

Remove a sensor BLE address from the whitelist.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `addr` | string | yes | BLE address in `XX:XX:XX:XX:XX:XX` format |

```json
{"command": "sensor_mgmt_remove", "addr": "AA:BB:CC:DD:EE:FF"}
```

### Response

```json
{
  "status": "ok",
  "addr": "AA:BB:CC:DD:EE:FF",
  "command_processed": "sensor_mgmt_remove"
}
```

### Asynchronous event

```json
{
  "type": "sensor_mgmt",
  "event": "REMOVED=AA:BB:CC:DD:EE:FF,COUNT=2"
}
```

> Removing involves only the whitelist. A currently-connected sensor is **not**
> automatically disconnected. Use `esl_command` with `acl disconnect <idx>` if needed.

---

## `sensor_mgmt_list`

List all sensors in the whitelist.

```json
{"command": "sensor_mgmt_list"}
```

### Response

```json
{
  "status": "sensor_mgmt_list_requested",
  "command_processed": "sensor_mgmt_list"
}
```

### Asynchronous events (one per entry, then LIST_DONE)

```json
{"type": "sensor_mgmt", "event": "ENTRY=0,AA:BB:CC:DD:EE:FF (random)"}
{"type": "sensor_mgmt", "event": "ENTRY=1,11:22:33:44:55:66 (random)"}
{"type": "sensor_mgmt", "event": "LIST_DONE,COUNT=2"}
```

---

## `sensor_mgmt_clear`

Remove all sensors from the whitelist.

```json
{"command": "sensor_mgmt_clear"}
```

### Response

```json
{
  "status": "sensor_mgmt_cleared",
  "command_processed": "sensor_mgmt_clear"
}
```

### Asynchronous event

```json
{
  "type": "sensor_mgmt",
  "event": "CLEARED"
}
```

---

## `sensor_mgmt_provision_done`

Signal that the initial sensor list provisioning is complete.

```json
{"command": "sensor_mgmt_provision_done"}
```

### Response

```json
{
  "status": "ok",
  "command_processed": "sensor_mgmt_provision_done"
}
```

### Asynchronous event

```json
{
  "type": "sensor_mgmt",
  "event": "PROVISIONED,COUNT=3,MODE=whitelist"
}
```

---

## `sensor_mgmt_set_key`

Import a BLE bond key for a sensor (for transferring sensors between gateways).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `addr` | string | yes | BLE address `XX:XX:XX:XX:XX:XX` |
| `key` | string | yes | Hex-encoded bond key blob |

```json
{"command": "sensor_mgmt_set_key", "addr": "AA:BB:CC:DD:EE:FF", "key": "0102030405060708090a0b0c0d0e0f10"}
```

### Response

```json
{
  "status": "ok",
  "addr": "AA:BB:CC:DD:EE:FF",
  "command_processed": "sensor_mgmt_set_key"
}
```

### Asynchronous event

```json
{
  "type": "sensor_mgmt",
  "event": "KEY_SET=AA:BB:CC:DD:EE:FF"
}
```

---

## `sensor_mgmt_get_key`

Export a BLE bond key for a sensor.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `addr` | string | yes | BLE address `XX:XX:XX:XX:XX:XX` |

```json
{"command": "sensor_mgmt_get_key", "addr": "AA:BB:CC:DD:EE:FF"}
```

### Response

```json
{
  "status": "ok",
  "addr": "AA:BB:CC:DD:EE:FF",
  "command_processed": "sensor_mgmt_get_key"
}
```

The key data arrives asynchronously via the existing `bt_key_export` notification.

---

## Unsolicited Events

These events are published on the data topic whenever the gateway reports
whitelist activity, even without a preceding MQTT command:

| Event type | Example `event` field | Meaning |
|---|---|---|
| `sensor_mgmt` | `ADDED=AA:BB:CC:DD:EE:FF,COUNT=3` | Sensor added |
| `sensor_mgmt` | `REMOVED=AA:BB:CC:DD:EE:FF,COUNT=2` | Sensor removed |
| `sensor_mgmt` | `CLEARED` | All sensors removed |
| `sensor_mgmt` | `MODE=whitelist` | Mode changed |
| `sensor_mgmt` | `PROVISIONED,COUNT=3,MODE=whitelist` | Provisioning complete |
| `sensor_mgmt` | `STATUS,MODE=whitelist,COUNT=3,PROVISIONED=1` | Status response |
| `sensor_mgmt` | `ENTRY=0,AA:BB:CC:DD:EE:FF (random)` | List entry |
| `sensor_mgmt` | `LIST_DONE,COUNT=2` | End of list dump |
| `sensor_mgmt` | `KEY_SET=AA:BB:CC:DD:EE:FF` | Bond key imported |
| `sensor_blocked` | `AA:BB:CC:DD:EE:FF (random),reason=not_in_whitelist` | Sensor rejected by whitelist |

---

## Common Workflows

### First boot — provision 3 sensors from cloud

```bash
# 1. Check status (expect PROVISIONED=0)
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"sensor_mgmt_status"}'

# 2. Set mode
mosquitto_pub ... -m '{"command":"sensor_mgmt_mode","mode":"whitelist"}'

# 3. Add sensors
mosquitto_pub ... -m '{"command":"sensor_mgmt_add","addr":"AA:BB:CC:DD:EE:FF"}'
mosquitto_pub ... -m '{"command":"sensor_mgmt_add","addr":"11:22:33:44:55:66"}'
mosquitto_pub ... -m '{"command":"sensor_mgmt_add","addr":"DE:AD:BE:EF:CA:FE"}'

# 4. Mark provisioning complete
mosquitto_pub ... -m '{"command":"sensor_mgmt_provision_done"}'
```

After provisioning, the list is saved to flash. On subsequent reboots the
gateway restores it automatically — no re-provisioning needed.

### Add a new sensor at runtime

```json
{"command": "sensor_mgmt_add", "addr": "99:88:77:66:55:44"}
```

The gateway immediately begins accepting this sensor in BLE scans.

### Transfer a sensor between gateways

**On source gateway (A):**
```json
{"command": "sensor_mgmt_get_key", "addr": "AA:BB:CC:DD:EE:FF"}
```
Save the key blob from the response.

```json
{"command": "sensor_mgmt_remove", "addr": "AA:BB:CC:DD:EE:FF"}
```

**On destination gateway (B):**
```json
{"command": "sensor_mgmt_add", "addr": "AA:BB:CC:DD:EE:FF"}
{"command": "sensor_mgmt_set_key", "addr": "AA:BB:CC:DD:EE:FF", "key": "<hex_key_from_A>"}
{"command": "sensor_mgmt_provision_done"}
```

### Switch to open mode

```json
{"command": "sensor_mgmt_mode", "mode": "open"}
```

---

## Error Handling

| Error | Cause | Action |
|-------|-------|--------|
| SPI send failure (error_code in response) | nRF5340 not ready or SPI bus error | Retry after a few seconds |
| `ENOENT` in gateway response | `remove` for nonexistent address | Address typo or already removed |
| `EALREADY` in gateway response | `add` for existing address | Harmless — entry already exists |
| `ENOMEM` in gateway response | Whitelist full (>32 entries) | Remove unused sensors or increase `CONFIG_ESL_SENSOR_WL_MAX_ENTRIES` |
