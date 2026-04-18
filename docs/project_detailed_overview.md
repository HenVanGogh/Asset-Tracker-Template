# nRF5340 Firmware Documentation

## 1. Overview
The nRF5340 serves as a specialized BLE scanner and gateway in this project. Its primary role is to listen for Extended Advertising packets (Coded PHY) from "nRF52840_" sensor beacons, decode the custom telemetry data, and forward the aggregated information to the nRF9160 via UART for cloud connectivity.

## 2. Architecture
The firmware is built on the **Zephyr RTOS** and leverages the Nordic Connect SDK (NCS). It uses a multi-threaded architecture to handle scanning, data processing, and UART communication concurrently without blocking critical operations.

### Core Components
- **BLE Scanner**: Handles radio operations and packet filtering.
- **Sensor Manager**: Maintains the state of discovered devices.
- **Reporting Module**: Manages periodic data transmission.
- **UART Handler**: Low-level driver interface for inter-chip communication.
- **UI Handler**: Provides visual feedback via LEDs.

## 3. Module Details

### 3.1 BLE Scanner (`ble_scanner.c`)
- **Configuration**: The scanner is configured for **Extended Scanning** to support Coded PHY (Long Range), which is essential for receiving data from distant sensors.
- **Filtering**: It implements a name-based filter, processing only devices whose names start with `"nRF52840_"`.
- **Data Parsing**: The scanner looks for a custom Service Data structure (ID `0x20`) within the advertising payload. This structure contains the sensor readings.

### 3.2 Sensor Manager (`sensor_manager.c`)
- **Registry**: Maintains a dynamic list of up to **10 unique sensors**.
- **Data Decoding**:
    - **Temperature & Humidity**: Decoded from 8.8 fixed-point format to floating-point values.
    - **Battery Voltage**: Raw millivolt value.
    - **Statistics**: Tracks advertisement counts and sensor uptime.
- **State Management**: Updates existing entries with new data or creates new entries for newly discovered sensors.

### 3.3 Reporting Module (`reporting.c`)
- **Dedicated Thread**: A separate thread (`reporting_thread`) handles the blocking UART transmission to ensure the scanning process is never interrupted.
- **Timer-Driven**: A Zephyr timer triggers the reporting process every **10 minutes**.
- **Safe Resource Sharing**:
    - **Memory Slab**: Pre-allocated buffers (`report_buf_slab`) prevent heap fragmentation and ensure fast allocation.
    - **Message Queue**: Pointers to filled buffers are passed to the reporting thread via a message queue (`report_msgq`), ensuring thread safety.

### 3.4 UART Handler (`uart_handler.c`)
- Provides a simplified abstraction over the Zephyr UART driver (`uart1`).
- Handles the physical transmission of data strings to the nRF9160.

### 3.5 UI Handler (`ui_handler.c`)
- Controls the board's LEDs to indicate status.
- **nRF52840 Dongle**: Uses RGB LEDs (Green/Blue).
- **Feather Board**: Uses the Blue LED.
- *Note: The active status update logic is currently disabled in the main loop.*

## 4. Data Flow

```mermaid
graph LR
    A[BLE Scanner] -->|Adv Packet| B(Parse Name & Data)
    B -->|Valid Sensor| C[Sensor Manager]
    C -->|Update/Create| D[(Sensor Registry)]
    E[Reporting Timer] -->|Trigger (10m)| F[Format Data]
    D -.-> F
    F -->|Alloc Buffer| G[Message Queue]
    G -->|Dequeue| H[Reporting Thread]
    H -->|UART TX| I[nRF9160]
```

1.  **Scan**: The scanner receives an advertising packet.
2.  **Parse**: It checks the device name and extracts the custom telemetry frame.
3.  **Update**: The `Sensor Manager` updates the sensor's latest data in the registry.
4.  **Trigger**: Every 10 minutes, the `Reporting Timer` fires.
5.  **Format & Queue**: The timer handler formats the data for all active sensors and pushes the buffers to a message queue.
6.  **Transmit**: The `Reporting Thread` wakes up, consumes the messages, and sends the data over UART to the nRF9160.