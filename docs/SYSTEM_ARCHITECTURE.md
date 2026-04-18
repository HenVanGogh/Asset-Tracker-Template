# ESL Gateway — System Architecture

## Overview

The **Thingy:91x** board combines two Nordic chips to form a cloud-connected BLE Electronic Shelf Label (ESL) gateway:

| Chip | Role |
|------|------|
| **nRF5340** | BLE Access Point — manages ESL tags via PAwR and NUS |
| **nRF9151** | Cloud Controller — runs LTE, MQTT, GPS, and application logic |

The nRF9151 is the master brain. It sends `esl_c` shell commands to the nRF5340 over an internal SPI bus and parses `#`-prefixed URC (unsolicited result code) notifications back.

```
Cloud (MQTT broker)
       │ LTE-M
  nRF9151 (Asset Tracker — application runs here)
       │ SPI1 (520-byte fixed frames, 4 MHz)
  nRF5340 (ESL Access Point)
       │ BLE PAwR + NUS
  ESL Tags (sensors)
```

---

## Source Layout

```
app/src/
  modules/
    uart_sensor/       ← SPI bridge + ESL tag management (uart_sensor.c/.h)
    custom_mqtt/       ← MQTT client + cloud protocol (custom_mqtt.c/.h)
    network/           ← LTE network management
    power/             ← Battery/power sampling
    location/          ← GNSS/cellular location
    button/            ← Hardware button handler
    led/               ← LED control
  common/              ← Shared types (zbus channels, message structs)
```

---

## Module: uart_sensor (SPI Bridge + ESL Tag Management)

### SPI Frame Protocol

Fixed 520-byte frames:

```
[0]     MAGIC    0xA5
[1]     TYPE     CMD=0x01 | CMD_RESP=0x02 | NOTIFY=0x03 | NOOP=0x05
[2-3]   LEN      uint16 little-endian — payload length
[4-515] PAYLOAD  zero-padded to 512 bytes
[516]   CRC8     CRC-8/ATM (poly=0x07) over bytes [0..515]
[517-519] reserved
```

Data-ready signal: nRF5340 P1.08 → nRF9151 P0.04 (rising edge = data queued on nRF5340).

### ESL Tag Database

In-RAM array `tag_db[ESL_MAX_TAGS]` of `esl_tag_info`:
- `esl_addr`      — ESL address (0x0000–0x01FF)
- `mac[18]`       — BLE MAC address (set from `#TAG_SCANNED:` or `#TAG:`)
- `name[13]`      — Human-readable name (from `#SENSOR_NAME:`, persisted to flash)
- `battery_mv`    — Last known battery voltage
- `uptime_s`      — Last known tag uptime
- `temperature`   — Last known temperature
- `flags`         — Last known status flags
- `connected`     — Whether tag is reachable via NUS/PAwR
- `last_seen`     — k_uptime_get() of last data reception

Names are persisted under `app/tag/<hex4>/name` in the Zephyr settings / FCB backend, and restored on boot before the startup sequence fires.

### Boot Sequence

```
SYS_INIT → uart_sensor_module_init()
  1. settings_load_subtree("app/tag")  ← restore names
  2. Delay 3 s (nRF5340 boot time)
  3. esl_startup_work_fn, step 0: send empty newline (clean prompt)
  4. Step 1: "esl_c pawr start_pawr"
  5. Step 2: "esl_c acl scan 1 1"  (start BLE scan)
  6. Step 3: start poll timer, esl_discovery_check(), try_send_time_to_ap(),
             "esl_c nus get_name" (request names for known tags)
```

### Tag Discovery Flow

```
nRF5340 → "#TAG_SCANNED: 1,E9:76:EE:E9:06:5F (random) to list"
  → parse_tag_scanned() — add to tag_db (addr=0xFFFF until configured)
  → ZBUS ESL_TAG_FOUND → custom_mqtt → "esl_tag_found" MQTT

nRF5340 → "#CONFIGURED: 1,0x0000,0x0000"
  → parse_configured() — update esl_addr in tag_db
  → ZBUS ESL_TAG_CONFIGURED → custom_mqtt → "esl_tag_configured" MQTT
  → "esl_c nus get_name"  ← auto-request name for newly provisioned tag

nRF5340 → "#SENSOR_NAME:0x0000,SENSOR01"
  → parse in process_data_line()
  → store in tag_db[].name
  → settings_save_one("app/tag/0000/name", ...)  ← persist to flash
  → ZBUS ESL_NAME_RESPONSE → custom_mqtt → "sensor_name" MQTT
```

### Periodic Polling

Every `CONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC` (default 600 s):
- Sends `"esl_c nus status <esl_id>"` for each known tag
- nRF5340 replies `"#NUS_STATUS:UP=3936s,BATT=4134mV,FLAGS=0x01"`
- Parses into tag_db, publishes ZBUS `ESL_NUS_RESPONSE` → MQTT `esl_sensor_data`

