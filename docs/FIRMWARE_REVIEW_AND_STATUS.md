# Firmware Review & Status — nRF9151 Thingy:91 X Asset Tracker

**Date:** 2026-04-02  
**Platform:** nRF9151 on Thingy:91 X  
**NCS version:** v3.0.2  
**Board:** `thingy91x_nrf9151_ns`

---

## Architecture Overview

The firmware uses Zephyr's State Machine Framework (SMF) with ZBUS for inter-module messaging.

### Modules

| Module | File | Purpose |
|---|---|---|
| Main | `app/src/main.c` | Top-level SMF: idle ↔ triggering ↔ sampling cycle |
| Custom MQTT | `app/src/modules/custom_mqtt/custom_mqtt.c` | MQTT client, command handler, data publishing |
| Network | `app/src/modules/network/network.c` | LTE connection management, PSM/eDRX |
| Power | `app/src/modules/power/power.c` | Battery monitoring via nPM1300 + nRF Fuel Gauge |
| UART/SPI Sensor | `app/src/modules/uart_sensor/uart_sensor.c` | SPI bridge to nRF5340 for ESL BLE tags |
| Environmental | `app/src/modules/environmental/environmental.c` | BME680 temp/humidity/pressure |
| Location | `app/src/modules/location/location.c` | GNSS + cellular location |
| LED | `app/src/modules/led/led.c` | RGB LED status indicators |
| Button | `app/src/modules/button/button.c` | Physical button handling |

### Communication Flow

```
ESL BLE Tags ←BLE→ nRF5340 ←SPI→ nRF9151 ←LTE→ MQTT Broker
                                     ↑
                              BME680 / nPM1300 / GNSS
```

---

## Runtime-Configurable Parameters (via MQTT commands)

All parameters are persisted in Zephyr Settings (flash) and survive reboots.

### MQTT Broker Configuration

| Command | Description | Example |
|---|---|---|
| `mqtt_get_config` | Get current MQTT config | `{"command":"mqtt_get_config"}` |
| `mqtt_set_broker` | Change broker host/port | `{"command":"mqtt_set_broker","host":"192.168.1.1","port":1883}` |
| `mqtt_set_auth` | Change username/password | `{"command":"mqtt_set_auth","username":"user","password":"pass"}` |
| `mqtt_set_client_id` | Change device ID | `{"command":"mqtt_set_client_id","id":"my_device"}` |
| `mqtt_set_topics` | Change pub/sub topics | `{"command":"mqtt_set_topics","pub_topic":"gw/dev1/data","sub_topic":"gw/dev1/cmd"}` |
| `mqtt_set_tls` | Enable/disable TLS | `{"command":"mqtt_set_tls","enabled":true,"sec_tag":12345}` |
| `mqtt_restart` | Apply config changes | `{"command":"mqtt_restart"}` |

**Note:** After changing broker/auth/topics/TLS, send `mqtt_restart` to reconnect with new settings.

### Power Mode

| Command | Description | Example |
|---|---|---|
| `set_power_mode` | Switch normal/high power | `{"command":"set_power_mode","mode":"high"}` |
| `set_power_mode` | Back to normal | `{"command":"set_power_mode","mode":"normal"}` |

**Normal mode:** 30-minute trigger interval, 60-minute heartbeat, PSM active.  
**High power mode:** 60-second trigger interval, 5-minute heartbeat, location updates enabled.  
Power mode is reported in every heartbeat message.

### ESL BLE Management

| Command | Description |
|---|---|
| `esl_scan` | Start/stop BLE scan for ESL tags |
| `esl_list_tags` | List known tags |
| `esl_nus_status` | Query NUS status of a tag |
| `esl_nus_sensors` | Query sensor data from tag |
| `esl_poll` | Start/stop periodic tag polling |
| `esl_get_tags` | Get full tag info as JSON |
| `esl_set_expected_tags` | Set expected tag count |
| `esl_get_name` | Get sensor names |
| `esl_command` / `esl_raw` | Raw ESL shell commands |
| `uart_passthrough` | Direct shell command pass-through |

### Sensor Configuration

| Command | Description |
|---|---|
| `sensor_get_config` | Get poll interval, retry, expected tags, NUS failures |
| `sensor_set_poll_interval` | Set ESL polling interval (10-86400 s) |
| `sensor_set_scan_retry_interval` | Set scan retry interval |
| `sensor_set_nus_failures` | Set max NUS failures before rescan |

---

## Design Decisions & Rationale

### clean_session = 0 (Persistent MQTT Sessions)

The device enters PSM (Power Saving Mode) between sampling cycles. During PSM sleep
the device is unreachable and does not listen to MQTT. With `clean_session=0`, the
broker queues QoS 1 commands. On reconnection (within the 6-second RAT window),
all pending commands are delivered immediately. This is essential for low-power
operation where commands must not be lost during sleep periods.

