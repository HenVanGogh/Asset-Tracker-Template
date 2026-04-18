# nRF5340 ↔ nRF9151 Integration Guide

This document describes the nRF5340 implementation and defines the interface requirements for nRF9151 firmware to ensure proper inter-chip communication.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         HOST PC                                  │
│   ┌──────────────┐                                              │
│   │  /dev/ttyACM0 │ ← USB CDC ACM (Console + MCUmgr SMP)        │
│   └───────┬──────┘                                              │
└───────────┼─────────────────────────────────────────────────────┘
            │ USB
┌───────────┼─────────────────────────────────────────────────────┐
│      THINGY:91 X                                                 │
│           │                                                      │
│  ┌────────┴────────────────────────────────────────┐            │
│  │              nRF5340 (App Core)                 │            │
│  │                                                  │            │
│  │   USB CDC ACM ──┬── Console (Logs)              │            │
│  │                 └── MCUmgr SMP (DFU)            │            │
│  │                                                  │            │
│  │   UART1 ─────────── Inter-chip Communication   │            │
│  │   (TX: P1.8, RX: P1.6, RTS: P0.30, CTS: P0.31) │            │
│  └────────┬────────────────────────────────────────┘            │
│           │ UART (Hardware Flow Control)                         │
│  ┌────────┴────────────────────────────────────────┐            │
│  │              nRF9151                            │            │
│  │   UART0 ─────────── Inter-chip Communication   │            │
│  └─────────────────────────────────────────────────┘            │
└─────────────────────────────────────────────────────────────────┘
```

---

## nRF5340 Implementation Details

### USB Configuration

The nRF5340 exposes **one USB CDC ACM port** to the host PC:
- **VID:** `0x1915` (Nordic Semiconductor)
- **PID:** `0x5310`
- **Product Name:** "Thingy91x Scanner"

This single port is used for:
1. **Console logging** (human-readable output)
2. **MCUmgr SMP** (firmware updates for nRF5340)

> **Note:** The current implementation does NOT bridge USB to nRF9151. The nRF9151 is NOT accessible via USB without additional bridging firmware.

---

### UART1 Configuration (nRF5340 → nRF9151)

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Peripheral** | UART1 | NOT UART0 |
| **TX Pin** | P1.8 | nRF5340 output → nRF9151 input |
| **RX Pin** | P1.6 | nRF5340 input ← nRF9151 output |
| **RTS Pin** | P0.30 | Hardware flow control |
| **CTS Pin** | P0.31 | Hardware flow control |
| **Baud Rate** | 115200 | Default speed |
| **Data Bits** | 8 | |
| **Parity** | None | |
| **Stop Bits** | 1 | |
| **Flow Control** | **Hardware (RTS/CTS)** | **CRITICAL** |

### Device Tree (nRF5340 - app.overlay)

```dts
&uart1 {
    status = "okay";
    pinctrl-0 = <&uart1_default>;
    pinctrl-1 = <&uart1_sleep>;
    pinctrl-names = "default", "sleep";
    hw-flow-control;
};

/* Pin configuration */
&pinctrl {
    uart1_default: uart1_default {
        group1 {
            psels = <NRF_PSEL(UART_TX, 1, 8)>,
                    <NRF_PSEL(UART_RTS, 0, 30)>,
                    <NRF_PSEL(UART_CTS, 0, 31)>;
        };
        group2 {
            psels = <NRF_PSEL(UART_RX, 1, 6)>;
            bias-pull-up;
        };
    };
};
```

---

## nRF9151 Requirements

### UART Configuration

The nRF9151 MUST use **matching UART settings**:

```dts
/* nRF9151 Device Tree */
&uart0 {
    status = "okay";
    current-speed = <115200>;
    hw-flow-control;
    pinctrl-0 = <&uart0_default>;
    pinctrl-1 = <&uart0_sleep>;
    pinctrl-names = "default", "sleep";
};
```

### Kconfig (prj.conf)

```kconfig
# UART for communication with nRF5340
CONFIG_SERIAL=y
CONFIG_UART_ASYNC_API=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_RING_BUFFER=y
```

---

## Communication Protocol

### Command Format (nRF9151 → nRF5340)

Commands are sent as **newline-terminated ASCII strings**:

```
COMMAND_NAME:ARG1,ARG2\n
```

| Field | Description |
|-------|-------------|
| `COMMAND_NAME` | Command identifier |
| `:` | Separator |
| `ARGn` | Optional comma-separated arguments |
| `\n` | **Required** terminator (0x0A) |

**Examples:**
```
SCAN_START\n
GET_SENSORS\n
SET_INTERVAL:300\n
NUS_SEND:AA:BB:CC:DD:EE:FF,hello\n
```

### Response Format (nRF5340 → nRF9151)

Responses are also newline-terminated:

```
OK:data\n
ERR:error_message\n
```

### Data Reporting (nRF5340 → nRF9151)

Sensor data is sent as JSON:

```json
{"type":"sensor","mac":"AA:BB:CC:DD:EE:FF","temp":23.5,"hum":45.2,"bat":3300}\n
```

---

## Timing Requirements

| Event | Timing | Notes |
|-------|--------|-------|
| **Power-on delay** | 500ms | nRF9151 should wait before sending commands |
| **Command timeout** | 5000ms | Max wait for response |
| **Inter-command gap** | 10ms | Minimum between commands |
| **Flow control** | Required | CTS must be respected |

---

## Pin Mapping Reference

### Thingy91x Board Connections

| nRF5340 Pin | Signal | nRF9151 Pin | Direction |
|-------------|--------|-------------|-----------|
| P1.8 | TX | UART0_RX | 5340 → 9151 |
| P1.6 | RX | UART0_TX | 9151 → 5340 |
| P0.30 | RTS | UART0_CTS | 5340 → 9151 |
| P0.31 | CTS | UART0_RTS | 9151 → 5340 |

> **Important:** The nRF9151 MUST configure its UART pins to match the Thingy91x hardware routing. Check the Thingy91x schematic for exact pin assignments on the nRF9151 side.

---

## Code Reference (nRF5340)

### UART Handler ([uart_handler.c](file:///home/mic/ncs/v3.0.2/nrf/samples/net/scan_adv/src/uart_handler.c))

```c
/* RX buffer with ring buffer */
#define RING_BUF_SIZE 256
static uint8_t ring_buffer[RING_BUF_SIZE];
struct ring_buf rx_ring_buf;

/* Get a complete command (newline-terminated) */
bool uart_get_command(char *buf, size_t len);

/* Send data to nRF9151 */
int uart_handler_send(const char *data);
```

### Command Parser ([command_parser.c](file:///home/mic/ncs/v3.0.2/nrf/samples/net/scan_adv/src/command_parser.c))

Parses commands received from nRF9151 and executes actions.

---

## Testing Procedure

### On nRF5340 side (already implemented):
1. USB logs appear on `/dev/ttyACM0`
2. MCUmgr DFU works via USB
3. UART1 ready for nRF9151 communication

### On nRF9151 side (to be implemented):
1. Configure UART0 with hardware flow control
2. Wait 500ms after boot before sending commands
3. Send test command: `GET_STATUS\n`
4. Verify response received

---

## Checklist for nRF9151 Implementation

- [ ] UART0 configured at 115200 baud
- [ ] Hardware flow control (RTS/CTS) enabled
- [ ] RX uses interrupt or async API with ring buffer
- [ ] Commands terminated with `\n`
- [ ] 500ms delay after boot before first command
- [ ] CTS line respected before transmitting
- [ ] Response timeout handling (5s recommended)