Failure tracking: after `CONFIG_APP_UART_SENSOR_NUS_MAX_POLL_FAILURES` consecutive no-responses, all tags are marked disconnected and a re-scan is triggered.

### Smart Scan (Discovery Loop)

`esl_discovery_check()` fires after startup and after any tag disconnect/configure event:
- If `connected_count < expected_tag_count` → schedule `scan_retry_work`
- `scan_retry_work` re-issues `"esl_c acl scan 1 1"` every `scan_retry_interval_sec`
- Scan stops automatically once enough tags are connected

`expected_tag_count` defaults to `CONFIG_APP_UART_SENSOR_ESL_EXPECTED_TAGS` (default = 1), configurable via MQTT command `esl_set_expected_tags`.

### Disconnection Events from nRF5340

The nRF5340 outputs two types of "Disconnected" events:

| Message | Meaning | Action |
|---------|---------|--------|
| `"Disconnected:Conn_idx:0xNN"` | Provisioning BLE connection ended — NORMAL | Log only, trigger rescan if below expected count |
| `"Disconnected: AA:BB:CC:DD:EE:FF ..."` | Actual tag BLE disconnect with MAC | Mark tag disconnected, publish `esl_tag_disconnected`, trigger rescan |

### Time Sync

When the nRF9151 gets a `DATE_TIME_OBTAINED` event from the LTE stack:
- Sends `"AP_SET_TIME:<epoch>"` to nRF5340
- nRF5340 replies `"#AP_EPOCH_SET:<epoch>"` on success or `"#AP_GET_TIME"` to request it

---

## Module: custom_mqtt (MQTT Client)

### State Machine

```
IDLE ──(network up)──► CONNECTING ──(CONNACK OK)──► CONNECTED
                                                         │
CONNECTED ──(network down / max failures)──► DISCONNECTING ──► IDLE
                                                         │
CONNECTED or CONNECTING ──(repeated failures)──► ERROR ──(retry backoff)──► IDLE
```

Reconnect backoff: `MQTT_RECONNECT_BASE_DELAY_SEC` → doubles on each failure, capped at 5 minutes.

### MQTT Topics

| Direction | Topic (from Kconfig) | Content |
|-----------|---------------------|---------|
| Publish   | `CONFIG_APP_CUSTOM_MQTT_PUBLISH_TOPIC`   | JSON messages (see below) |
| Subscribe | `CONFIG_APP_CUSTOM_MQTT_SUBSCRIBE_TOPIC` | JSON commands |

All published messages are QoS 1 and include `"device_id"` and `"timestamp"` (k_uptime in ms).

### Published Message Types

| `"type"` | Trigger | Key Fields |
|----------|---------|------------|
| `esl_tag_found` | Tag seen during BLE scan | `mac` |
| `esl_tag_configured` | Tag provisioned with ESL addr | `esl_id` |
| `esl_tag_connected` | Tag BLE connected | `mac`, `esl_id` |
| `esl_tag_disconnected` | Tag BLE disconnected (MAC format) | `mac` |
| `esl_sensor_data` | NUS poll result | `esl_id`, `name`, `battery_mv`, `battery_pct`, `uptime_s`, `flags`, `temperature` |
| `sensor_name` | Name received from tag via NUS | `esl_addr`, `name` |
| `esl_tag_list` | Response to `esl_list_tags` command | `tags[]`, `tag_count` |
| `heartbeat` | Every `MQTT_HEARTBEAT_INTERVAL_SEC` | `firmware_version`, `uptime_ms`, `diagnostics` |
| `power` | Battery/power sample | `percentage`, `voltage`, `current_ma`, `temperature` |
| `location` | GNSS fix | `latitude`, `longitude`, `accuracy`, `method` |
| `reboot_reason` | First connection after watchdog reboot | `reason: "mqtt_inactivity"` |
| `connected` | Every MQTT (re)connection | `build`, `message` |

### MQTT Command Reference

Commands arrive as JSON on the subscribe topic:

```json
{"command": "<name>", ...args...}
```

#### ESL / Tag Commands

| Command | Args | Description |
|---------|------|-------------|
| `esl_scan` | `"action":"start"\|"stop"` | Start/stop BLE scan |
| `esl_list_tags` | — | List all known tags + request fresh data |
| `esl_status` | — | Gateway status (tag count, uptime, expected) |
| `esl_nus_status` | `"id":<esl_id>` | Poll NUS status for one tag |
| `esl_nus_sensors` | `"id":<esl_id>` | Poll NUS sensor data for one tag |
| `esl_poll` | `"action":"start"\|"stop"` | Start/stop auto periodic poll |
| `esl_command` | `"args":"<shell args>"` | Send raw `esl_c` shell command |
| `esl_get_name` | `"esl_id":<n>` optional | Request name from tag (omit for all) |
| `esl_set_expected_tags` | `"count":<n>` | Set expected tag count (persisted) |

