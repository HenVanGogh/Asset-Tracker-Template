# MQTT ESL / UART Sensor Commands

Commands for managing ESL (Electronic Shelf Label) BLE tags, the nRF5340 Access Point (AP),
and UART sensor configuration.

Requires `CONFIG_APP_UART_SENSOR=y` in the build (except `uart_debug_echo` and `uart_command`,
which are available regardless — they simply return `"uart_not_available"` if the module is off).

→ Back to [MQTT_COMMANDS.md](MQTT_COMMANDS.md)

---

## Architecture

```
[MQTT broker]
     │
     │ (subscribe topic)
     ▼
[nRF9151 — custom_mqtt.c]
     │
     │ UART (UART0 / configured pins)
     ▼
[nRF5340 AP — ESL Access Point firmware]
     │
     │ BLE
     ▼
[ESL Tags (Electronic Shelf Labels)]
```

MQTT commands destined for the ESL tags are forwarded by the nRF9151 to the nRF5340 AP
via UART as text commands. The AP handles the BLE communication with the tags.

All raw commands forwarded to the AP are automatically prefixed with `esl_c ` by
`uart_sensor_esl_command()` before transmission.

---

## Silent passthrough (`uart_passthrough` type)

Use this format to forward a raw command to the UART **without** receiving an MQTT response.
Suitable for high-frequency or fire-and-forget commands when acknowledgment overhead is
not needed.

```json
{
  "type": "uart_passthrough",
  "command": "reset_ap"
}
```

- No MQTT response is published
- The command value is passed directly to `uart_sensor_esl_command()`
- The `esl_c ` prefix is added automatically

---

## `uart_command`

Send a raw UART command to the ESL AP with MQTT acknowledgment.
The command string is forwarded via `uart_sensor_esl_command()` (adds `esl_c ` prefix).

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `args` | string | Yes | Raw command string to forward to the AP |

```json
{"command": "uart_command", "args": "acl list"}
```

```json
{"command": "uart_command", "args": "reset_ap"}
```

### Response

```json
{
  "status": "esl_command_sent",
  "command_processed": "uart_command"
}
```

If the UART sensor module is disabled in this build:
```json
{"status": "uart_not_available"}
```

---

## `uart_debug_echo`

Toggle UART debug echo mode. When **enabled**, every unrecognized line received from the UART
is published to the MQTT data topic as:

```json
{"type": "uart_debug", "line": "<raw uart line>"}
```

This is **disabled by default** — do not leave it enabled in production as it will flood the
broker with raw UART output.

### Parameters

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | No | `"start"` | `"start"` to enable, `"stop"` to disable |

```json
{"command": "uart_debug_echo", "action": "start"}
```

```json
{"command": "uart_debug_echo", "action": "stop"}
```

### Response

```json
{
  "status": "uart_debug_echo_enabled",
  "uart_debug_echo_active": true,
  "command_processed": "uart_debug_echo"
}
```

---

## ESL Scanning

### `esl_scan`

Start or stop BLE scanning for ESL tags.

| Field | Type | Required | Description |
|---|---|---|---|
| `action` | string | No | `"stop"` to stop; omit or any other value to start |

```json
{"command": "esl_scan"}
```

```json
{"command": "esl_scan", "action": "stop"}
```

**Response:**
```json
{"status": "esl_scan_started", "command_processed": "esl_scan"}
```
```json
{"status": "esl_scan_stopped", "command_processed": "esl_scan"}
```

---

## ESL Polling

### `esl_poll`

Start or stop the periodic polling loop that keeps ESL tags awake and collects sensor data.

| Field | Type | Required | Description |
|---|---|---|---|
| `action` | string | No | `"stop"` to stop; omit to start |

```json
{"command": "esl_poll"}
```

```json
{"command": "esl_poll", "action": "stop"}
```

**Response:**
```json
{"status": "esl_poll_started", "command_processed": "esl_poll"}
```
```json
{"status": "esl_poll_stopped", "command_processed": "esl_poll"}
```

---

## ESL status and tag information

### `esl_status`

Return a quick status summary: tag count and device uptime. Also sends `"acl list"` to the AP
to refresh tag data (result arrives as a subsequent UART-decoded MQTT message).

**No parameters required.**

```json
{"command": "esl_status"}
```

**Response:**
```json
{
  "status": "ok",
  "tag_count": 3,
  "uptime_ms": 7654321,
  "command_processed": "esl_status"
}
```

---

### `esl_list_tags`

Return the current tag count and ask the AP for its Access Control List. The AP's reply arrives
as a separate MQTT publication when the UART response is received.

**No parameters required.**

```json
{"command": "esl_list_tags"}
```

