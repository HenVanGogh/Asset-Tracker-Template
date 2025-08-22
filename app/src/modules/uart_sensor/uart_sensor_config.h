/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file uart_sensor_config.h
 * @brief Production-quality configuration for UART sensor module
 * 
 * This module provides comprehensive configuration options for UART-based
 * external sensor communication. All parameters can be configured at build
 * time using Kconfig for production deployment flexibility.
 * 
 * @section probe_id_formatting Probe ID Formatting
 * 
 * The module supports two probe ID formatting modes:
 * 
 * 1. **MAC-style hex formatting** (default, UART_SENSOR_FORMAT_PROBE_ID=1):
 *    - Input:  "nRF_52840_MySensor"
 *    - Output: "4D:79:53:65:6E:73:6F:72:00:00:00:00:00:00:00:00"
 *    - Use case: When you need consistent hex-encoded identifiers
 * 
 * 2. **Raw name formatting** (UART_SENSOR_FORMAT_PROBE_ID=0):
 *    - Input:  "nRF_52840_MySensor"
 *    - Output: "MySensor"
 *    - Use case: When you want human-readable probe names in MQTT messages
 * 
 * @section configuration Configuration Examples
 * 
 * To disable probe ID formatting and use raw names:
 * @code{.conf}
 * CONFIG_APP_UART_SENSOR_FORMAT_PROBE_ID=n
 * @endcode
 * 
 * To adjust data freshness timeout:
 * @code{.conf}
 * CONFIG_APP_UART_SENSOR_DATA_MAX_AGE_MS=600000  # 10 minutes
 * @endcode
 */

#ifndef _UART_SENSOR_CONFIG_H_
#define _UART_SENSOR_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* UART sensor configuration parameters */

/** Maximum age of sensor data before it's considered stale (milliseconds) */
#ifdef CONFIG_APP_UART_SENSOR_DATA_MAX_AGE_MS
#define UART_SENSOR_DATA_MAX_AGE_MS         CONFIG_APP_UART_SENSOR_DATA_MAX_AGE_MS
#else
#define UART_SENSOR_DATA_MAX_AGE_MS         (5 * 60 * 1000)  /* 5 minutes */
#endif

/** Minimum valid temperature range (degrees Celsius) */
#define UART_SENSOR_TEMP_MIN                (-40.0f)

/** Maximum valid temperature range (degrees Celsius) */
#define UART_SENSOR_TEMP_MAX                (85.0f)

/** Minimum valid humidity range (percentage) */
#define UART_SENSOR_HUMIDITY_MIN            (0.0f)

/** Maximum valid humidity range (percentage) */
#define UART_SENSOR_HUMIDITY_MAX            (100.0f)

/** Minimum valid battery level (percentage) */
#define UART_SENSOR_BATTERY_MIN             (0.0f)

/** Maximum valid battery level (percentage) */
#define UART_SENSOR_BATTERY_MAX             (100.0f)

/** Minimum valid probe ID length */
#define UART_SENSOR_PROBE_ID_MIN_LEN        (5)

/** Background processing thread priority */
#ifdef CONFIG_APP_UART_SENSOR_THREAD_PRIORITY
#define UART_SENSOR_THREAD_PRIORITY         CONFIG_APP_UART_SENSOR_THREAD_PRIORITY
#else
#define UART_SENSOR_THREAD_PRIORITY         (5)
#endif

/** Background processing thread stack size */
#ifdef CONFIG_APP_UART_SENSOR_THREAD_STACK_SIZE
#define UART_SENSOR_THREAD_STACK_SIZE       CONFIG_APP_UART_SENSOR_THREAD_STACK_SIZE
#else
#define UART_SENSOR_THREAD_STACK_SIZE       (2048)
#endif

/** UART receive buffer size */
#define UART_SENSOR_RX_BUF_SIZE             (256)

/** UART message queue size (number of messages) */
#ifdef CONFIG_APP_UART_SENSOR_MSG_QUEUE_SIZE
#define UART_SENSOR_MSG_QUEUE_SIZE          CONFIG_APP_UART_SENSOR_MSG_QUEUE_SIZE
#else
#define UART_SENSOR_MSG_QUEUE_SIZE          (10)
#endif

/** UART message queue alignment */
#define UART_SENSOR_MSG_QUEUE_ALIGN         (4)

/** Probe ID formatting configuration */
/** Set to 1 to enable MAC-style hex formatting (e.g., "41:42:43:44:45:46:47:48:...") */
/** Set to 0 to use raw probe name as received from UART (e.g., "nRF_52840_MySensor") */
#ifdef CONFIG_APP_UART_SENSOR_FORMAT_PROBE_ID
#define UART_SENSOR_FORMAT_PROBE_ID         CONFIG_APP_UART_SENSOR_FORMAT_PROBE_ID
#else
#define UART_SENSOR_FORMAT_PROBE_ID         (1)
#endif

/** Probe ID prefix to remove when formatting is enabled */
#define UART_SENSOR_PROBE_ID_PREFIX         "nRF_52840_"

/** Maximum length for raw probe ID (when formatting is disabled) */
#define UART_SENSOR_RAW_PROBE_ID_MAX_LEN    (31)

/** Default probe ID for uninitialized state */
#define UART_SENSOR_DEFAULT_PROBE_ID        "NO_PROBE"

/** Initialization probe ID (used during startup) */
#define UART_SENSOR_INIT_PROBE_ID           "PROBE_INIT"

/** ZBUS channel publication timeout (milliseconds) */
#define UART_SENSOR_ZBUS_TIMEOUT_MS         (250)

/** Minimum delay between UART device operations (milliseconds) */
#define UART_SENSOR_DEVICE_SETTLE_DELAY_MS  (10)

#ifdef __cplusplus
}
#endif

#endif /* _UART_SENSOR_CONFIG_H_ */
