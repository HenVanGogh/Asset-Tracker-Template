# UART Sensor Module

## Overview

The UART sensor module provides production-quality communication with external sensor probes via UART. It features comprehensive configuration options, error handling, data validation, and statistics collection.

## Key Features

- **Configurable UART Communication**: Customizable buffer sizes, timeouts, and protocol settings
- **Data Validation**: Optional range checking for temperature, humidity, and battery readings
- **Error Recovery**: Automatic recovery from communication errors with configurable retry logic
- **Statistics Collection**: Comprehensive tracking of messages, errors, and performance metrics
- **Thread Safety**: Mutex-protected data access for multi-threaded environments
- **Power Management**: Optional low-power features with wake-up capability
- **Flexible Probe ID Handling**: Use plain probe names without MAC-style formatting

## Configuration

### Basic Settings

```kconfig
CONFIG_APP_UART_SENSOR=y                    # Enable UART sensor module
CONFIG_APP_UART_SENSOR_AUTO_PUBLISH=y       # Auto-publish received data
CONFIG_APP_UART_SENSOR_DATA_VALIDATION=y    # Enable data validation
```

### Communication Settings

```kconfig
CONFIG_APP_UART_SENSOR_RX_BUFFER_SIZE=256          # UART RX buffer size
CONFIG_APP_UART_SENSOR_LINE_BUFFER_SIZE=128        # Line assembly buffer size
CONFIG_APP_UART_SENSOR_MSG_QUEUE_SIZE=10           # Message queue depth
CONFIG_APP_UART_SENSOR_ZBUS_TIMEOUT_MS=1000        # ZBUS publish timeout
```

### Threading and Performance

```kconfig
CONFIG_APP_UART_SENSOR_THREAD_STACK_SIZE=2048      # Processing thread stack
CONFIG_APP_UART_SENSOR_THREAD_PRIORITY=5           # Thread priority (lower = higher)
CONFIG_APP_UART_SENSOR_PROCESSING_TIMEOUT_MS=5000  # Data processing timeout
```

### Data Validation

```kconfig
CONFIG_APP_UART_SENSOR_TEMP_MIN=-40               # Minimum valid temperature (°C)
CONFIG_APP_UART_SENSOR_TEMP_MAX=85                # Maximum valid temperature (°C)
CONFIG_APP_UART_SENSOR_HUMIDITY_MIN=0             # Minimum valid humidity (%)
CONFIG_APP_UART_SENSOR_HUMIDITY_MAX=100           # Maximum valid humidity (%)
CONFIG_APP_UART_SENSOR_REJECT_INVALID_DATA=n      # Reject or accept invalid data
```

### Error Handling

```kconfig
CONFIG_APP_UART_SENSOR_ERROR_RECOVERY=y           # Enable automatic error recovery
CONFIG_APP_UART_SENSOR_MAX_CONSECUTIVE_ERRORS=5   # Errors before recovery attempt
CONFIG_APP_UART_SENSOR_RECOVERY_DELAY_MS=5000     # Delay between recovery attempts
```

### Protocol Configuration

```kconfig
CONFIG_APP_UART_SENSOR_MESSAGE_DELIMITER="\r\n"   # Message end characters
CONFIG_APP_UART_SENSOR_FIELD_SEPARATOR=":"        # Name/data separator
CONFIG_APP_UART_SENSOR_VALUE_SEPARATOR=","        # Value separator
```

### Probe ID Settings

```kconfig
CONFIG_APP_UART_SENSOR_PROBE_ID_FORMAT=n          # Disable MAC-style formatting
CONFIG_APP_UART_SENSOR_PROBE_ID_MIN_LEN=3         # Minimum probe name length
CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN=31        # Maximum probe name length
CONFIG_APP_UART_SENSOR_DEFAULT_PROBE_ID="UNKNOWN_PROBE"  # Default probe name
```

## Protocol Format

The module expects UART data in the following format:

```
probe_name:temperature,humidity,battery_mv
```

### Example

```
MyProbe_001:23.5,45.2,3800
WeatherStation:21.0,67.8,4100
nRF_Sensor_123:25.1,32.4,3950
```

### Field Descriptions

- **probe_name**: Plain text identifier for the sensor probe (used as-is, no formatting)
- **temperature**: Temperature reading in degrees Celsius
- **humidity**: Relative humidity as percentage (0-100)
- **battery_mv**: Battery voltage in millivolts