**Response:**
```json
{
  "status": "ok",
  "tag_count": 3,
  "command_processed": "esl_list_tags"
}
```

---

### `esl_get_tags`

Return detailed information for all known ESL tags, including battery level, uptime, temperature,
flags, MAC address, and BLE connection status. No UART command is sent to the AP — this data is
from the cached state maintained by the nRF9151.

**No parameters required.**

```json
{"command": "esl_get_tags"}
```

**Response:**
```json
{
  "status": "ok",
  "tag_count": 2,
  "tags": [
    {
      "esl_id": "ESL_0x0001",
      "mac": "AA:BB:CC:DD:EE:01",
      "battery_mv": 2980,
      "uptime_s": 3600,
      "temperature": 22,
      "flags": 0,
      "connected": true
    },
    {
      "esl_id": "ESL_0x0002",
      "mac": "AA:BB:CC:DD:EE:02",
      "battery_mv": 2850,
      "uptime_s": 7200,
      "temperature": 21,
      "flags": 0,
      "connected": false
    }
  ],
  "command_processed": "esl_get_tags"
}
```

| Field | Type | Description |
|---|---|---|
| `esl_id` | string | ESL address in hex, e.g. `"ESL_0x0001"` |
| `mac` | string | BLE MAC address |
| `battery_mv` | number | Battery voltage in millivolts |
| `uptime_s` | number | Tag uptime in seconds |
| `temperature` | number | Temperature reading (°C) |
| `flags` | number | Raw ESL flag bits |
| `connected` | boolean | Whether the tag is currently connected via BLE |

---

### `esl_set_expected_tags`

Set the number of ESL tags the system expects to see. Used for alerting logic (e.g., if fewer
tags are seen than expected, an alert can be triggered). Saved to flash.

| Field | Type | Required | Description |
|---|---|---|---|
| `count` | integer | Yes | Expected number of tags |

```json
{"command": "esl_set_expected_tags", "count": 3}
```

**Response:**
```json
{
  "status": "ok",
  "expected_tags": 3,
  "command_processed": "esl_set_expected_tags"
}
```

---

### `esl_get_name`

