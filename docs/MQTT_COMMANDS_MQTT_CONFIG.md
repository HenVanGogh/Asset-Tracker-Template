# MQTT Broker Runtime Configuration Commands

Commands for changing MQTT connection parameters at runtime without reflashing.
All changes are persisted to flash (`settings` subsystem) and survive reboot.

→ Back to [MQTT_COMMANDS.md](MQTT_COMMANDS.md)

---

## Overview

The firmware has two layers of MQTT configuration:

1. **Compile-time defaults** — set in `Kconfig.custom_mqtt` / `prj.conf`
2. **Runtime overrides** — stored in flash under the `app/mqtt/` settings namespace

On boot, runtime overrides (if present) take precedence over compile-time defaults.
This means you can change the broker without reflashing.

### Workflow for changing broker

```
mqtt_set_broker  (save new host/port)
mqtt_set_auth    (save new credentials, if needed)
mqtt_restart     (disconnect + reconnect with new config)
```

Changes take effect **only after `mqtt_restart`** (or a full device reboot).

---

## `mqtt_get_config`

Returns the currently active MQTT configuration and firmware version. Useful for verifying
what settings are in effect.

**No parameters required.**

```json
{"command": "mqtt_get_config"}
```

### Response

```json
{
  "status": "ok",
  "host": "217.154.155.83",
  "port": 1883,
  "username": "mqttuser",
  "client_id": "gateway_A1F3",
  "pub_topic": "gateway/gateway_A1F3/data",
  "sub_topic": "gateway/gateway_A1F3/command",
  "tls_enabled": false,
  "sec_tag": 0,
  "power_mode": "high",
  "version": "2.0.0",
  "command_processed": "mqtt_get_config"
}
```

> **Note**: The `password` field is intentionally omitted from the response for security.

| Field | Description |
|---|---|
| `host` | Current broker hostname or IP |
| `port` | Current broker port |
| `username` | MQTT username |
| `client_id` | MQTT client identifier |
| `pub_topic` | Topic the device publishes data to |
| `sub_topic` | Topic the device subscribes for commands |
| `tls_enabled` | Whether MQTT TLS is active |
| `sec_tag` | Modem credential tag used for TLS |
| `power_mode` | `"high"` or `"normal"` (see [MQTT_COMMANDS_DEVICE.md](MQTT_COMMANDS_DEVICE.md)) |
| `version` | Firmware version string from `app/VERSION` |

---

## `mqtt_set_broker`

Change the broker hostname and/or port number. At least one of `host` or `port` must be provided.
Saved to flash — persists across reboots.

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `host` | string | One of `host`/`port` required | Broker hostname or IP address |
| `port` | integer | One of `host`/`port` required | Broker TCP port |

```json
{"command": "mqtt_set_broker", "host": "217.154.155.83", "port": 1883}
```

```json
{"command": "mqtt_set_broker", "host": "mqtt.example.com"}
```

```json
{"command": "mqtt_set_broker", "port": 8883}
```

### Response — saved

```json
{
  "status": "saved",
  "host": "217.154.155.83",
  "port": 1883,
  "note": "Send mqtt_restart to reconnect with new broker",
  "command_processed": "mqtt_set_broker"
}
```

### Response — nothing changed

```json
{
  "status": "no_changes",
  "hint": "{\"command\":\"mqtt_set_broker\",\"host\":\"192.168.1.1\",\"port\":1883}",
  "command_processed": "mqtt_set_broker"
}
```

---

## `mqtt_set_auth`

Change the MQTT username and/or password. Saved to flash.

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `username` | string | One of `username`/`password` required | MQTT username |
| `password` | string | One of `username`/`password` required | MQTT password |

```json
{"command": "mqtt_set_auth", "username": "newuser", "password": "newpass"}
```

```json
{"command": "mqtt_set_auth", "password": "newpass_only"}
```

### Response — saved

```json
{
  "status": "saved",
  "note": "Send mqtt_restart to reconnect with new credentials",
  "command_processed": "mqtt_set_auth"
}
```

> **Security note**: The new password is transmitted over the current MQTT connection in plaintext
> if TLS is disabled. Enable TLS (`mqtt_set_tls`) before changing credentials on production devices.

---

## `mqtt_set_client_id`

Change the MQTT client identifier. This must be unique per device on the broker.
Saved to flash.

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | string | Yes | New client ID (non-empty) |

```json
{"command": "mqtt_set_client_id", "id": "tracker-unit-007"}
```

### Response — saved

```json
{
  "status": "saved",
  "client_id": "tracker-unit-007",
  "note": "Send mqtt_restart to reconnect with new client ID",
  "command_processed": "mqtt_set_client_id"
}
```

### Response — missing

