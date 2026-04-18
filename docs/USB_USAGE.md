# USB Flashing and Logging for Thingy:91 X

This document explains how to use USB for firmware flashing and log viewing on the Thingy:91 X platform with this project.

## Architecture Overview

The Thingy:91 X uses a dual-chip architecture where USB functionality is provided through the nRF5340:

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Thingy:91 X                                 │
│  ┌───────────────────┐           ┌───────────────────┐              │
│  │     nRF5340       │           │     nRF9151       │              │
│  │  (Board Controller)│           │ (Main Application)│              │
│  │                   │           │                   │              │
│  │  USB Device Stack │           │  Your Application │              │
│  │       ↓           │  UART0    │        ↓          │              │
│  │  CDC ACM 0 ◄──────┼───────────┼──► Console/Logs   │              │
│  │  CDC ACM 1 ◄──────┼───UART1───┼──► Data/Sensors   │              │
│  │       ↓           │           │        ↓          │              │
│  │  Connectivity     │  GPIO     │  MCUboot          │              │
│  │  Bridge Firmware  │───P0.24───┼──► Reset Control  │              │
│  └────────┬──────────┘           └───────────────────┘              │
│           │                                                          │
└───────────┼──────────────────────────────────────────────────────────┘
            │
        USB-C Port
            │
    ┌───────┴───────┐
    │ Host Computer │
    │  - Serial     │
    │  - nrfutil    │
    │  - mcumgr     │
    └───────────────┘
```

## USB Port Identification

When connected via USB, the Thingy:91 X exposes **two virtual serial ports**:

| Port | nRF9151 UART | Purpose | Baud Rate |
|------|--------------|---------|-----------|
| CDC ACM 0 (lower #) | UART0 | Logging + MCUboot Flashing | 115200 (logs), 1Mbps (DFU) |
| CDC ACM 1 (higher #) | UART1 | Inter-chip data (uart_sensor) | 115200 |

### Linux
```bash
# List connected ports
ls /dev/ttyACM*
# Usually: /dev/ttyACM0 (logs) and /dev/ttyACM1 (data)
```

### Windows
Look in Device Manager → Ports (COM & LPT) for two "nRF USB" or "Connectivity Bridge" ports.

### macOS
```bash
ls /dev/tty.usbmodem*
```

---

## Viewing Logs via USB

Connect to the **first CDC ACM port** (lower number) at 115200 baud:

### Using `screen` (Linux/macOS)
```bash
screen /dev/ttyACM0 115200
# To exit: Ctrl+A, then K, then Y
```

### Using `minicom`
```bash
minicom -D /dev/ttyACM0 -b 115200
```

### Using nRF Connect for Desktop
1. Open **nRF Connect for Desktop** → **Serial Terminal**
2. Select the CDC ACM 0 port
3. Set baud rate to 115200
4. Click **Connect**

### Using PuTTY (Windows)
1. Select "Serial" connection type
2. Enter COM port (e.g., COM3)
3. Set Speed to 115200
4. Click Open

---

## Flashing Firmware via USB

### Method 1: MCUboot Serial Recovery (Recommended)

This method allows flashing even if the application is corrupted.

#### Enter Recovery Mode
1. **Hold Button 1** (SW3 on the board)
2. **Press Reset** (SW4) or power cycle the device
3. Release Button 1 after 1 second

#### Flash with nrfutil
```bash
# Flash the application update image
nrfutil device program --firmware build/app/zephyr/app_update.bin \
    --serial-port /dev/ttyACM0 \
    --serial-number auto \
    --x-family NRF91 \
    --x-mcuboot-interface serial \
    --x-mcuboot-protocol mcumgr \
    --x-mcuboot-baudrate 1000000
```

#### Flash with mcumgr
```bash
# Create connection profile
mcumgr conn add thingy91x type="serial" connstring="dev=/dev/ttyACM0,baud=1000000"

# Upload image
mcumgr --conn thingy91x image upload build/app/zephyr/app_update.bin

# Confirm the image (makes it permanent)
mcumgr --conn thingy91x image confirm

