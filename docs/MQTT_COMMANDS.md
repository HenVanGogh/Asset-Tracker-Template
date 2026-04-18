# MQTT Command Reference — Thingy:91X Asset Tracker Gateway

Complete reference for all MQTT commands accepted by the gateway firmware.

---

## Table of Contents

- [Connection Details](#connection-details)
- [Message Format](#message-format)
- [Response Envelope](#response-envelope)
- [Command Quick Reference](#command-quick-reference)
- [Detailed Documentation](#detailed-documentation)

---

## Connection Details

| Parameter | Default (Kconfig) | Runtime value (this deployment) |
|---|---|---|
| Broker hostname | `49fc73a33de54e32966eac3525e9106c.s1.eu.hivemq.cloud` | `217.154.155.83` |
| Broker port | `8883` (TLS) | `1883` (plaintext) |
| Username | `hivemq.webclient...` | `mqttuser` |
| Password | `I.a$,...` | `mqttuser` |
| Client ID | `gateway_0000` | `gateway_XXXX` (auto-generated) |
| **Publish topic** | `gateway/gateway_0000/data` | `gateway/gateway_XXXX/data` |
| **Subscribe topic** | `gateway/gateway_0000/command` | `gateway/gateway_XXXX/command` |

> All connection parameters can be changed at runtime via MQTT commands and are persisted to flash.
> See [MQTT_COMMANDS_MQTT_CONFIG.md](MQTT_COMMANDS_MQTT_CONFIG.md) for details.
>
> The device name (`gateway_XXXX`) is a random 4-character hex suffix generated on first boot
> and persisted to flash. Use `mqtt_get_config` to discover the device's actual name and topics.
> The name survives firmware updates and reflashing as long as the settings partition is not erased.

### Quick test with mosquitto_pub

```bash
# Send a command (replace broker/topic as needed — use your device's actual name)
# Find the device name via: mosquitto_sub ... -t "gateway/+/data"
mosquitto_pub -h 217.154.155.83 -p 1883 \
  -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"get_status","id":1}'

# Subscribe to responses (use wildcard to discover all gateways)
mosquitto_sub -h 217.154.155.83 -p 1883 \
  -u mqttuser -P mqttuser \
  -t "gateway/+/data"
```

---

## Message Format

All commands are sent as JSON objects to the **subscribe topic** (device receives these).

### Standard command format

```json
{
  "command": "<command_name>",
  "id": 42,
  "<param1>": <value1>,
  "<param2>": <value2>
}
```

The `"command"` key is the primary dispatcher. An alias `"cmd"` is also accepted as the key name.

The `"id"` field is **optional**. When included, the device echoes it back in the response,
allowing the caller to correlate requests with their responses. The value can be a number or a
string — both are echoed as-is.

### Silent passthrough (no command acknowledgment)

A special `"type": "uart_passthrough"` format routes a command string directly to the UART
without generating an MQTT response. Use this for ESL commands when acknowledgment overhead
is not needed.

```json
{
  "type": "uart_passthrough",
  "command": "<raw_esl_command>"
}
```

---

## Response Envelope

Every command response is published to the **publish topic** as a JSON object with the
following standard fields:

```json
{
  "device_id": "gateway_A1F3",
  "timestamp": 123456,
  "received_message": "{\"command\":\"get_status\",\"id\":42}",
  "response_sequence": 42,
  "id": 42,

  "status": "<result>",
  "command_processed": "<command_name>",

  /* ... command-specific fields ... */
}
```

| Field | Type | Description |
|---|---|---|
| `device_id` | string | MQTT client ID of the responding device |
| `timestamp` | number | Milliseconds since device boot (`k_uptime_get()`) |
| `received_message` | string | Original message payload (for correlation) |
| `response_sequence` | number | Monotonically increasing publish counter |
| `id` | number/string | Echoed from request — present only if the request included an `id` field |
| `status` | string | Result of the command — see per-command docs |
| `command_processed` | string | Echo of the command name |

---

## Command Quick Reference

### Device / system

| Command | Description | Required params | Optional params |
|---|---|---|---|
| [`get_status`](#get_status) | Return device uptime and connection state | — | — |
| [`get_location`](#get_location) | Trigger LTE/GNSS location fix | — | — |
| [`set_power_mode`](#set_power_mode) | Switch between normal and high-frequency mode | `mode` | — |
| [`reboot`](#reboot) | Reboot the device | — | `delay_s` |

### FOTA firmware update

| Command | Description | Required params | Optional params |
|---|---|---|---|
| [`fota_start`](#fota_start) | Download and stage a new firmware image (nRF9151) | `url` | `sec_tag` |
| [`fota_cancel`](#fota_cancel) | Cancel an in-progress download | — | — |
| [`fota_status`](#fota_status) | Query download progress | — | — |
| [`image_info`](#image_info) | Show running version, confirmation state, swap type | — | — |
| [`image_confirm`](#image_confirm) | Permanently confirm the running image (no revert) | — | — |

### Companion-chip firmware download *(requires `CONFIG_APP_EXT_DFU=y`)*

| Command | Description | Required params | Optional params |
|---|---|---|---|
| [`ext_fota_nrf5340`](#ext_fota_nrf5340) | Download nRF5340 firmware to external flash slot | `url` | `sec_tag` |
| [`ext_fota_nrf52840`](#ext_fota_nrf52840) | Download nRF52840 firmware to external flash slot | `url` | `sec_tag` |
| [`ext_fota_status`](#ext_fota_status) | Query download status for both companion slots | — | — |
| [`ext_fota_cancel`](#ext_fota_cancel) | Cancel an active companion-chip download | — | — |
| [`ext_fota_erase`](#ext_fota_erase) | Erase a companion-chip flash slot | `target` | — |

### SPI DFU — push firmware to nRF5340 over SPI *(requires `CONFIG_APP_SPI_DFU=y`)*

| Command | Description | Required params |
|---|---|---|
| [`spi_dfu_nrf5340`](MQTT_COMMANDS_FOTA.md#spi_dfu_nrf5340) | Start nRF5340 firmware update over SPI | — |
| [`spi_dfu_status`](MQTT_COMMANDS_FOTA.md#spi_dfu_status) | Query SPI DFU progress | — |
| [`spi_dfu_cancel`](MQTT_COMMANDS_FOTA.md#spi_dfu_cancel) | Cancel an active SPI DFU | — |

### MQTT broker runtime configuration

| Command | Description | Required params |
|---|---|---|
| [`mqtt_get_config`](#mqtt_get_config) | Return current broker config and firmware version | — |
| [`mqtt_set_broker`](#mqtt_set_broker) | Change broker hostname and/or port | `host` or `port` |
| [`mqtt_set_auth`](#mqtt_set_auth) | Change username and/or password | `username` or `password` |
| [`mqtt_set_client_id`](#mqtt_set_client_id) | Change MQTT client ID | `id` |
| [`mqtt_set_topics`](#mqtt_set_topics) | Change publish/subscribe topics | `pub_topic` or `sub_topic` |
| [`mqtt_set_tls`](#mqtt_set_tls) | Enable/disable TLS and set security tag | `enabled` or `sec_tag` |
| [`mqtt_restart`](#mqtt_restart) | Reconnect to broker (applies config changes) | — |

### ESL BLE tag management *(requires `CONFIG_APP_UART_SENSOR=y`)*

| Command | Description |
|---|---|
| [`esl_scan`](#esl_scan) | Start or stop BLE scanning for ESL tags |
| [`esl_poll`](#esl_poll) | Start or stop periodic ESL tag polling |
| [`esl_status`](#esl_status) | Return AP status and tag count |
| [`esl_list_tags`](#esl_list_tags) | Return tag count and trigger AP ACL list dump |
| [`esl_get_tags`](#esl_get_tags) | Return full tag info array (battery, temp, flags…) |
| [`esl_set_expected_tags`](#esl_set_expected_tags) | Set expected number of tags (persisted) |
| [`esl_get_name`](#esl_get_name) | Request human-readable name from one or all tags |
| [`esl_nus_status`](#esl_nus_status) | Request NUS status from a specific tag |
| [`esl_nus_sensors`](#esl_nus_sensors) | Request sensor data from a specific tag via NUS |
| [`esl_nus_reset`](#esl_nus_reset) | Reset NUS connection to a specific tag |
| [`esl_nus_led`](#esl_nus_led) | Toggle LED on a specific tag via NUS |
| [`esl_command`](#esl_command) | Send raw ESL command string (with `esl_c` prefix) |
| [`esl_raw`](#esl_raw) | Send raw ESL command string (simpler syntax) |
| [`ap_set_time`](#ap_set_time) | Push current LTE UTC time to the nRF5340 AP |

### UART / sensor configuration *(requires `CONFIG_APP_UART_SENSOR=y`)*

| Command | Description |
|---|---|
| [`uart_command`](#uart_command) | Send raw UART command (acknowledged) |
| [`uart_debug_echo`](#uart_debug_echo) | Enable/disable publishing unknown UART lines to MQTT |
| [`sensor_get_config`](#sensor_get_config) | Return current sensor polling configuration |
| [`sensor_set_poll_interval`](#sensor_set_poll_interval) | Set sensor poll interval in seconds (persisted) |
| [`sensor_set_scan_retry_interval`](#sensor_set_scan_retry_interval) | Set BLE scan retry interval (persisted) |
| [`sensor_set_nus_failures`](#sensor_set_nus_failures) | Set max NUS failure threshold (persisted) |

### Sensor whitelist management *(requires `CONFIG_APP_UART_SENSOR=y`)*

| Command | Description | Required params |
|---|---|---|
| [`sensor_mgmt_status`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_status) | Query whitelist state | — |
| [`sensor_mgmt_mode`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_mode) | Set filtering mode | `mode` |
| [`sensor_mgmt_add`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_add) | Add sensor to whitelist | `addr` |
| [`sensor_mgmt_remove`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_remove) | Remove sensor from whitelist | `addr` |
| [`sensor_mgmt_list`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_list) | List all whitelisted sensors | — |
| [`sensor_mgmt_clear`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_clear) | Clear entire whitelist | — |
| [`sensor_mgmt_provision_done`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_provision_done) | Mark provisioning complete | — |
| [`sensor_mgmt_set_key`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_set_key) | Import bond key | `addr`, `key` |
| [`sensor_mgmt_get_key`](MQTT_COMMANDS_SENSOR_MGMT.md#sensor_mgmt_get_key) | Export bond key | `addr` |

---

## Detailed Documentation

Individual command groups are documented in dedicated files:

| File | Contents |
|---|---|
| [MQTT_COMMANDS_DEVICE.md](MQTT_COMMANDS_DEVICE.md) | `get_status`, `get_location`, `set_power_mode`, `reboot` |
| [MQTT_COMMANDS_FOTA.md](MQTT_COMMANDS_FOTA.md) | `fota_start`, `fota_cancel`, `fota_status`, `image_info`, `image_confirm`, `ext_fota_*`, `spi_dfu_*` |
| [MQTT_COMMANDS_MQTT_CONFIG.md](MQTT_COMMANDS_MQTT_CONFIG.md) | All `mqtt_*` commands — runtime broker reconfiguration |
| [MQTT_COMMANDS_ESL.md](MQTT_COMMANDS_ESL.md) | All `esl_*`, `sensor_*`, `uart_*`, `ap_set_time` commands |
| [MQTT_COMMANDS_SENSOR_MGMT.md](MQTT_COMMANDS_SENSOR_MGMT.md) | Sensor whitelist management — `sensor_mgmt_*` commands |
