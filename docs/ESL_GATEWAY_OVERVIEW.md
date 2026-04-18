# ESL BLE Gateway — System Overview

## What This System Does

The **Thingy:91x** board acts as a cloud-connected gateway for **Electronic Shelf Label (ESL)** BLE sensors. It combines two Nordic chips on one board:

| Chip | Role | Connectivity |
|------|------|--------------|
| **nRF5340** | BLE Access Point — scans, connects, and manages ESL tags | Bluetooth 5.4 (PAwR, PAST) |
| **nRF9151** | Cloud Controller — runs LTE, MQTT, GPS, and all application logic | LTE-M / NB-IoT, GNSS |

The nRF9151 is the "brain." It sends shell commands to the nRF5340 over an internal UART bus and parses the responses. All management, scheduling, and cloud communication is handled by the nRF9151.

```
                     MQTT (LTE-M)
   Cloud  <─────────────────────────>  nRF9151 (Asset Tracker)
                                          │
                                    UART1 (115200 baud)
                                          │
                                       nRF5340 (ESL AP)
                                          │
                                     BLE PAwR + NUS
                                      ┌───┼───┐
                                      │   │   │
                                    Tag  Tag  Tag
                                   (ESL sensors)
```

---

## How It Works — Step by Step

### 1. Boot Sequence

1. Both chips power on simultaneously.
2. The nRF5340 initialises its BLE stack and ESL Access Point firmware, then prints a shell prompt (`ESL_AP:~$`) on UART1.
3. The nRF9151 detects this prompt and runs a startup sequence:
   - Starts **PAwR** (Periodic Advertising with Responses) — this is the BLE mechanism that keeps ESL tags synchronised.
   - Starts a **BLE scan** to discover nearby ESL tags.
4. The nRF9151 connects to the LTE network and establishes an MQTT session with the cloud broker.

### 2. Tag Discovery & Onboarding

When the nRF9151 issues a scan command, the nRF5340 advertises and listens for ESL tags. Discovered tags appear as:

```
#TAG_SCANNED: 1,E9:76:EE:E9:06:5F (random) to list
```

The nRF5340 automatically:
- Connects to each discovered tag
- Exchanges encryption keys
- Configures the tag with an ESL address and group
- Sets up PAwR synchronisation and PAST (Periodic Advertising Sync Transfer)

Once configured, the tags sync to BLE PAwR and wake up periodically to check for commands.

### 3. Periodic Sensor Polling (Every 5 Minutes)

A timer on the nRF9151 fires every **5 minutes** (configurable via `CONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC`). For each known tag, it sends:

```
esl_c nus status <tag_id>
```

The nRF5340 contacts the tag over **NUS (Nordic UART Service)** piped through BLE PAwR, and responds with:

```
#NUS_STATUS:UP=3936s,BATT=4134mV,FLAGS=0x01
```

The nRF9151 parses this, updates its in-memory tag database, and publishes the data to the cloud via MQTT.

### 4. Cloud MQTT Commands

The nRF9151 subscribes to an MQTT command topic. A cloud operator or management application can send JSON commands to manage the gateway remotely:

| Command | Description | Example Payload |
|---------|-------------|-----------------|
| `esl_scan` | Start or stop BLE scanning | `{"command":"esl_scan"}` or `{"command":"esl_scan","action":"stop"}` |
| `esl_list_tags` | List all known tags and request fresh data | `{"command":"esl_list_tags"}` |
| `esl_status` | Get gateway status (tag count, uptime) | `{"command":"esl_status"}` |
| `esl_nus_status` | Poll NUS status for a specific tag | `{"command":"esl_nus_status","id":1}` |
| `esl_nus_sensors` | Poll NUS sensor data for a specific tag | `{"command":"esl_nus_sensors","id":1}` |
| `esl_poll` | Start/stop automatic periodic polling | `{"command":"esl_poll"}` or `{"command":"esl_poll","action":"stop"}` |
| `esl_command` | Send a raw `esl_c` shell command | `{"command":"esl_command","args":"acl list"}` |
| `get_status` | Get gateway system status | `{"command":"get_status"}` |
| `get_location` | Trigger GNSS/cellular location fix | `{"command":"get_location"}` |
| `uart_passthrough` | Forward raw text to nRF5340 shell | `{"type":"uart_passthrough","command":"esl_c acl scan 1 1"}` |

Responses are published back on the MQTT data topic as JSON.

### 5. Data Published to Cloud

Every poll cycle (and on-demand), the gateway publishes JSON like:

```json
{
  "device_id": "gateway_A1F3",
  "timestamp": 1234567890,
  "tag_count": 3,
  "tags": [
    {
      "esl_addr": "0x0000",
      "mac": "E9:76:EE:E9:06:5F",
      "battery_mv": 4134,
      "uptime_s": 3936,
      "flags": 1
    }
  ],
  "gateway": {
    "temperature": 23.5,
    "humidity": 45.2,
    "battery": 87,
    "gnss_lat": 59.9139,
    "gnss_lon": 10.7522
  }
}
```

---

## Architecture Diagram