# Reset to boot new image
mcumgr --conn thingy91x reset
```

### Method 2: nRF Connect Programmer (GUI)

1. Open **nRF Connect for Desktop** → **Programmer**
2. Connect device and select it
3. Add the `.hex` file from `build/app/zephyr/merged.hex`
4. Click **Write**

---

## nRF5340 Connectivity Bridge Requirements

> **IMPORTANT**: USB functionality depends on the nRF5340 running the Connectivity Bridge firmware. This is pre-programmed on new Thingy:91 X devices.

### Checking if Connectivity Bridge is Running

If you connect the device via USB and see two serial ports enumerated, the Connectivity Bridge is working.

If **no ports appear**, you may need to flash the Connectivity Bridge firmware to the nRF5340.

### Flashing Connectivity Bridge (Recovery)

You need an external J-Link debugger connected to the debug header (P8):

```bash
# Build and flash connectivity bridge
cd $NCS_ROOT/nrf/applications/connectivity_bridge
west build -b thingy91x/nrf5340/cpuapp --pristine
west flash --recover
```

### nRF5340 Requirements for USB Functionality

For developers implementing custom nRF5340 firmware and wanting to maintain USB bridging:

| Requirement | Description |
|-------------|-------------|
| **USB Device Stack** | `CONFIG_USB_DEVICE_STACK=y` |
| **USB CDC ACM** | `CONFIG_USB_CDC_ACM=y` (2 instances for dual port) |
| **UART to nRF9151** | Configure UART instances matching nRF9151 pins |
| **UART0 Bridge** | P1.05 (TX) → nRF9151 RX, P1.04 (RX) ← nRF9151 TX |
| **UART1 Bridge** | P1.01 (TX) → nRF9151 RX, P1.00 (RX) ← nRF9151 TX |
| **Flow Control** | UART1 requires HW flow control (RTS/CTS) for reliability |
| **Reset Control** | GPIO P0.24 controls nRF9151 reset (for serial recovery entry) |
| **Baud Rate Handling** | Support dynamic baud rate changes via `CONFIG_UART_LINE_CTRL=y` |

### nRF5340 Connectivity Bridge Key Configurations

```ini
# USB Stack
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_DEVICE_VID=0x1915
CONFIG_USB_DEVICE_PID=0x5300
CONFIG_USB_COMPOSITE_DEVICE=y
CONFIG_USB_CDC_ACM=y
CONFIG_UART_LINE_CTRL=y

# UART for bridging
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_UART_ASYNC_API=y
```

### nRF5340 Device Tree Requirements

```devicetree
/* UART0 - bridges to nRF9151 UART0 (logging/DFU) */
&uart0 {
    compatible = "nordic,nrf-uarte";
    status = "okay";
    current-speed = <115200>;
    pinctrl-0 = <&uart0_default>;
    pinctrl-names = "default";
};

/* UART1 - bridges to nRF9151 UART1 (inter-chip data) */
&uart1 {
    compatible = "nordic,nrf-uarte";
    status = "okay";
    current-speed = <115200>;
    pinctrl-0 = <&uart1_default>;
    pinctrl-names = "default";
    hw-flow-control;  /* Critical for data integrity */
};

/* Reset control for nRF9151 */
/ {
    nrf9151_reset: nrf9151-reset {
        compatible = "gpio-keys";
        gpios = <&gpio0 24 GPIO_ACTIVE_LOW>;
    };
};
```

---

## Troubleshooting

### No USB Ports Appearing
1. Try a different USB cable (some are charge-only)
2. Check if nRF5340 has Connectivity Bridge firmware (see above)
3. On Windows, check Device Manager for driver issues

### Logs Not Showing
1. Verify correct port (CDC ACM 0, the lower number)
2. Check baud rate is 115200
3. Ensure the device has booted (not stuck in MCUboot)

### MCUboot Not Accepting Image
1. Verify you're using `app_update.bin` (not `zephyr.bin`)
2. Check you're in recovery mode (hold button while resetting)
3. Try 1000000 baud for MCUboot (uses high-speed UART)

### UART Sensor Data Not Working
1. Verify UART1 is still configured correctly (check overlay)
2. Check flow control pins are connected
3. Ensure both devices have matching baud rates

---

## Related Files

| File | Purpose |
|------|---------|
| `app/prj.conf` | Main project configuration (log backends) |
| `app/boards/thingy91x_nrf9151_ns.conf` | Board-specific config (console enable) |
| `app/boards/thingy91x_nrf9151_ns.overlay` | Device tree (UART0 for logging) |
| `app/sysbuild/mcuboot/boards/thingy91x_nrf9151.conf` | MCUboot serial recovery config |
| `app/sysbuild/mcuboot/boards/thingy91x_nrf9151.overlay` | MCUboot UART (1Mbps for DFU) |
