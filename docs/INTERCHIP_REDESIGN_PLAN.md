# nRF9151 ↔ nRF5340 Inter-Chip Communication Redesign

## Executive Summary

This document defines the architecture for redesigning the communication between the **nRF9151** (LTE modem + application brain) and the **nRF5340** (BLE radio peripheral) on the Thingy:91x board.

**Key principle:** The nRF9151 is the **master** — all business logic, scheduling, and decision-making lives here. The nRF5340 is a **dumb BLE radio** that only executes commands from the nRF9151 and reports results back.

---

## Table of Contents

1. [Current State Analysis](#1-current-state-analysis)
2. [Problems with Current Architecture](#2-problems-with-current-architecture)
3. [New Architecture Overview](#3-new-architecture-overview)
4. [UART Transport Layer](#4-uart-transport-layer)
5. [Protocol Design](#5-protocol-design)
6. [nRF9151 Changes (uart_bridge Module)](#6-nrf9151-changes-uart_bridge-module)
7. [nRF5340 Changes (Slave Firmware)](#7-nrf5340-changes-slave-firmware)
8. [State Machine Design](#8-state-machine-design)
9. [Error Handling & Recovery](#9-error-handling--recovery)
10. [Data Flow Diagrams](#10-data-flow-diagrams)
11. [Implementation Plan](#11-implementation-plan)
12. [Docker Build Integration](#12-docker-build-integration)
13. [Testing Strategy](#13-testing-strategy)

---

## 1. Current State Analysis

### nRF5340 (ESL AP — central_esl)
- **~9,300 lines** of complex ESL Access Point code (NCS v2.8.0)
- Full ESL profile: scanning, connecting, PAwR, PAST, OTS, EAD encryption
- NUS-over-PAwR vendor commands (status, sensors, config, LED, reset)
- Shell-based CLI over USB CDC ACM (`esl_c` commands)
- Tag database on LittleFS (internal flash)
- Auto-onboarding with configurable ESL address allocation
- 3-image sysbuild: MCUboot + App + ipc_radio (net core BLE controller)
- **No UART communication to nRF9151 — all interaction via USB to host PC**

### nRF9151 (Asset-Tracker-Template)
- Modular architecture using Zephyr SMF + ZBUS (NCS v3.0.2)
- Modules: network, custom_mqtt, uart_sensor, environmental, power, button, led, location, fota
- `uart_sensor` module: ISR-driven UART1 RX, parses `name:temp,hum,bat_mv` format
- MQTT bridge: sensor data to cloud, commands from cloud forwarded to UART
- Simple text protocol: `status\n`, `list\n`, `connect <name>\n`, etc.
- **Designed for a simple BLE scanner, NOT for ESL AP complexity**

### Physical Interface
| nRF9151 Pin | Signal | nRF5340 Pin |
|---|---|---|
| P0.5 (TX) | UART1_TX → | P1.6 (RX) |
| P0.4 (RX) | UART1_RX ← | P1.8 (TX) |
| P0.23 (RTS) | Flow ctrl → | P0.31 (CTS) |
| P0.22 (CTS) | Flow ctrl ← | P0.30 (RTS) |

**Baud: 115200, 8N1, HW flow control**

---

## 2. Problems with Current Architecture

| Problem | Impact | Severity |
|---|---|---|
| **Protocol mismatch** — nRF5340 ESL AP uses shell commands (`esl_c acl scan 1 1`), nRF9151 sends simple `status\n` | Cannot communicate at all | Critical |
| **nRF5340 has too much intelligence** — it runs shell, manages tag DB, handles auto-onboarding, makes decisions | Duplicated logic, hard to coordinate | High |
| **No structured framing** — text lines with no sequence numbers, no checksums, no ACK/NAK | Lost commands, no delivery guarantee | High |
| **USB CDC dependency** — ESL AP expects shell over USB, not UART | Must redesign nRF5340 communication interface | Critical |
| **NCS version mismatch** — nRF5340 on v2.8.0, nRF9151 on v3.0.2 | Potential API incompatibilities | Medium |
| **No command-response correlation** — fire-and-forget with URC-style async responses | Cannot track which response matches which command | High |
| **Single UART line buffer** — 128 bytes, no ring buffer, ISR-assembled lines | Long ESL responses will be truncated/corrupted | Medium |
| **No heartbeat/watchdog** — no mechanism to detect if nRF5340 is alive | Silent failures go undetected | High |

---

## 3. New Architecture Overview

```
┌────────────────────────────────────── Thingy:91x ──────────────────────────────────┐
│                                                                                      │
│  ┌──────────────────────────────────┐    UART1     ┌──────────────────────────────┐  │
│  │         nRF9151 (MASTER)         │◄════════════►│     nRF5340 (SLAVE)          │  │
│  │                                  │  115200 8N1   │                              │  │
│  │  ┌────────────────────────────┐  │  HW Flow Ctrl │  ┌────────────────────────┐  │  │
│  │  │   uart_bridge module       │  │               │  │  UART command handler  │  │  │
│  │  │   ├─ TX: framed commands   │──┼──TX────►RX───┼──│  ├─ parse & execute     │  │  │
│  │  │   ├─ RX: framed responses  │◄─┼──RX◄────TX──┼──│  ├─ BLE operations      │  │  │
│  │  │   ├─ sequence tracking     │  │               │  │  └─ report results     │  │  │
│  │  │   ├─ timeout & retry       │  │               │  └────────────────────────┘  │  │
│  │  │   └─ ZBUS interface        │  │               │                              │  │
│  │  └────────────────────────────┘  │               │  ┌────────────────────────┐  │  │
│  │                                  │               │  │  BLE subsystem         │  │  │
│  │  ┌────────────┐ ┌─────────────┐  │               │  │  ├─ Scanner            │  │  │
│  │  │custom_mqtt │ │   main.c    │  │               │  │  ├─ Central (connect)  │  │  │
│  │  │(cloud cmds)│ │ (SMF state  │  │               │  │  ├─ PAwR (sync comms)  │  │  │
│  │  │            │ │  machine)   │  │               │  │  ├─ NUS client         │  │  │
│  │  └────────────┘ └─────────────┘  │               │  │  └─ EAD encryption     │  │  │
│  │                                  │               │  └────────────────────────┘  │  │
│  │  Modules: network, env, power,   │               │                              │  │
│  │  button, led, location, fota     │               │  ┌────────────────────────┐  │  │
│  └──────────────────────────────────┘               │  │  ipc_radio (net core)  │  │  │
│                                                      │  │  BLE HCI controller    │  │  │
│                                                      │  └────────────────────────┘  │  │
│                                                      └──────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **nRF9151 is the brain** — decides what to scan, when to connect, what data to collect, when to report
2. **nRF5340 is the radio** — executes BLE commands, returns results, has no business logic
3. **Request-Response protocol** — every command gets an explicit response (ACK, NAK, or data)
4. **Sequence numbers** — correlate commands with responses, detect drops
5. **Binary-efficient framing** — length-prefixed with CRC for reliability
6. **Asynchronous events** — nRF5340 can send unsolicited events (scan results, disconnects)
7. **Heartbeat** — periodic keepalive to detect chip failures
8. **Idempotent commands** — safe to retry on timeout

---

## 4. UART Transport Layer

### Frame Format

We use a **length-prefixed binary frame** with ASCII payload for debuggability:

```
┌──────┬────────┬──────┬───────────┬───────┬──────┐
│ SOF  │ LEN_HI │LEN_LO│  PAYLOAD  │ CRC_HI│CRC_LO│
│ 0x7E │  (MSB) │(LSB) │  (ASCII)  │ (MSB) │(LSB) │
└──────┴────────┴──────┴───────────┴───────┴──────┘
  1B      1B      1B    0-1024 B      1B      1B
```

| Field | Size | Description |
|---|---|---|
| `SOF` | 1 byte | Start of frame: `0x7E` |
| `LEN` | 2 bytes | Payload length (big-endian, max 1024) |
| `PAYLOAD` | 0-1024 bytes | ASCII text (see protocol section) |
| `CRC` | 2 bytes | CRC-16/CCITT over `LEN + PAYLOAD` |

**Why this hybrid approach:**
- Binary framing guarantees reliable message boundaries (no "line corruption" issues)
- ASCII payload keeps messages human-readable in logs and easy to debug
- CRC catches UART bit errors, HW flow control prevents buffer overruns
- Small overhead (5 bytes) for reliability

### Escape Sequences

If `0x7E` appears in payload, it is escaped as `0x7D 0x5E`. If `0x7D` appears, it is escaped as `0x7D 0x5D`. This ensures the SOF byte is unambiguous for re-synchronization after errors.

### Frame Configuration

```
Baud rate:        115200
Max frame size:   1024 bytes payload + 5 bytes overhead = 1029 bytes
Max throughput:   ~11,520 bytes/sec → ~11 max-size frames/sec
Typical latency:  <1ms for short commands
```

### Alternative: Stick with Line-Based ASCII

For simplicity and faster initial bring-up, we can **keep** newline-terminated ASCII but add structure:

```
$SEQ,CMD,ARG1,ARG2,...*CHECKSUM\n
```

Example: `$01,SCAN_START,1,1*A3\n`

| Part | Description |
|---|---|
| `$` | Start marker |
| `SEQ` | 2-digit hex sequence number (00-FF, wraps) |
| `CMD` | Command name |
| `ARGn` | Comma-separated arguments |
| `*` | Checksum delimiter |
| `CHECKSUM` | 2-digit hex XOR of all bytes between `$` and `*` |
| `\n` | Terminator |

**Recommendation:** Start with **line-based ASCII** (simpler to debug, test via serial terminal) and upgrade to binary framing later if bandwidth or reliability demands it. The protocol layer is abstracted so the transport can be swapped.

---

## 5. Protocol Design

### 5.1 Message Types

Every message is one of:

| Type | Direction | Prefix | Description |
|---|---|---|---|
| **CMD** | 9151 → 5340 | `$` | Command from master |
| **RSP** | 5340 → 9151 | `@` | Response to a specific command |
| **EVT** | 5340 → 9151 | `!` | Unsolicited event from slave |
| **ACK** | 5340 → 9151 | `+` | Command accepted, processing |
| **NAK** | 5340 → 9151 | `-` | Command rejected with error code |

### 5.2 Command Format (nRF9151 → nRF5340)

```
$SEQ,COMMAND[,ARG1[,ARG2[,...]]]\n
```

Examples:
```
$01,PING\n
$02,BLE_SCAN_START,1\n
$03,BLE_CONNECT,E9:76:EE:E9:06:5F,random\n
$04,ESL_CONFIGURE,0x0000\n
$05,NUS_STATUS,0x0000\n
$06,NUS_SENSORS,0x0000\n
$07,PAWR_START\n
```

### 5.3 Response Format (nRF5340 → nRF9151)

```
@SEQ,STATUS[,DATA1[,DATA2[,...]]]\n
```

- `SEQ` matches the command's sequence number
- `STATUS` is `OK`, `ERR`, `BUSY`, or `TIMEOUT`

Examples:
```
@01,OK\n
@02,OK,SCANNING\n
@03,OK,CONNECTED,0\n
@03,ERR,CONN_FAILED,-11\n
@05,OK,UP=3936,BATT=4134,FLAGS=0x01\n
```

### 5.4 Event Format (nRF5340 → nRF9151)

```
!EVT_NAME[,DATA1[,DATA2[,...]]]\n
```

Events are **unsolicited** — they have no sequence number because they are not responses to commands.

| Event | Arguments | Description |
|---|---|---|
| `!TAG_FOUND,<mac>,<rssi>,<name>` | BLE address, RSSI, device name | Tag scanned during BLE scan |
| `!TAG_CONNECTED,<conn_idx>,<mac>` | Connection index, BLE address | ACL connection established |
| `!TAG_DISCONNECTED,<conn_idx>,<reason>` | Connection index, HCI reason | ACL connection lost |
| `!TAG_CONFIGURED,<esl_addr>,<mac>` | ESL address, BLE address | Tag fully configured |
| `!TAG_SYNCED,<esl_addr>` | ESL address | Tag synchronized via PAST |
| `!SENSOR_DATA,<esl_addr>,<temp>,<batt>` | ESL addr, temp (0.01°C), battery (mV) | Sensor reading from PAwR response |
| `!NUS_RSP,<esl_addr>,<cmd>,<status>,<data>` | ESL addr, NUS cmd, status, hex data | NUS command response |
| `!BASIC_STATE,<esl_addr>,<flags>` | ESL addr, state bitmap | Tag basic state update |
| `!PAWR_STARTED` | — | PAwR advertising started |
| `!PAWR_STOPPED` | — | PAwR advertising stopped |
| `!ERROR,<code>,<message>` | Error code, description | Internal error on nRF5340 |
| `!HEARTBEAT,<uptime_s>,<tags>,<mem_free>` | Uptime, active tags, free heap | Periodic health report |
| `!BOOT,<version>,<reset_reason>` | FW version, reset reason | nRF5340 boot notification |

### 5.5 ACK/NAK Format

```
+SEQ\n          ← Command accepted, will process
-SEQ,ERR_CODE,MESSAGE\n   ← Command rejected
```

Error codes:
| Code | Meaning |
|---|---|
| `E01` | Unknown command |
| `E02` | Invalid arguments |
| `E03` | Busy (previous command still executing) |
| `E04` | BLE not ready |
| `E05` | Tag not found |
| `E06` | Connection failed |
| `E07` | Timeout |
| `E08` | Internal error |
| `E09` | Out of resources (connections, memory) |

### 5.6 Complete Command Set

#### System Commands

| Command | Args | Response | Description |
|---|---|---|---|
| `PING` | — | `OK` | Check nRF5340 is alive |
| `VERSION` | — | `OK,<fw_ver>,<ncs_ver>` | Get firmware version |
| `RESET` | — | `OK` (then reboot) | Reboot nRF5340 |
| `STATUS` | — | `OK,<uptime>,<heap>,<tags>,<conns>` | System status |
| `SET_LOG_LEVEL,<level>` | 0-4 | `OK` | Set log verbosity |

#### BLE Scanning

| Command | Args | Response | Description |
|---|---|---|---|
| `BLE_SCAN_START[,<oneshot>]` | 0/1 | `OK,SCANNING` | Start BLE scan (continuous/oneshot) |
| `BLE_SCAN_STOP` | — | `OK,STOPPED` | Stop BLE scan |
| `BLE_SCAN_FILTER_UUID,<uuid>` | 16-bit UUID hex | `OK` | Set scan UUID filter |
| `BLE_SCAN_FILTER_NAME,<prefix>` | Name prefix | `OK` | Set scan name filter |
| `BLE_SCAN_LIST` | — | `OK,<count>` + `!TAG_FOUND` events | List discovered tags |

#### ACL Connection

| Command | Args | Response | Description |
|---|---|---|---|
| `BLE_CONNECT,<mac>,<type>` | BLE addr, `public`/`random` | `OK,<conn_idx>` | Connect to tag |
| `BLE_CONNECT_ESL,<esl_addr>` | ESL address (hex) | `OK,<conn_idx>` | Reconnect by ESL addr |
| `BLE_DISCONNECT,<conn_idx>` | Connection index | `OK` | Disconnect |
| `BLE_CONN_LIST` | — | `OK,<count>,<data>` | List active connections |
| `BLE_SECURITY,<conn_idx>,<level>` | Conn idx, sec level | `OK` | Set security level |

#### ESL Profile

| Command | Args | Response | Description |
|---|---|---|---|
| `ESL_DISCOVER,<conn_idx>` | Connection index | `OK` when complete | Start GATT discovery |
| `ESL_CONFIGURE,<conn_idx>,<esl_addr>` | Conn idx, ESL addr | `OK,<esl_addr>` | Full ESL configuration |
| `ESL_PAST,<conn_idx>` | Connection index | `OK` | Send PAST |
| `ESL_PING,<esl_addr>` | ESL address | `OK,<basic_state>` | Ping tag via ECP |
| `ESL_SENSOR,<esl_addr>,<idx>` | ESL addr, sensor index | `OK,<data>` | Read sensor via ECP |
| `ESL_LED,<esl_addr>,<idx>,<params>` | ESL addr, LED idx, params | `OK` | Control LED |
| `ESL_DISPLAY,<esl_addr>,<disp>,<img>` | ESL addr, display/image idx | `OK` | Control display |
| `ESL_FACTORY_RESET,<esl_addr>` | ESL address | `OK` | Factory reset tag |
| `ESL_UNASSOCIATE,<esl_addr>` | ESL address | `OK` | Unassociate tag |

#### PAwR (Periodic Advertising with Responses)

| Command | Args | Response | Description |
|---|---|---|---|
| `PAWR_START` | — | `OK` | Start PAwR advertising |
| `PAWR_STOP` | — | `OK` | Stop PAwR advertising |
| `PAWR_STATUS` | — | `OK,<running>,<groups>,<synced_tags>` | PAwR status |

#### NUS (Vendor-Specific over PAwR)

| Command | Args | Response | Description |
|---|---|---|---|
| `NUS_STATUS,<esl_addr>` | ESL address | Event: `!NUS_RSP,...` | Request tag status |
| `NUS_SENSORS,<esl_addr>` | ESL address | Event: `!NUS_RSP,...` | Request all sensors |
| `NUS_CONFIG,<esl_addr>` | ESL address | Event: `!NUS_RSP,...` | Get tag configuration |
| `NUS_RESET,<esl_addr>` | ESL address | Event: `!NUS_RSP,...` | Reset tag |
| `NUS_LED,<esl_addr>,<idx>,<mode>` | ESL addr, LED idx, 0/1/2 | Event: `!NUS_RSP,...` | Control LED via NUS |
| `NUS_UPDATE_INT,<esl_addr>,<ms>` | ESL addr, interval ms | Event: `!NUS_RSP,...` | Set update interval |

#### Tag Database

| Command | Args | Response | Description |
|---|---|---|---|
| `TAG_LIST` | — | `OK,<count>` + multi-line data | List all stored tags |
| `TAG_REMOVE,<esl_addr>` | ESL address | `OK` | Remove tag from storage |
| `TAG_CLEAR_ALL` | — | `OK` | Clear all tags |

#### Automatic Operations

| Command | Args | Response | Description |
|---|---|---|---|
| `AUTO_ONBOARD,<enable>` | 0/1 | `OK` | Enable/disable auto-onboarding |
| `SENSOR_REPORT,<enable>[,<interval_s>]` | 0/1, optional interval | `OK` | Enable auto sensor reporting |

---

## 6. nRF9151 Changes (uart_bridge Module)

### 6.1 Replace `uart_sensor` with `uart_bridge`

The current `uart_sensor` module is too simplistic. Replace it with a new `uart_bridge` module that acts as the **command orchestrator** for the nRF5340.

#### New Module Structure

```
app/src/modules/uart_bridge/
├── uart_bridge.c          # Main module: ZBUS integration, state machine
├── uart_bridge.h          # Public API and message types
├── uart_transport.c       # Low-level UART framing, TX/RX, CRC
├── uart_transport.h       # Transport layer API
├── uart_protocol.c        # Protocol encoding/decoding, seq tracking
├── uart_protocol.h        # Protocol definitions, command/response types
├── ble_manager.c          # High-level BLE operations (scan, connect, configure)
├── ble_manager.h          # BLE manager API
├── Kconfig                # Configuration options
├── shell.c                # Debug shell commands
└── README.md              # Module documentation
```

### 6.2 uart_transport Layer

Handles raw UART I/O with the following features:
- **ISR-driven RX** with ring buffer (not line buffer) — handles partial frames
- **Frame assembly** — accumulates bytes until complete frame received
- **CRC validation** — drops corrupted frames
- **TX with flow control** — respects CTS, uses interrupt-driven TX
- **Re-sync** — if corrupted, scans for next SOF (or `$` in ASCII mode)

```c
/* Transport API */
int uart_transport_init(const struct device *uart_dev);
int uart_transport_send(const uint8_t *data, size_t len);
int uart_transport_register_rx_cb(uart_transport_rx_cb_t cb);
```

### 6.3 uart_protocol Layer

Handles message encoding/decoding and command tracking:

```c
/* Protocol API */
int uart_proto_send_cmd(const char *cmd, const char *args, uint8_t *seq_out);
int uart_proto_register_rsp_cb(uart_proto_rsp_cb_t cb);
int uart_proto_register_evt_cb(uart_proto_evt_cb_t cb);

/* Sequence tracking */
struct pending_cmd {
    uint8_t seq;
    char cmd[32];
    int64_t sent_at;
    uart_proto_rsp_cb_t callback;
    void *user_data;
    bool completed;
};

#define MAX_PENDING_CMDS 8
```

### 6.4 ble_manager Layer

High-level BLE operations that orchestrate multi-step sequences:

```c
/* BLE Manager API — called from main state machine or MQTT commands */
int ble_mgr_scan_start(bool oneshot);
int ble_mgr_scan_stop(void);
int ble_mgr_connect_tag(const bt_addr_le_t *addr);
int ble_mgr_configure_tag(uint8_t conn_idx, uint16_t esl_addr);
int ble_mgr_onboard_tag(const bt_addr_le_t *addr);  /* full auto: connect→discover→configure→PAST */
int ble_mgr_read_sensor(uint16_t esl_addr, uint8_t sensor_idx);
int ble_mgr_get_tag_status(uint16_t esl_addr);
int ble_mgr_start_pawr(void);
int ble_mgr_stop_pawr(void);
int ble_mgr_enable_sensor_reporting(uint32_t interval_s);
```

### 6.5 uart_bridge Module (ZBUS Integration)

The main module subscribes to ZBUS channels and drives BLE operations:

```c
/* ZBUS channels */
ZBUS_CHAN_DEFINE(UART_BRIDGE_CHAN, struct uart_bridge_msg, ...);

/* Message types */
enum uart_bridge_msg_type {
    /* Requests (from main or MQTT) */
    UART_BRIDGE_REQ_SCAN_START,
    UART_BRIDGE_REQ_SCAN_STOP,
    UART_BRIDGE_REQ_CONNECT,
    UART_BRIDGE_REQ_CONFIGURE,
    UART_BRIDGE_REQ_ONBOARD,
    UART_BRIDGE_REQ_READ_SENSOR,
    UART_BRIDGE_REQ_TAG_STATUS,
    UART_BRIDGE_REQ_START_PAWR,
    UART_BRIDGE_REQ_STOP_PAWR,
    UART_BRIDGE_REQ_RAW_CMD,          /* Pass-through for MQTT */

    /* Responses/Events (from nRF5340) */
    UART_BRIDGE_EVT_TAG_FOUND,
    UART_BRIDGE_EVT_TAG_CONNECTED,
    UART_BRIDGE_EVT_TAG_DISCONNECTED,
    UART_BRIDGE_EVT_TAG_CONFIGURED,
    UART_BRIDGE_EVT_SENSOR_DATA,
    UART_BRIDGE_EVT_NUS_RESPONSE,
    UART_BRIDGE_EVT_ERROR,
    UART_BRIDGE_EVT_HEARTBEAT,
    UART_BRIDGE_EVT_5340_BOOT,
    UART_BRIDGE_EVT_5340_DEAD,        /* Heartbeat timeout */
};

/* Sensor data from tag (via nRF5340) */
struct tag_sensor_data {
    uint16_t esl_addr;
    char mac[18];
    float temperature;     /* °C */
    uint16_t battery_mv;
    int8_t rssi;
    int64_t timestamp;
};

/* Bridge message */
struct uart_bridge_msg {
    enum uart_bridge_msg_type type;
    union {
        struct tag_sensor_data sensor;
        struct { char mac[18]; int8_t rssi; char name[32]; } tag_found;
        struct { uint8_t conn_idx; char mac[18]; } tag_connected;
        struct { uint8_t conn_idx; uint8_t reason; } tag_disconnected;
        struct { uint16_t esl_addr; char mac[18]; } tag_configured;
        struct { uint16_t esl_addr; uint8_t cmd; uint8_t status; char data[64]; } nus_rsp;
        struct { uint8_t code; char message[64]; } error;
        struct { uint32_t uptime_s; uint8_t active_tags; uint32_t free_heap; } heartbeat;
        struct { char version[32]; uint8_t reset_reason; } boot;
        struct { char cmd[128]; } raw_cmd;
    };
};
```

### 6.6 ZBUS Wiring

```
                        ZBUS
  ┌──────────┐      ┌──────────────┐      ┌─────────────┐
  │  main.c  │─────►│ UART_BRIDGE  │─────►│ uart_bridge │──── UART1 ──► nRF5340
  │ (trigger)│      │   _CHAN       │      │   module    │
  └──────────┘      └──────────────┘      └──────┬──────┘
                                                  │
  ┌──────────┐      ┌──────────────┐              │
  │custom_   │◄─────│ UART_BRIDGE  │◄─────────────┘ (events/responses)
  │mqtt      │      │   _CHAN       │
  │(publish) │      └──────────────┘
  └──────────┘

  ┌──────────┐      ┌──────────────┐      ┌─────────────┐
  │custom_   │─────►│ CUSTOM_MQTT  │─────►│ uart_bridge │ (command forwarding)
  │mqtt      │      │   _CHAN       │      │  listener   │
  │(commands)│      └──────────────┘      └─────────────┘
  └──────────┘
```

---

## 7. nRF5340 Changes (Slave Firmware)

### 7.1 Architecture Change

The nRF5340 firmware needs significant restructuring:

**Remove:**
- Shell interface (no more `esl_c` commands)
- USB CDC ACM console dependency (optional: keep for debug logging)
- Auto-onboarding logic (moved to nRF9151)
- Tag database management policy (nRF5340 keeps a cache, nRF9151 is authoritative)
- Timer-based autonomous operations

**Keep:**
- BLE scanning engine
- ACL connection management
- ESL profile: GATT discovery, characteristic reads/writes
- PAwR advertising, subevent management, EAD encryption
- PAST procedure
- NUS over PAwR
- OTS client (image transfer)
- EAD key management
- ipc_radio net core (unchanged)

**Add:**
- UART command handler (parse incoming commands, dispatch to BLE subsystem)
- UART response/event sender (format results, send over UART)
- Heartbeat timer (periodic health report)
- Boot notification (announce firmware version on startup)
- Command queue (serialize BLE operations that can't run concurrently)

### 7.2 New nRF5340 Module Structure

```
src/
├── main.c                      # Init, heartbeat, boot announcement
├── uart_handler.c/.h           # UART RX/TX, frame parsing
├── command_dispatcher.c/.h     # Parse commands, dispatch to handlers
├── ble_controller.c/.h         # BLE scan, connect, disconnect — thin wrapper
├── esl_operations.c/.h         # ESL profile operations (discover, configure, PAST)
├── pawr_controller.c/.h        # PAwR start/stop, sync buffer management
├── nus_operations.c/.h         # NUS over PAwR commands
├── tag_cache.c/.h              # Minimal tag data cache (in-RAM, no filesystem needed)
├── event_reporter.c/.h         # Format and send events/responses over UART
└── esl_common.c/.h             # Shared ESL definitions (kept from original)
```

### 7.3 Command Dispatcher

```c
/* command_dispatcher.c */

typedef int (*cmd_handler_t)(uint8_t seq, const char *args);

struct cmd_entry {
    const char *name;
    cmd_handler_t handler;
    bool requires_ble_ready;
};

static const struct cmd_entry cmd_table[] = {
    /* System */
    { "PING",              cmd_ping,              false },
    { "VERSION",           cmd_version,           false },
    { "RESET",             cmd_reset,             false },
    { "STATUS",            cmd_status,            false },
    
    /* BLE Scanning */
    { "BLE_SCAN_START",    cmd_scan_start,        true },
    { "BLE_SCAN_STOP",     cmd_scan_stop,         true },
    { "BLE_SCAN_LIST",     cmd_scan_list,         true },
    
    /* ACL Connection */
    { "BLE_CONNECT",       cmd_connect,           true },
    { "BLE_CONNECT_ESL",   cmd_connect_esl,       true },
    { "BLE_DISCONNECT",    cmd_disconnect,         true },
    
    /* ESL Profile */
    { "ESL_DISCOVER",      cmd_esl_discover,      true },
    { "ESL_CONFIGURE",     cmd_esl_configure,     true },
    { "ESL_PAST",          cmd_esl_past,          true },
    { "ESL_PING",          cmd_esl_ping,          true },
    { "ESL_SENSOR",        cmd_esl_sensor,        true },
    { "ESL_LED",           cmd_esl_led,           true },
    { "ESL_DISPLAY",       cmd_esl_display,       true },
    
    /* PAwR */
    { "PAWR_START",        cmd_pawr_start,        true },
    { "PAWR_STOP",         cmd_pawr_stop,         true },
    { "PAWR_STATUS",       cmd_pawr_status,       true },
    
    /* NUS */
    { "NUS_STATUS",        cmd_nus_status,        true },
    { "NUS_SENSORS",       cmd_nus_sensors,       true },
    { "NUS_CONFIG",        cmd_nus_config,        true },
    { "NUS_LED",           cmd_nus_led,           true },
    
    /* Tag Management */
    { "TAG_LIST",          cmd_tag_list,          false },
    { "TAG_REMOVE",        cmd_tag_remove,        false },
    { "TAG_CLEAR_ALL",     cmd_tag_clear_all,     false },
    
    /* Automation */
    { "AUTO_ONBOARD",      cmd_auto_onboard,      true },
    { "SENSOR_REPORT",     cmd_sensor_report,     true },
};
```

### 7.4 Event Reporter

```c
/* event_reporter.c */

/* Send a response to a command */
void evt_send_response(uint8_t seq, const char *status, const char *data);
/* Example: evt_send_response(0x03, "OK", "CONNECTED,0") → "@03,OK,CONNECTED,0\n" */

/* Send an unsolicited event */
void evt_send_event(const char *event_name, const char *data);
/* Example: evt_send_event("TAG_FOUND", "E9:76:EE:E9:06:5F,-45,ESL_Tag_01") */
/*        → "!TAG_FOUND,E9:76:EE:E9:06:5F,-45,ESL_Tag_01\n" */

/* Send heartbeat */
void evt_send_heartbeat(uint32_t uptime_s, uint8_t active_tags, uint32_t free_heap);
/* → "!HEARTBEAT,3936,5,12480\n" */

/* Send boot notification */
void evt_send_boot(const char *fw_version, uint8_t reset_reason);
/* → "!BOOT,1.0.0,0\n" */
```

---

## 8. State Machine Design

### 8.1 nRF9151 uart_bridge State Machine (SMF)

```
STATE_BRIDGE_RUNNING (root ancestor)
│
├── STATE_BRIDGE_INIT
│   Entry: configure UART, wait 500ms, send PING
│   Transition → SYNCING when UART ready
│
├── STATE_BRIDGE_SYNCING
│   Entry: send PING, start 5s timeout
│   On @xx,OK → transition to IDLE
│   On timeout → retry PING (max 3 retries) or → ERROR
│
├── STATE_BRIDGE_IDLE
│   Entry: heartbeat monitoring active
│   On !HEARTBEAT → reset watchdog, update stats
│   On heartbeat timeout (30s) → transition to ERROR
│   On ZBUS request → transition to appropriate sub-state
│
├── STATE_BRIDGE_SCANNING
│   Entry: send BLE_SCAN_START
│   On !TAG_FOUND → publish to ZBUS
│   On BLE_SCAN_STOP request → send BLE_SCAN_STOP → IDLE
│   On timeout → auto-stop → IDLE
│
├── STATE_BRIDGE_ONBOARDING
│   Entry: orchestrate full tag onboarding sequence
│   Sub-states:
│   ├── ONBOARD_CONNECTING     → send BLE_CONNECT
│   ├── ONBOARD_DISCOVERING    → send ESL_DISCOVER
│   ├── ONBOARD_CONFIGURING    → send ESL_CONFIGURE
│   ├── ONBOARD_SYNCING        → send ESL_PAST
│   └── ONBOARD_COMPLETE       → publish result → IDLE
│   On error at any step → cleanup → publish error → IDLE
│
├── STATE_BRIDGE_COLLECTING
│   Entry: send sensor/status requests to tags
│   On !SENSOR_DATA → accumulate
│   On !NUS_RSP → accumulate
│   On all data collected or timeout → publish batch → IDLE
│
└── STATE_BRIDGE_ERROR
    Entry: log error, attempt recovery
    On UART recovery → SYNCING
    On max retries exceeded → publish fatal error, stay in ERROR
    On manual reset → INIT
```

### 8.2 nRF5340 State Machine (simplified, no SMF — plain switch/case)

```
STATE_BOOT
│ → initialize UART, BLE, send !BOOT event
│ → transition to READY
│
STATE_READY
│ → listening for commands on UART
│ → heartbeat timer sends !HEARTBEAT every 15s
│ → BLE callbacks fire events (!TAG_FOUND, !TAG_DISCONNECTED, etc.)
│
│ On command received:
│   ├── Simple command (PING, STATUS, VERSION) → execute immediately, send @RSP
│   ├── BLE command (SCAN, CONNECT, etc.) → queue if busy, execute, send @RSP + !EVTs
│   └── Multi-step command (ESL_CONFIGURE) → process steps, send progress events + final @RSP
│
STATE_ERROR
│ → serious failure (BLE init failed, UART error)
│ → send !ERROR event
│ → attempt recovery or wait for RESET command
```

---

## 9. Error Handling & Recovery

### 9.1 Timeout Matrix

| Operation | Timeout | Retries | Recovery |
|---|---|---|---|
| PING | 2s | 3 | UART re-init |
| BLE_SCAN_START | 5s | 1 | Log error |
| BLE_CONNECT | 10s | 2 | Next tag |
| ESL_DISCOVER | 15s | 1 | Disconnect + retry |
| ESL_CONFIGURE | 20s | 1 | Disconnect + retry |
| ESL_PAST | 10s | 3 | Disconnect (avoid k_oops) |
| NUS_STATUS | 5s | 2 | Skip tag |
| Heartbeat | 30s | — | Enter ERROR, try RESET |
| MQTT publish | 10s | 3 | Exponential backoff |

### 9.2 nRF9151 Recovery Strategy

```
Heartbeat lost for 30s
        │
        ▼
 Send PING (3 retries, 2s each)
        │
   ┌────┴────┐
   │ Success  │ → Resume normal operation
   └─────────┘
        │ Failure
        ▼
 Toggle nRF5340 RESET GPIO (if available)
 OR send UART break
        │
        ▼
 Wait 2s, retry PING
        │
   ┌────┴────┐
   │ Success  │ → Re-initialize (SYNCING state)
   └─────────┘
        │ Failure
        ▼
 Log fatal error, report to cloud
 Enter degraded mode (nRF9151-only sensors)
```

### 9.3 Command Queue (nRF5340 side)

BLE operations often can't run concurrently (e.g., scan + connect). The nRF5340 maintains a simple command queue:

```c
#define CMD_QUEUE_SIZE 8

struct queued_cmd {
    uint8_t seq;
    char cmd[32];
    char args[96];
};

static struct queued_cmd cmd_queue[CMD_QUEUE_SIZE];
static K_MSGQ_DEFINE(cmd_msgq, sizeof(struct queued_cmd), CMD_QUEUE_SIZE, 4);

/* If a command arrives while busy, queue it and send +SEQ (ACK) */
/* When current operation completes, dequeue and execute next */
```

---

## 10. Data Flow Diagrams

### 10.1 Full Onboarding Flow (nRF9151 driven)

```
  nRF9151 (master)                    nRF5340 (slave)                ESL Tag
       │                                    │                           │
       │  $01,BLE_SCAN_START,1              │                           │
       │───────────────────────────────────►│                           │
       │  @01,OK,SCANNING                   │                           │
       │◄───────────────────────────────────│                           │
       │                                    │   ◄── BLE Adv ──────────│
       │  !TAG_FOUND,E9:76:..,−45,Tag01    │                           │
       │◄───────────────────────────────────│                           │
       │                                    │                           │
       │  $02,BLE_CONNECT,E9:76:..,random   │                           │
       │───────────────────────────────────►│                           │
       │  +02                               │                           │
       │◄───────────────────────────────────│  ── Connection Req ─────►│
       │                                    │  ◄─ Connection Rsp ──────│
       │  !TAG_CONNECTED,0,E9:76:..         │                           │
       │◄───────────────────────────────────│                           │
       │  @02,OK,CONNECTED,0                │                           │
       │◄───────────────────────────────────│                           │
       │                                    │                           │
       │  $03,ESL_DISCOVER,0                │                           │
       │───────────────────────────────────►│                           │
       │  +03                               │  ── GATT Discovery ─────►│
       │◄───────────────────────────────────│  ◄─ Attributes ─────────│
       │  @03,OK                            │                           │
       │◄───────────────────────────────────│                           │
       │                                    │                           │
       │  $04,ESL_CONFIGURE,0,0x0000        │                           │
       │───────────────────────────────────►│                           │
       │  +04                               │  ── Write ESL addr ─────►│
       │◄───────────────────────────────────│  ── Write AP key ───────►│
       │                                    │  ── Write RSP key ──────►│
       │                                    │  ── Write abs time ─────►│
       │  !TAG_CONFIGURED,0x0000,E9:76:..   │                           │
       │◄───────────────────────────────────│                           │
       │  @04,OK,0x0000                     │                           │
       │◄───────────────────────────────────│                           │
       │                                    │                           │
       │  $05,ESL_PAST,0                    │                           │
       │───────────────────────────────────►│                           │
       │  +05                               │  ── PAST ───────────────►│
       │◄───────────────────────────────────│                           │
       │  !TAG_SYNCED,0x0000                │                           │
       │◄───────────────────────────────────│                           │
       │  @05,OK                            │                           │
       │◄───────────────────────────────────│                           │
       │                                    │                           │
       │  $06,BLE_DISCONNECT,0              │                           │
       │───────────────────────────────────►│  ── Disconnect ─────────►│
       │  @06,OK                            │                           │
       │◄───────────────────────────────────│                           │
```

### 10.2 Sensor Data Collection (Steady State)

```
  nRF9151                             nRF5340                    ESL Tags (synced)
       │                                    │                           │
       │  $10,NUS_STATUS,0x0000             │                           │
       │───────────────────────────────────►│                           │
       │  +10                               │  ── PAwR subevent ──────►│
       │◄───────────────────────────────────│  ◄─ EAD response ───────│
       │  !NUS_RSP,0x0000,0x01,OK,          │                           │
       │    UP=3936,BATT=4134,FLAGS=0x01    │                           │
       │◄───────────────────────────────────│                           │
       │  @10,OK                            │                           │
       │◄───────────────────────────────────│                           │
       │                                    │                           │
       │     (nRF9151 publishes to MQTT)    │                           │
       │                                    │                           │
       │  $11,ESL_SENSOR,0x0000,0           │                           │
       │───────────────────────────────────►│                           │
       │  +11                               │  ── PAwR sensor req ────►│
       │◄───────────────────────────────────│  ◄─ Sensor response ────│
       │  !SENSOR_DATA,0x0000,2350,4134     │  (temp=23.50°C, bat)     │
       │◄───────────────────────────────────│                           │
       │  @11,OK                            │                           │
       │◄───────────────────────────────────│                           │
```

### 10.3 MQTT → BLE Passthrough

```
  MQTT Broker        nRF9151                           nRF5340
       │                    │                                │
       │ {"type":"uart_passthrough",                         │
       │  "command":"NUS_STATUS,0x0000"}                     │
       │──────────────────►│                                │
       │                    │  $xx,NUS_STATUS,0x0000         │
       │                    │──────────────────────────────►│
       │                    │  !NUS_RSP,...                   │
       │                    │◄──────────────────────────────│
       │                    │                                │
       │  {"type":"nus_response",                            │
       │   "esl_addr":"0x0000",                              │
       │   "data":{...}}                                     │
       │◄──────────────────│                                │
```

---

## 11. Implementation Plan

### Phase 1: Transport & Basic Communication (Week 1-2)

**nRF9151 side:**
1. Create `uart_bridge` module skeleton with ZBUS integration
2. Implement `uart_transport.c` — ISR RX with ring buffer, TX, frame assembly
3. Implement `uart_protocol.c` — sequence tracking, send CMD, parse RSP/EVT
4. Implement PING/VERSION/STATUS commands
5. Add heartbeat monitoring (check for `!HEARTBEAT` events)
6. Wire `uart_bridge` into main.c ZBUS subscriptions
7. Keep `uart_sensor` as fallback (compile-time switch)

**nRF5340 side:**
1. Create `uart_handler.c` — UART1 RX/TX for inter-chip communication
2. Create `command_dispatcher.c` — parse `$SEQ,CMD,ARGS` format
3. Create `event_reporter.c` — send `@SEQ,STATUS,DATA` and `!EVENT,DATA`
4. Implement PING, VERSION, STATUS handlers
5. Add heartbeat timer (15s interval)
6. Add boot notification (`!BOOT,...`)

**Test:** nRF9151 boots → sends PING → nRF5340 responds → heartbeat flows

### Phase 2: BLE Scanning & Connection (Week 2-3)

**nRF9151 side:**
1. Implement `ble_manager.c` — `ble_mgr_scan_start/stop`, `ble_mgr_connect_tag`
2. Handle `!TAG_FOUND` events — parse and publish to ZBUS
3. Handle `!TAG_CONNECTED` / `!TAG_DISCONNECTED` events
4. Wire scan trigger to button press and MQTT commands
5. Update `custom_mqtt` to publish tag discovery events

**nRF5340 side:**
1. Implement `ble_controller.c` — wrap existing scan/connect code
2. Implement `BLE_SCAN_START`, `BLE_SCAN_STOP`, `BLE_SCAN_LIST` handlers
3. Implement `BLE_CONNECT`, `BLE_DISCONNECT` handlers
4. Fire `!TAG_FOUND`, `!TAG_CONNECTED`, `!TAG_DISCONNECTED` events
5. Remove shell dependency for scan/connect (drive from UART commands)

**Test:** nRF9151 sends SCAN_START → tags found → nRF9151 sends CONNECT → connected

### Phase 3: ESL Profile & PAwR (Week 3-4)

**nRF9151 side:**
1. Implement ESL command wrappers in `ble_manager.c`
2. Implement full onboarding sequence (scan→connect→discover→configure→PAST→disconnect)
3. Handle `!TAG_CONFIGURED`, `!TAG_SYNCED` events
4. Implement `PAWR_START/STOP` sequence

**nRF5340 side:**
1. Implement `esl_operations.c` — wrap existing ESL profile code
2. Implement `pawr_controller.c` — wrap existing PAwR code
3. Implement all ESL command handlers
4. Fire configuration/sync events

**Test:** Full onboarding of ESL tag driven from nRF9151

### Phase 4: NUS & Sensor Data (Week 4-5)

**nRF9151 side:**
1. Implement NUS command wrappers
2. Implement `SENSOR_REPORT` to enable periodic auto-reporting on nRF5340
3. Handle `!SENSOR_DATA` and `!NUS_RSP` events
4. Publish sensor data to MQTT with proper JSON formatting
5. Implement data aggregation (batch multiple tag readings)

**nRF5340 side:**
1. Implement `nus_operations.c` — wrap existing NUS-over-PAwR code
2. Implement NUS command handlers
3. Implement `SENSOR_REPORT` (periodic sensor polling driven by command, reports via events)

**Test:** Periodic sensor data flowing: Tag → nRF5340 (PAwR) → nRF9151 (UART) → MQTT

### Phase 5: Error Handling, Recovery & Hardening (Week 5-6)

1. Implement timeout/retry logic for all commands
2. Implement heartbeat-based nRF5340 failure detection
3. Implement UART recovery (re-init on consecutive errors)
4. Implement command queue on nRF5340 (handle concurrent requests)
5. Add CRC to frame transport (if not done in Phase 1)
6. Stress testing: rapid commands, disconnect during operation, power cycling
7. Memory leak analysis (heap monitor on both chips)

### Phase 6: MQTT Integration & Production Polish (Week 6-7)

1. Update `custom_mqtt` to handle all new BLE event types
2. Implement proper JSON serialization for all tag data
3. Add MQTT command handlers for all BLE operations
4. Implement batch data publishing (efficient cloud updates)
5. Update documentation
6. Final integration testing with real ESL tags

---

## 12. Docker Build Integration

### Current Setup

The project uses Docker for building. The existing [flash_docker.sh](../flash_docker.sh) uses image `ncs-v2.6.1-debian`.

### Two-Chip Build Challenge

Since nRF5340 and nRF9151 are separate build targets with potentially different NCS versions:

| Chip | NCS Version | Build Target | Docker Image |
|---|---|---|---|
| nRF9151 | v3.0.2 | `thingy91x/nrf9151/ns` | `ncs-v3.0.2-debian` |
| nRF5340 | v2.8.0 | `thingy91x/nrf5340/cpuapp` | `ncs-v2.8.0-debian` |

### Recommended Build Script

```bash
#!/bin/bash
# build_all.sh — Build both nRF9151 and nRF5340 firmware

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NRF91_DIR="$SCRIPT_DIR/app"
NRF53_DIR="$SCRIPT_DIR/nrf5340_firmware"  # New directory for nRF5340 code

# --- Build nRF9151 ---
echo "=== Building nRF9151 firmware ==="
docker run --rm \
    -v "$NRF91_DIR":/workspace/app \
    -w /workspace/app \
    ncs-v3.0.2-debian \
    west build -b thingy91x/nrf9151/ns --pristine

# --- Build nRF5340 ---
echo "=== Building nRF5340 firmware ==="
docker run --rm \
    -v "$NRF53_DIR":/workspace/app \
    -w /workspace/app \
    ncs-v2.8.0-debian \
    west build -b thingy91x/nrf5340/cpuapp --pristine -- \
        -DCONFIG_MCUBOOT=y

# --- Flash both ---
echo "=== Flashing nRF9151 ==="
docker run --rm --privileged \
    -v /dev:/dev \
    -v "$NRF91_DIR":/workspace/app \
    -w /workspace/app \
    ncs-v3.0.2-debian \
    west flash --recover

echo "=== Flashing nRF5340 ==="
docker run --rm --privileged \
    -v /dev:/dev \
    -v "$NRF53_DIR":/workspace/app \
    -w /workspace/app \
    ncs-v2.8.0-debian \
    west flash --recover
```

### Future: Unified NCS Version

Long-term, port the nRF5340 firmware to NCS v3.0.2 and use sysbuild to build both chips in a single `west build` invocation. This requires:
1. Porting ESL AP from NCS v2.8.0 → v3.0.2 API changes
2. Creating a sysbuild configuration that includes the nRF5340 as a companion image
3. Defining the nRF5340 firmware as a `sysbuild` extra image

---

## 13. Testing Strategy

### 13.1 Unit Tests (Host-based)

Test protocol encoding/decoding on `native_sim`:

```
tests/
├── uart_protocol/      # Protocol encode/decode tests
│   ├── test_cmd_format.c
│   ├── test_rsp_parse.c
│   ├── test_evt_parse.c
│   ├── test_crc.c
│   └── test_seq_tracking.c
├── ble_manager/        # BLE manager state machine tests
│   ├── test_onboarding_flow.c
│   └── test_error_recovery.c
└── uart_transport/     # Transport layer tests
    ├── test_frame_assembly.c
    └── test_escape_sequences.c
```

### 13.2 Integration Tests (Two-board)

Use UART loopback or two real boards:

| Test | Description | Pass Criteria |
|---|---|---|
| Boot sync | Both chips boot, nRF9151 receives !BOOT and heartbeat | PING succeeds within 5s |
| Scan | nRF9151 triggers scan, receives tag events | At least 1 !TAG_FOUND received |
| Connect | Full connect + discover + configure + PAST | !TAG_SYNCED received |
| Sensor | Read sensor data via NUS | Valid temperature/battery data |
| Recovery | Kill nRF5340 power, verify nRF9151 detects and recovers | Heartbeat timeout → ERROR → recovery |
| Stress | Send 100 commands in rapid succession | No lost responses, no crashes |
| Long-run | 24h continuous operation with periodic sensor reads | No memory leaks, stable heartbeat |

### 13.3 MQTT End-to-End

| Test | Description |
|---|---|
| Cloud → Tag | Send MQTT command → nRF9151 → nRF5340 → ESL tag → response → MQTT |
| Tag → Cloud | Sensor auto-report → PAwR → nRF5340 → UART → nRF9151 → MQTT |
| Batch publish | Collect data from 10 tags, publish as single MQTT message |

---

## Appendix A: Pin Quick Reference (Thingy:91x)

```
nRF9151 UART1          nRF5340 UART1
─────────────          ─────────────
P0.05  TX  ──────────► P1.06  RX
P0.04  RX  ◄────────── P1.08  TX
P0.23  RTS ──────────► P0.31  CTS
P0.22  CTS ◄────────── P0.30  RTS
```

## Appendix B: NCS Version Compatibility Notes

| Feature | NCS v2.8.0 (nRF5340) | NCS v3.0.2 (nRF9151) |
|---|---|---|
| Zephyr | v3.7.99 | v4.0.99 |
| BLE PAwR | Supported | N/A (no BLE on nRF9151) |
| SMF | Supported | Supported |
| ZBUS | Supported | Supported |
| UART API | Same (interrupt-driven) | Same (interrupt-driven) |
| Sysbuild | Supported | Supported |
| MCUboot | Supported | Supported |

## Appendix C: Migration Checklist

### nRF9151 (Asset-Tracker-Template)

- [ ] Create `uart_bridge` module directory structure
- [ ] Implement `uart_transport.c` (ISR RX, ring buffer, frame assembly)
- [ ] Implement `uart_protocol.c` (sequence tracking, CMD/RSP/EVT handling)
- [ ] Implement `ble_manager.c` (high-level BLE operation orchestration)
- [ ] Implement `uart_bridge.c` (ZBUS integration, SMF state machine)
- [ ] Add Kconfig for uart_bridge module
- [ ] Update `app/Kconfig` to source uart_bridge Kconfig
- [ ] Update `app/CMakeLists.txt` to build uart_bridge sources
- [ ] Update `main.c` to subscribe to `UART_BRIDGE_CHAN`
- [ ] Update `custom_mqtt.c` to handle new event types
- [ ] Update `custom_mqtt.c` UART forwarding to use uart_bridge protocol
- [ ] Add devicetree overlay changes (if any pin changes needed)
- [ ] Add prj.conf entries for uart_bridge
- [ ] Update `thingy91x_nrf9151_ns.conf` board config
- [ ] Remove or conditionally compile old `uart_sensor` module
- [ ] Add shell commands for uart_bridge debugging
- [ ] Write unit tests for protocol layer
- [ ] Write integration test stubs

### nRF5340 (central_esl → esl_slave)

- [ ] Create UART handler module (`uart_handler.c/.h`)
- [ ] Create command dispatcher (`command_dispatcher.c/.h`)
- [ ] Create event reporter (`event_reporter.c/.h`)
- [ ] Refactor BLE scan code into `ble_controller.c`
- [ ] Refactor ESL profile code into `esl_operations.c`
- [ ] Refactor PAwR code into `pawr_controller.c`
- [ ] Refactor NUS code into `nus_operations.c`
- [ ] Add UART1 to device tree overlay (already present)
- [ ] Enable UART1 interrupt-driven in prj.conf
- [ ] Add heartbeat timer
- [ ] Add boot notification
- [ ] Add command queue for serializing BLE operations
- [ ] Remove shell command dependency for core operations
- [ ] Keep shell as optional debug interface (compile-time)
- [ ] Test all command handlers individually
- [ ] Integration test with nRF9151

---

## Appendix D: Example MQTT JSON Payloads (Updated)

### Tag Discovery Event
```json
{
  "type": "tag_found",
  "timestamp": 1739912345,
  "mac": "E9:76:EE:E9:06:5F",
  "rssi": -45,
  "name": "ESL_Tag_01"
}
```

### Tag Sensor Data
```json
{
  "type": "tag_sensor_data",
  "timestamp": 1739912400,
  "esl_addr": "0x0000",
  "mac": "E9:76:EE:E9:06:5F",
  "temperature": 23.50,
  "battery_mv": 4134,
  "flags": "0x01"
}
```

### Tag Status (NUS Response)
```json
{
  "type": "tag_status",
  "timestamp": 1739912500,
  "esl_addr": "0x0000",
  "uptime_s": 3936,
  "battery_mv": 4134,
  "flags": "0x01"
}
```

### Gateway Status
```json
{
  "type": "gateway_status",
  "timestamp": 1739912600,
  "nrf9151": {
    "uptime_s": 7200,
    "lte_connected": true,
    "signal_dbm": -85,
    "mqtt_connected": true,
    "heap_free": 45000
  },
  "nrf5340": {
    "uptime_s": 3936,
    "active_tags": 5,
    "heap_free": 12480,
    "pawr_running": true,
    "last_heartbeat_ago_s": 12
  }
}
```

### MQTT Command → BLE Action
```json
{
  "type": "ble_command",
  "command": "NUS_STATUS",
  "args": "0x0000"
}
```

### Batch Sensor Report
```json
{
  "type": "sensor_batch",
  "timestamp": 1739912700,
  "gateway_id": "thingy91x-001",
  "tags": [
    {
      "esl_addr": "0x0000",
      "mac": "E9:76:EE:E9:06:5F",
      "temperature": 23.50,
      "battery_mv": 4134,
      "last_seen_s": 30
    },
    {
      "esl_addr": "0x0001",
      "mac": "D4:22:AB:CD:EF:01",
      "temperature": 21.80,
      "battery_mv": 3900,
      "last_seen_s": 45
    }
  ]
}
```