```
┌────────────────────────────────────────────────────────────┐
│                     Cloud / MQTT Broker                    │
│                   (217.154.155.83:1883)                     │
└─────────────┬──────────────────────┬───────────────────────┘
              │ Subscribe            │ Publish
              │ .../command          │ .../data
              ▼                      ▲
┌─────────────────────────────────────────────────────────────┐
│                    nRF9151 (Asset Tracker)                   │
│                                                             │
│  ┌──────────┐  ┌────────────┐  ┌──────────┐  ┌──────────┐ │
│  │ Network  │  │Custom MQTT │  │   UART   │  │ Environ. │ │
│  │ Module   │──│  Module    │──│  Sensor  │  │  Module  │ │
│  └──────────┘  └────────────┘  └────┬─────┘  └──────────┘ │
│                                     │                       │
│  ┌──────────┐  ┌────────────┐      │  ┌──────────────────┐ │
│  │ Location │  │   Power    │      │  │  Main (SMF/ZBUS) │ │
│  │  Module  │  │  Module    │      │  └──────────────────┘ │
│  └──────────┘  └────────────┘      │                       │
└────────────────────────────────────┼───────────────────────┘
                                     │ UART1 (115200 8N1 + HW FC)
┌────────────────────────────────────┼───────────────────────┐
│                    nRF5340 (ESL AP)│                        │
│                                    │                        │
│  ┌──────────────┐    ┌─────────────┘                       │
│  │  Zephyr Shell │◄──┘                                     │
│  │  (esl_c cmds) │                                         │
│  └───────┬───────┘                                         │
│          │                                                  │
│  ┌───────▼────────┐                                        │
│  │  ESL AP Stack   │                                       │
│  │  BLE 5.4 PAwR   │                                      │
│  │  PAST, NUS, OTS │                                      │
│  └───────┬─────────┘                                       │
└──────────┼─────────────────────────────────────────────────┘
           │ BLE PAwR (Periodic Advertising with Responses)
     ┌─────┼─────┐
     ▼     ▼     ▼
   ┌───┐ ┌───┐ ┌───┐
   │Tag│ │Tag│ │Tag│   ESL BLE Sensors
   └───┘ └───┘ └───┘
```

---

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **nRF5340 runs unmodified ESL AP shell** | Battle-tested BLE stack; only change is redirecting shell I/O from USB to UART1 |
| **nRF9151 sends `esl_c` commands as plain text** | Same commands used during development/testing; no custom binary protocol needed |
| **nRF5340 stays always-on** | Must maintain BLE PAwR advertising to keep ESL tags synchronised |
| **5-minute poll interval** | Balances battery life of tags with data freshness; configurable 30s–3600s |
| **All logic on nRF9151** | Single firmware to update for feature changes; nRF5340 is "dumb pipe" for BLE |

---

## Hardware: Thingy:91x Inter-Chip UART

The two chips communicate over **UART1** with hardware flow control:

| Signal | nRF9151 Pin | nRF5340 Pin |
|--------|-------------|-------------|
| TX     | P0.05       | P1.08 (RX)  |
| RX     | P0.04       | P1.06 (TX)  |
| RTS    | P0.23       | P0.30       |
| CTS    | P0.22       | P0.31       |

Settings: **115200 baud, 8N1, hardware flow control (RTS/CTS)**.

---

## Building & Flashing

The system uses two separate NCS (nRF Connect SDK) versions:

| Chip | NCS Version | Build Command |
|------|-------------|---------------|
| nRF5340 | v2.8.0 | `west build -b thingy91x/nrf5340/cpuapp -- -DSHIELD=nrf7002ek` |
| nRF9151 | v3.0.2 | `west build -b thingy91x/nrf9151/ns` |

Both are flashed via J-Link or the `nrf91_flasher.py` script.

---

## Configurable Parameters

| Kconfig Option | Default | Description |
|---------------|---------|-------------|
| `CONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC` | 300 | How often to poll all tags (seconds) |
| `CONFIG_APP_UART_SENSOR_ESL_MAX_TAGS` | 16 | Maximum tracked tags |
| `CONFIG_APP_UART_SENSOR_ESL_STARTUP_DELAY_MS` | 500 | Delay before startup sequence |
| `CONFIG_APP_UART_SENSOR_ESL_CMD_SPACING_MS` | 2000 | Gap between shell commands |
| `CONFIG_APP_CUSTOM_MQTT_BROKER_HOSTNAME` | 217.154.155.83 | MQTT broker address |
| `CONFIG_APP_CUSTOM_MQTT_BROKER_PORT` | 1883 | MQTT broker port |
| `CONFIG_APP_MODULE_TRIGGER_TIMEOUT_SECONDS` | 600 | Main data report interval |

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| No `ESL_AP:~$` prompt detected | nRF5340 not booting or UART pins misconfigured | Check overlay DTS; verify UART1 pin mapping |
| Tags not discovered | PAwR not started, or tags out of range | Send `{"command":"esl_scan"}` via MQTT |
| `#NUS_STATUS` not received | Tag lost PAwR sync | Re-scan and let AP re-configure tag |
| MQTT not connecting | No LTE coverage or broker unreachable | Check `get_status` command; verify SIM card |
| Tag battery reads 0 | NUS status not yet polled | Wait for next poll cycle or trigger manually |