```json
{
  "status": "missing_id",
  "hint": "{\"command\":\"mqtt_set_client_id\",\"id\":\"my_device\"}",
  "command_processed": "mqtt_set_client_id"
}
```

> **Important**: After changing the client ID, the device will subscribe to the **same**
> `sub_topic` as before. The broker may enforce unique client IDs — if two devices use the
> same ID, one will be kicked off. Use `mqtt_set_topics` to update topics to match the new ID.

---

## `mqtt_set_topics`

Change the publish and/or subscribe topics. Saved to flash.

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `pub_topic` | string | One of `pub_topic`/`sub_topic` required | Topic the device publishes data to |
| `sub_topic` | string | One of `pub_topic`/`sub_topic` required | Topic the device subscribes for commands |

```json
{
  "command": "mqtt_set_topics",
  "pub_topic": "gw/tracker-007/data",
  "sub_topic": "gw/tracker-007/command"
}
```

### Response — saved

```json
{
  "status": "saved",
  "pub_topic": "gw/tracker-007/data",
  "sub_topic": "gw/tracker-007/command",
  "note": "Send mqtt_restart to reconnect with new topics",
  "command_processed": "mqtt_set_topics"
}
```

> **Warning**: After `mqtt_restart`, you must publish commands to the **new** `sub_topic`.
> The device will no longer listen on the old topic.

---

## `mqtt_set_tls`

Enable or disable MQTT TLS and set the modem credential tag used for client authentication.
Saved to flash.

TLS uses the modem's built-in TLS stack. The security credential (server root CA or client cert)
must be pre-provisioned into the modem's credential store at the specified `sec_tag` before
enabling TLS.

### Parameters

| Field | Type | Required | Description |
|---|---|---|---|
| `enabled` | boolean | One of `enabled`/`sec_tag` required | `true` to enable TLS, `false` to disable |
| `sec_tag` | integer | One of `enabled`/`sec_tag` required | Modem TLS credential tag (0 = none) |

```json
{"command": "mqtt_set_tls", "enabled": true, "sec_tag": 16842754}
```

```json
{"command": "mqtt_set_tls", "enabled": false}
```

### Response

```json
{
  "status": "saved",
  "tls_enabled": true,
  "sec_tag": 16842754,
  "note": "Send mqtt_restart to reconnect with new TLS settings",
  "command_processed": "mqtt_set_tls"
}
```

### Credential tag reference

| Tag | Certificate | Used for |
|---|---|---|
| `16842754` | ISRG Root X1 (Let's Encrypt) | HTTPS FOTA downloads, MQTT TLS to Let's Encrypt-signed brokers |

Provision a certificate via AT command:
```
AT%CMNG=0,16842754,0,"-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----"
```

---

## `mqtt_restart`

Disconnects from the current broker and reconnects using the active configuration (including any
recently saved changes). The response is published **before** disconnecting.

**No parameters required.**

```json
{"command": "mqtt_restart"}
```

### Response

```json
{
  "status": "restarting",
  "host": "217.154.155.83",
  "port": 1883,
  "command_processed": "mqtt_restart"
}
```

The reconnect happens 2 seconds after this response is published, giving time for the response
to be acknowledged and delivered. If the broker or credentials have changed, you must subscribe
to the new topic **before** issuing `mqtt_restart` (if different from current).

### Reconnect behavior

- Uses exponential backoff on connection failure, starting from `MQTT_RECONNECT_BASE_DELAY_SEC`
- If a new `sub_topic` was set, subscribes to it on reconnect (old subscription drops automatically)
- If TLS settings changed, the new TLS mode applies from the next connection onwards

---

## Full reconfiguration example

Move the device to a new TLS-enabled broker:

```bash
BROKER=217.154.155.83
PORT=1883
TOPIC_CMD="gateway/gateway_XXXX/command"

pub() {
  mosquitto_pub -h $BROKER -p $PORT -u mqttuser -P mqttuser -t "$TOPIC_CMD" -m "$1"
}

# 1. Set the new broker
pub '{"command":"mqtt_set_broker","host":"new-broker.example.com","port":8883}'

# 2. Set credentials
pub '{"command":"mqtt_set_auth","username":"prod_user","password":"s3cr3t"}'

# 3. Enable TLS (credential must already be provisioned)
pub '{"command":"mqtt_set_tls","enabled":true,"sec_tag":16842754}'

# 4. Update topics to match new broker convention
pub '{"command":"mqtt_set_topics","pub_topic":"devices/tracker007/up","sub_topic":"devices/tracker007/down"}'

# 5. Reconnect (device will now be on the new broker)
pub '{"command":"mqtt_restart"}'
# After this the device is no longer reachable on the old broker.
# Switch your subscriber to the new broker + new pub_topic.
```