#### Sensor Config Commands

| Command | Args | Description |
|---------|------|-------------|
| `sensor_get_config` | — | Get poll interval, scan retry, expected tags, NUS failures |
| `sensor_set_poll_interval` | `"interval_s":<n>` | Poll interval in seconds (persisted) |
| `sensor_set_scan_retry_interval` | `"interval_s":<n>` | Scan retry interval in seconds (persisted) |
| `sensor_set_nus_failures` | `"count":<n>` | Max consecutive NUS poll failures (persisted) |

#### MQTT Runtime Config Commands

| Command | Args | Description |
|---------|------|-------------|
| `set_mqtt_host` | `"host":"<hostname>"` | Override MQTT broker host (persisted) |
| `set_mqtt_port` | `"port":<n>` | Override MQTT broker port (persisted) |
| `set_mqtt_user` | `"user":"<username>"` | Override MQTT username (persisted) |
| `set_mqtt_pass` | `"pass":"<password>"` | Override MQTT password (persisted) |
| `mqtt_restart` | — | Disconnect and reconnect with updated config |
| `get_status` | — | Full system status |

#### System Commands
| Command | Description |
|---------|-------------|
| `get_location` | Trigger a GNSS/cellular location fix |
| `ap_set_time` | Push current UTC epoch to nRF5340 immediately |
| `uart_debug_on` / `uart_debug_off` | Enable/disable raw UART RX echo to MQTT |

UART passthrough (alternative format):
```json
{"type": "uart_passthrough", "command": "acl scan 1 1"}
```
→ Forwards directly as `esl_c <command>` to nRF5340 shell.

### Settings Persistence (Flash / FCB)

| Key prefix | Module | Content |
|------------|--------|---------|
| `app/mqtt/host` | custom_mqtt | MQTT broker hostname |
| `app/mqtt/port` | custom_mqtt | MQTT broker port (uint16) |
| `app/mqtt/user` | custom_mqtt | MQTT username |
| `app/mqtt/pass` | custom_mqtt | MQTT password |
| `app/mqtt/client_id` | custom_mqtt | MQTT client ID |
| `app/sensor/poll` | custom_mqtt | Poll interval (uint32, seconds) |
| `app/sensor/scan_retry` | custom_mqtt | Scan retry interval (uint32, seconds) |
| `app/sensor/expected` | custom_mqtt | Expected tag count (uint8) |
| `app/sensor/nus_fail` | custom_mqtt | Max NUS failures (uint8) |
| `app/watchdog/mqtt_inactive` | custom_mqtt | Flag: last reboot was watchdog (uint8) |
| `app/tag/<hex4>/name` | uart_sensor | Tag name string (e.g. `app/tag/0000/name`) |

### Inactivity Watchdog

If no MQTT publish succeeds for `MQTT_INACTIVITY_WATCHDOG_SEC` (3600 s / 1 hour):
1. Flash `app/watchdog/mqtt_inactive = 1`
2. Call `sys_reboot(SYS_REBOOT_COLD)`
3. On next boot, after first MQTT reconnect, publishes `{"type":"reboot_reason","reason":"mqtt_inactivity"}`
4. Clears the flag from flash

---

## Inter-Module Communication (ZBUS)

```
uart_sensor ──UART_SENSOR_CHAN──► custom_mqtt  (sensor data, tag events, names)
custom_mqtt ──CUSTOM_MQTT_CHAN──► uart_sensor  (MQTT commands forwarded to SPI)
network     ──NETWORK_CHAN─────► custom_mqtt  (LTE up/down)
power       ──POWER_CHAN───────► custom_mqtt  (battery data)
location    ──LOCATION_CHAN────► custom_mqtt  (GNSS data)
button      ──BUTTON_CHAN──────► custom_mqtt  (button press events)
```

---

## Build & Flash

```bash
# Build (from Asset-Tracker-Template/ directory)
docker run --rm \
  -v /home/mic/ncs/v3.0.2:/ncs \
  -v $(pwd)/app:/workspace/app \
  -w /workspace/app \
  ncs-v2.6.1-debian \
  west build --board thingy91x/nrf9151/ns

# Flash
./flash_docker.sh
```

Build output target: `app/build/app/zephyr/app_update.bin` (~331 KB flash, ~65% RAM).

---

## Known Limitations

- Only one scan/connect loop at a time (sequential, not parallel)
- `esl_c nus get_name` always targets the currently-synced tag (group 0, addr 0x00) — nRF5340 firmware does not accept per-tag addressing in the basic `nus get_name` command
- Location module uses cellular fallback if GNSS takes longer than configured timeout