Request the human-readable name from one or all ESL tags via the AP. The name response arrives
asynchronously as a `sensor_name` MQTT publication (not included in this command's response).

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `esl_id` | integer | No | 65535 (all) | Target ESL address (decimal). Omit or `65535` to query all known tags. |

```json
{"command": "esl_get_name"}
```

```json
{"command": "esl_get_name", "esl_id": 1}
```

**Response:**
```json
{
  "status": "get_name_sent",
  "target": "all",
  "command_processed": "esl_get_name"
}
```

```json
{
  "status": "get_name_sent",
  "esl_id": 1,
  "command_processed": "esl_get_name"
}
```

---

## NUS (Nordic UART Service) per-tag commands

These commands communicate with individual tags via the BLE NUS (Nordic UART Service) channel.
The target tag is identified by its ESL address (decimal integer).

### `esl_nus_status`

Request NUS status from a specific ESL tag.

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | integer | Yes | ESL address (decimal) |

```json
{"command": "esl_nus_status", "id": 1}
```

**Response:**
```json
{
  "status": "nus_status_requested",
  "esl_id": 1,
  "command_processed": "esl_nus_status"
}
```

---

### `esl_nus_sensors`

Request current sensor data from a specific ESL tag via NUS.

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | integer | Yes | ESL address (decimal) |

```json
{"command": "esl_nus_sensors", "id": 1}
```

**Response:**
```json
{
  "status": "nus_sensors_requested",
  "esl_id": 1,
  "command_processed": "esl_nus_sensors"
}
```

---

### `esl_nus_reset`

Reset the NUS BLE connection to a specific tag. Useful to recover from a stuck connection.

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | integer | Yes | ESL address (decimal) |

```json
{"command": "esl_nus_reset", "id": 2}
```

**Response:**
```json
{
  "status": "nus_reset_sent",
  "esl_id": 2,
  "command_processed": "esl_nus_reset"
}
```

---

### `esl_nus_led`

Toggle the LED on a specific tag via NUS.

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | integer | Yes | ESL address (decimal) |

```json
{"command": "esl_nus_led", "id": 1}
```

**Response:**
```json
{
  "status": "nus_led_sent",
  "esl_id": 1,
  "command_processed": "esl_nus_led"
}
```

---

## Raw ESL commands

### `esl_command`

Send any raw command string to the AP via UART (with `esl_c ` prefix added automatically).
Use this for commands not exposed by specific MQTT commands.

| Field | Type | Required | Description |
|---|---|---|---|
| `args` | string | Yes | Raw ESL command string |

```json
{"command": "esl_command", "args": "acl list"}
```

```json
{"command": "esl_command", "args": "nus reset 0"}
```

**Response:**
```json
{
  "status": "esl_command_sent",
  "command_processed": "esl_command"
}
```

---

### `esl_raw`

Simpler syntax variant of `esl_command`. Accepts the command in either a `"cmd"` or `"args"` field.

| Field | Type | Required | Description |
|---|---|---|---|
| `cmd` | string | Yes (or `args`) | Raw ESL command string |
| `args` | string | Yes (or `cmd`) | Alternative field name |

```json
{"command": "esl_raw", "cmd": "reset_ap"}
```

```json
{"command": "esl_raw", "args": "acl list"}
```

**Response:**
```json
{
  "status": "esl_raw_sent",
  "cmd_sent": "reset_ap",
  "command_processed": "esl_raw"
}
```

If neither `"cmd"` nor `"args"` is present:
```json
{
  "status": "missing_cmd",
  "hint": "{\"command\":\"esl_raw\",\"cmd\":\"reset_ap\"}",
  "command_processed": "esl_raw"
}
```

---

## AP time synchronization

### `ap_set_time`

Push the current LTE UTC time to the nRF5340 AP. Normally the time is sent automatically
on boot after the LTE clock is synchronized. Use this command to manually re-sync after an
AP reboot or if the AP drifted.

**No parameters required.**

```json
{"command": "ap_set_time"}
```

**Response — success:**
```json
{
  "status": "ap_set_time_sent",
  "command_processed": "ap_set_time"
}
```

**Response — LTE time not yet available:**
```json
{
  "status": "error",
  "hint": "LTE time may not be available yet",
  "error_code": -116,
  "command_processed": "ap_set_time"
}
```

---

## Sensor polling configuration

All sensor config values are **persisted to flash** and survive reboot.

### `sensor_get_config`

Return the current sensor module configuration.

**No parameters required.**

```json
{"command": "sensor_get_config"}
```

**Response:**
```json
{
  "status": "ok",
  "poll_interval_s": 600,
  "scan_retry_interval_s": 60,
  "expected_tags": 3,
  "nus_max_failures": 3,
  "command_processed": "sensor_get_config"
}
```

| Field | Description |
|---|---|
| `poll_interval_s` | How often (seconds) the system actively polls all ESL tags |
| `scan_retry_interval_s` | How long (seconds) to wait before retrying a BLE scan after a failed scan |
| `expected_tags` | Number of ESL tags that should be present (used for alerting) |
| `nus_max_failures` | Maximum consecutive NUS failures before marking a tag as disconnected |

---

### `sensor_set_poll_interval`

Set how often the system polls ESL tags. Valid range: 10–86400 seconds.

| Field | Type | Required | Description |
|---|---|---|---|
| `interval_s` | integer | Yes | Poll interval in seconds (10–86400) |

```json
{"command": "sensor_set_poll_interval", "interval_s": 300}
```

**Response:**
```json
{
  "status": "ok",
  "poll_interval_s": 300,
  "command_processed": "sensor_set_poll_interval"
}
```

---

### `sensor_set_scan_retry_interval`

Set the BLE scan retry interval — how long to wait after a failed BLE scan before trying again.

| Field | Type | Required | Description |
|---|---|---|---|
| `interval_s` | integer | Yes | Retry interval in seconds |

```json
{"command": "sensor_set_scan_retry_interval", "interval_s": 30}
```

**Response:**
```json
{
  "status": "ok",
  "scan_retry_interval_s": 30,
  "command_processed": "sensor_set_scan_retry_interval"
}
```

---

### `sensor_set_nus_failures`

Set the maximum number of consecutive NUS failures before a tag is considered disconnected / lost.
Valid range: 1–20.

| Field | Type | Required | Description |
|---|---|---|---|
| `count` | integer | Yes | Max failures before tag considered offline (1–20) |

```json
{"command": "sensor_set_nus_failures", "count": 5}
```

**Response:**
```json
{
  "status": "ok",
  "nus_max_failures": 5,
  "command_processed": "sensor_set_nus_failures"
}
```

---

## Common AP commands reference

The following strings can be sent via `esl_command`, `esl_raw`, or `uart_command`
(the `esl_c ` prefix is added automatically):

| AP command string | Description |
|---|---|
| `acl list` | List all known tags in the AP access control list |
| `reset_ap` | Reset the nRF5340 AP (soft reset via command) |
| `nus reset <id>` | Reset NUS connection to tag `<id>` |
| `nus led <id>` | Toggle LED on tag `<id>` |
| `nus status <id>` | Get NUS status for tag `<id>` |
| `nus sensors <id>` | Get sensor readings for tag `<id>` |

These are examples — the full AP command set is documented in the AP firmware.
