# MQTT Device Commands

Commands for querying device state, triggering location fixes, controlling power mode, and rebooting.

→ Back to [MQTT_COMMANDS.md](MQTT_COMMANDS.md)

---

## `get_status`

Returns the current operational state of the device.

**No parameters required.**

```json
{"command": "get_status"}
```

### Response

```json
{
  "device_id": "gateway_A1F3",
  "timestamp": 1234567,
  "received_message": "{\"command\":\"get_status\"}",
  "response_sequence": 5,
  "status": "online",
  "uptime_ms": 1234567,
  "mqtt_state": 2,
  "network_connected": true,
  "command_processed": "get_status"
}
```

| Field | Type | Description |
|---|---|---|
| `status` | string | Always `"online"` if the device can respond |
| `uptime_ms` | number | Milliseconds since last boot |
| `mqtt_state` | number | Internal MQTT state: `0`=IDLE, `1`=CONNECTING, `2`=CONNECTED, `3`=DISCONNECTING, `4`=ERROR |
| `network_connected` | boolean | Whether the LTE network link is established |

---

## `get_location`

Triggers a location fix using the configured location method (LTE cell-ID, GNSS, Wi-Fi, or combined).

Requires `CONFIG_APP_LOCATION=y`.

**No parameters required.**

```json
{"command": "get_location"}
```

### Response (immediate acknowledgment)

```json
{
  "status": "location_requested",
  "command_processed": "get_location"
}
```

The location result is **not** included in this response — it is published separately to the data topic
once the location fix completes (may take several seconds to minutes depending on signal conditions
and the configured location method).

If the location module is not enabled in the build:

```json
{
  "status": "location_not_available",
  "command_processed": "get_location"
}
```

---

## `set_power_mode`

Controls the LTE power profile and MQTT heartbeat/reporting interval. The mode is **persisted to flash**
and restored after reboot.

| Mode | LTE behavior | Heartbeat interval | Trigger interval |
|---|---|---|---|
| `"normal"` | PSM/eDRX enabled (low power, longer latency) | `MQTT_HEARTBEAT_INTERVAL_SEC` | `CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS` |
| `"high"` | PSM/eDRX disabled, always connected | `MQTT_HIGH_POWER_HEARTBEAT_SEC` | `MQTT_HIGH_POWER_TRIGGER_INTERVAL_SEC` |

> **Note**: At firmware boot the device always starts in **high-power mode** (PSM/eDRX not requested),
> then switches to the persisted mode. This ensures MQTT connectivity during startup.

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `mode` | string | Yes | `"high"` or `"normal"` |

```json
{"command": "set_power_mode", "mode": "high"}
```

```json
{"command": "set_power_mode", "mode": "normal"}
```

### Response

```json
{
  "status": "ok",
  "power_mode": "high",
  "command_processed": "set_power_mode"
}
```

On invalid input:
```json
{
  "status": "invalid_mode",
  "hint": "{\"command\":\"set_power_mode\",\"mode\":\"high\"}",
  "command_processed": "set_power_mode"
}
```

---

## `reboot`

Reboots the device after a configurable delay. The response is published **before** the reboot so the
caller receives confirmation. After the delay, log buffers are flushed (`LOG_PANIC()`) and the device
performs a cold reboot.

### Parameters

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `delay_s` | integer | No | `3` | Seconds to wait before rebooting. Clamped to 1–60. |

```json
{"command": "reboot"}
```

```json
{"command": "reboot", "delay_s": 10}
```

### Response (published before reboot fires)

```json
{
  "status": "rebooting",
  "delay_s": 3,
  "command_processed": "reboot"
}
```

> **Important**: The MQTT connection will drop after `delay_s` seconds. If you need to change broker
> config before rebooting, issue `mqtt_set_broker` / `mqtt_set_auth` first, then `reboot`.

### Common use cases

1. **Apply MQTT config changes** — After issuing `mqtt_set_broker`, `mqtt_set_auth`, etc., call
   `mqtt_restart` to reconnect without rebooting. Use `reboot` only when a full system restart is needed.

2. **Recovery** — If the device is in an unexpected state, a remote reboot restores clean operation.

3. **FOTA fallback** — If `fota_start` completed download but the device did not reboot automatically
   (e.g., network disconnect timed out), send `{"command":"reboot"}` to perform the reboot manually.
   MCUboot will apply the staged image on next boot.

---

## Inactivity watchdog reboot

The device has a built-in inactivity watchdog. If no MQTT traffic is observed for
`MQTT_INACTIVITY_WATCHDOG_SEC` seconds, the device automatically reboots.

This is controlled via `Kconfig.custom_mqtt`:

```
CONFIG_APP_MQTT_INACTIVITY_WATCHDOG_SEC=3600
```

The watchdog flag (`mqtt_inactive_reboot_flag`) can be seen in device logs before the reboot.
You can also tune the threshold via the `mqtt_inactive` settings key (internal, not exposed as a
direct MQTT command).
