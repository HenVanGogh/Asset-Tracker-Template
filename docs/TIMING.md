# MQTT & Scheduling Timing Reference

This document lists every timer, interval, and scheduling constant in the firmware and explains how to change them.

---

## Power / Sensor Sample Interval

**What it controls**: How often the device reads battery voltage, current, and temperature and publishes a `power` MQTT message.

| Item | Value | File |
|------|-------|------|
| `CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS` | **1800 s (30 min)** | `app/src/Kconfig.main` line ~10 |

**To change**: Edit `Kconfig.main`:
```kconfig
config APP_MODULE_TRIGGER_TIMEOUT_SECONDS
    int "…"
    default 1800   # ← change this value (seconds)
```
Or override in `prj.conf`:
```
CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS=3600
```

**How it works**:
1. On MQTT connect, the SMF auto-descends to `STATE_SAMPLE_DATA` → first sample fires immediately.
2. `wait_for_trigger_entry()` (`main.c`) schedules `trigger_work` for the next interval.
3. `trigger_work` publishes to `TIMER_CHAN` → `wait_for_trigger_run()` transitions to `STATE_SAMPLE_DATA` again.
4. Repeat indefinitely while MQTT is connected.

---

## Heartbeat Interval

**What it controls**: How often a `{"type":"heartbeat",…}` diagnostic message is published.

| Item | Value | File |
|------|-------|------|
| `MQTT_HEARTBEAT_INTERVAL_SEC` | **3600 s (60 min)** | `app/src/modules/custom_mqtt/custom_mqtt_config.h` line ~32 |

**To change**: Edit `custom_mqtt_config.h`:
```c
#define MQTT_HEARTBEAT_INTERVAL_SEC  3600   /* seconds */
```

**How it works**:
- `connected_entry()` (`custom_mqtt.c`) calls `k_work_schedule(&data_send_work, K_SECONDS(10))` → first heartbeat fires 10 s after connect.
- `data_send_work_handler()` sends the heartbeat and calls `k_work_schedule(&data_send_work, K_SECONDS(MQTT_HEARTBEAT_INTERVAL_SEC))` → reschedules itself.

---

## MQTT Connect: Initial Status Message

**What it controls**: A one-time `{"status":"connected",…}` message sent immediately on every MQTT connect (separate from heartbeat).

**Location**: `connected_entry()` in `app/src/modules/custom_mqtt/custom_mqtt.c` line ~966.

There is no timeout constant — it fires once per connection. To disable it, comment out the `cJSON`-build block in `connected_entry()`.

---

## ESL Poll Interval (UART Sensor Module)

**What it controls**: How often the nRF9151 sends the `esl_c nus pawr-poll` command to the nRF5340 to request tag sensor data via NUS-over-PAwR.

| Item | Value | File |
|------|-------|------|
| `ESL_POLL_INTERVAL_SEC` | see `uart_sensor.c` | `app/src/modules/uart_sensor/uart_sensor.c` |

Search for `ESL_POLL_INTERVAL_SEC` in `uart_sensor.c` to find the current value and `esl_poll_work` scheduling.

---

## Location Update Interval (currently disabled)

| Item | Value | File |
|------|-------|------|
| `MQTT_LOCATION_UPDATE_INTERVAL_SEC` | 300 s (disabled) | `custom_mqtt_config.h` line ~34 |

The location work is commented out. To enable it, uncomment the `k_work_schedule(&mqtt_ctx.location_trigger_work, …)` line in `connected_entry()`.

---

## MQTT Reconnect Delays

| Constant | Value | Meaning |
|----------|-------|---------|
| `MQTT_RECONNECT_BASE_DELAY_SEC` | 5 s | Starting back-off delay |
| `MQTT_RECONNECT_MAX_DELAY_SEC` | 300 s | Maximum back-off delay (5 min) |

Both in `custom_mqtt_config.h`.

---

## Summary Table

| Message | Frequency | Where to change |
|---------|-----------|-----------------|
| `power` | Every 30 min | `Kconfig.main` → `APP_MODULE_TRIGGER_TIMEOUT_SECONDS` |
| `heartbeat` | Every 60 min (first: 10 s after connect) | `custom_mqtt_config.h` → `MQTT_HEARTBEAT_INTERVAL_SEC` |
| `connected` status | Once per MQTT connect | `connected_entry()` in `custom_mqtt.c` |
| `esl_tag_found` | On BLE scan discovery | Event-driven, no timer |
| `esl_tag_configured` | On tag ESL provisioning | Event-driven, no timer |
| `esl_tag_connected` | On tag BLE reconnect | Event-driven, no timer |
| `esl_sensor_data` | On each NUS/PAwR poll response | Event-driven (poll driven by `ESL_POLL_INTERVAL_SEC`) |

---

## Known Issues Fixed

- **Duplicate power at boot**: Previously `triggering_entry()` scheduled `trigger_work` with `K_NO_WAIT`, causing a second `sensor_and_poll_triggers_send()` call alongside the SMF auto-descent to `STATE_SAMPLE_DATA`. Fixed by scheduling with `K_SECONDS(interval_sec)` so `wait_for_trigger_entry()` cancels it before it fires.