### Inactivity Watchdog (3600 s reboot)

A previous firmware bug caused the device to silently stop publishing data. The
1-hour inactivity watchdog is a safety net: if no successful MQTT publish, subscribe
acknowledgment, or ping response occurs for 1 hour, the device reboots. A flag is
saved to flash before reboot and reported as `reboot_reason: mqtt_inactivity` on
the next connection. This is intentionally aggressive — a production device must
never silently go offline.

### Environmental Sensor (Disabled)

The BME680 environmental sensor (temperature, humidity, pressure) is available on
the Thingy:91 X but is currently disabled in the firmware. The sensor data is not
needed for the current deployment. The module code is complete and can be re-enabled
by uncommenting the sampling block in `main.c` and removing the debug guard.

### Location Module (Known Issue — Disabled)

The GNSS/cellular location module has a known heap corruption issue when triggered
automatically. Manual location requests via MQTT (`get_location` command) use a
direct API bypass that works. Automatic periodic location triggering is disabled
until the heap issue is resolved. The root cause is suspected to be ZBUS buffer
exhaustion when location + sensor publishes overlap.

### TLS Support

TLS is compiled in (`CONFIG_MQTT_LIB_TLS=y`) but currently disabled at runtime
(`CONFIG_APP_CUSTOM_MQTT_USE_TLS=n`). TLS can now be enabled at runtime via the
`mqtt_set_tls` command without recompilation. When enabled, the device uses the
modem's credential storage with the configured security tag.

---

## Logging

All modules are currently set to `LOG_LEVEL_DBG` for development visibility.
For production deployment with optimized power consumption, these should be
reduced to `LOG_LEVEL_WRN` or `LOG_LEVEL_ERR`:

```
# In prj.conf — change _DBG to _WRN for production:
CONFIG_APP_CUSTOM_MQTT_LOG_LEVEL_DBG=y
CONFIG_APP_BUTTON_LOG_LEVEL_DBG=y
CONFIG_APP_LOCATION_LOG_LEVEL_DBG=y
CONFIG_APP_CLOUD_LOG_LEVEL_DBG=y
CONFIG_APP_NETWORK_LOG_LEVEL_DBG=y
CONFIG_APP_LOG_LEVEL_DBG=y
```

Debug logging increases flash/RAM usage and power consumption but provides
essential diagnostic information during the current development/integration phase.

---

## Versioning

Firmware version is tracked in `app/VERSION`:

```
VERSION_MAJOR = X
VERSION_MINOR = Y
PATCHLEVEL = Z
VERSION_TWEAK = <build_number>
EXTRAVERSION = dev|rc|release
```

The build system generates `APP_VERSION_STRING` from this file. Major/minor versions
are bumped manually for releases. The build number is auto-incremented by the
`scripts/app_version.py` script before each build.

The heartbeat message includes `firmware_version` with the proper version string.

Build artifacts should be archived externally with naming:
`asset-tracker-v{MAJOR}.{MINOR}.{PATCH}+{BUILD}-thingy91x.hex`

---

## Battery & Charging

The power module reads battery state from the nPM1300 PMIC via the nRF Fuel Gauge
library. The following data is reported:

- **percentage** — State of charge (0-100%)
- **voltage** — Battery terminal voltage
- **current_ma** — Instantaneous current (positive = charging, negative = discharging)
- **temperature** — Battery/PMIC temperature
- **charging** — Whether the device is connected to external power (USB/mains)
- **data_valid** — Whether the sensor read succeeded (false = no real data available)

When the sensor read fails, `data_valid` is set to `false` and no fake values are
reported. This prevents misinterpretation of battery state.

Charging detection uses the current sign: positive current with voltage > 4.0V
indicates active USB/mains charging.

---

## Known Issues & Limitations

1. **Location heap corruption** — Automatic location requests disabled; manual MQTT trigger works
2. **Environmental sensor disabled** — Not needed for current deployment
3. **No MQTT command authentication** — Any client on the command topic can control the device; broker-level ACLs recommended
4. **No rate limiting on commands** — Rapid command spam possible; relies on broker ACLs
5. **FOTA not tested with custom MQTT** — FOTA module compiled but untested on this config

---

## File Quick Reference

| Path | Description |
|---|---|
| `app/prj.conf` | Main Kconfig — MQTT broker, module enables, stack sizes |
| `app/boards/thingy91x_nrf9151_ns.conf` | Board-specific config — sensors, LED, WiFi disable |
| `app/boards/thingy91x_nrf9151_ns.overlay` | Device tree — UART0, BME680, SPI pins |
| `app/src/modules/custom_mqtt/custom_mqtt_config.h` | Tunable constants — timeouts, thresholds |
| `app/VERSION` | Firmware version (major.minor.patch+build) |