## API Functions

### Core Functions

```c
int uart_sensor_init(void);                          // Initialize module
int uart_sensor_sample_request(void);                // Request data publication
int uart_sensor_get_current_data(struct uart_sensor_msg *data);  // Get latest data
int uart_sensor_check_status(void);                  // Check module status
```

### Statistics and Control

```c
int uart_sensor_get_stats(struct uart_sensor_stats *stats);      // Get statistics
int uart_sensor_reset_stats(void);                   // Reset error counters
int uart_sensor_set_auto_publish(bool enable);       // Enable/disable auto-publish
bool uart_sensor_validate_data(const struct uart_sensor_msg *msg);  // Validate data
```

## ZBUS Integration

The module publishes data via ZBUS channel `UART_SENSOR_CHAN` with message types:

- `UART_SENSOR_DATA_RESPONSE`: Normal sensor data
- `UART_SENSOR_ERROR_RESPONSE`: Error information
- `UART_SENSOR_STATS_RESPONSE`: Statistics data

## MQTT Integration

When integrated with the custom MQTT module, sensor data is published as JSON:

```json
{
  "device_id": "thingy91x-asset-tracker",
  "type": "uart_sensor",
  "sequence": 42,
  "timestamp": 1234567890,
  "data": {
    "temperature": 23.5,
    "humidity": 45.2,
    "probe_name": "MyProbe_001",
    "probe_battery": 90.5,
    "sensor_timestamp": 1234567890
  },
  "quality": {
    "temperature_valid": true,
    "humidity_valid": true,
    "battery_valid": true,
    "probe_name_valid": true
  }
}
```

## Error Handling

The module provides comprehensive error handling:

### Error Types

- `UART_SENSOR_ERROR_PARSE_FAILED`: Data parsing failed
- `UART_SENSOR_ERROR_INVALID_DATA`: Data validation failed
- `UART_SENSOR_ERROR_COMM_TIMEOUT`: Communication timeout
- `UART_SENSOR_ERROR_DEVICE_NOT_READY`: UART device not ready
- `UART_SENSOR_ERROR_BUFFER_OVERFLOW`: Buffer overflow
- `UART_SENSOR_ERROR_VALIDATION_FAILED`: Data validation failed

### Recovery Features

- Automatic device recovery after consecutive errors
- Configurable retry delays and thresholds
- Statistics tracking for monitoring system health
- Error message publication via ZBUS

## Statistics

The module tracks comprehensive statistics:

```c
struct uart_sensor_stats {
    uint32_t messages_received;     // Total messages received
    uint32_t messages_parsed;       // Successfully parsed messages
    uint32_t parse_errors;          // Parse failure count
    uint32_t validation_errors;     // Validation failure count
    uint32_t publish_errors;        // ZBUS publish failure count
    uint32_t comm_errors;           // Communication error count
    int64_t last_error_time;        // Timestamp of last error
    int64_t uptime_ms;              // Module uptime
};
```

## Power Management

Optional power management features:

- UART wake-up capability for low-power operation
- Device suspend/resume support
- Configurable stabilization delays

## Best Practices

1. **Buffer Sizing**: Set buffer sizes based on expected message length and frequency
2. **Validation**: Enable data validation in production environments
3. **Error Recovery**: Enable automatic recovery for unattended operation
4. **Statistics**: Monitor statistics regularly to detect communication issues
5. **Thread Priority**: Set appropriate thread priority based on system requirements
6. **Probe Names**: Use descriptive, unique probe names for identification

## Troubleshooting

### Common Issues

1. **No Data Received**: Check UART device configuration and wiring
2. **Parse Errors**: Verify data format matches expected protocol
3. **Validation Failures**: Check sensor ranges in configuration
4. **Buffer Overflows**: Increase buffer sizes or reduce data frequency
5. **High Error Rates**: Enable error recovery and check communication quality

### Debug Options

Enable debug logging:
```kconfig
CONFIG_APP_UART_SENSOR_LOG_LEVEL_DBG=y
```

Check module status:
```c
uart_sensor_check_status();
```

Monitor statistics:
```c
struct uart_sensor_stats stats;
uart_sensor_get_stats(&stats);
LOG_INF("Messages: %u, Errors: %u", stats.messages_received, stats.parse_errors);
```
