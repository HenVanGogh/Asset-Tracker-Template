/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _UART_SENSOR_H_
#define _UART_SENSOR_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART configuration using Kconfig values */
#define UART_RX_BUF_SIZE             CONFIG_APP_UART_SENSOR_RX_BUFFER_SIZE
#define UART_LINE_BUF_SIZE           CONFIG_APP_UART_SENSOR_LINE_BUFFER_SIZE
#define UART_MSG_QUEUE_SIZE          CONFIG_APP_UART_SENSOR_MSG_QUEUE_SIZE
#define UART_THREAD_STACK_SIZE       CONFIG_APP_UART_SENSOR_THREAD_STACK_SIZE
#define UART_THREAD_PRIORITY         CONFIG_APP_UART_SENSOR_THREAD_PRIORITY
#define UART_ZBUS_TIMEOUT_MS         CONFIG_APP_UART_SENSOR_ZBUS_TIMEOUT_MS
#define UART_PROCESSING_TIMEOUT_MS   CONFIG_APP_UART_SENSOR_PROCESSING_TIMEOUT_MS

/* Protocol configuration */
#define UART_PROTOCOL_VERSION        CONFIG_APP_UART_SENSOR_PROTOCOL_VERSION
#define UART_MESSAGE_DELIMITER       CONFIG_APP_UART_SENSOR_MESSAGE_DELIMITER
#define UART_FIELD_SEPARATOR         CONFIG_APP_UART_SENSOR_FIELD_SEPARATOR
#define UART_VALUE_SEPARATOR         CONFIG_APP_UART_SENSOR_VALUE_SEPARATOR

/* Data validation limits */
#ifdef CONFIG_APP_UART_SENSOR_DATA_VALIDATION
#define UART_TEMP_MIN                CONFIG_APP_UART_SENSOR_TEMP_MIN
#define UART_TEMP_MAX                CONFIG_APP_UART_SENSOR_TEMP_MAX
#define UART_HUMIDITY_MIN            CONFIG_APP_UART_SENSOR_HUMIDITY_MIN
#define UART_HUMIDITY_MAX            CONFIG_APP_UART_SENSOR_HUMIDITY_MAX
#endif

/* Battery voltage configuration */
#define UART_BATTERY_MIN_MV          CONFIG_APP_UART_SENSOR_BATTERY_MIN_MV
#define UART_BATTERY_MAX_MV          CONFIG_APP_UART_SENSOR_BATTERY_MAX_MV

/* Probe ID configuration */
#define UART_PROBE_ID_MIN_LEN        CONFIG_APP_UART_SENSOR_PROBE_ID_MIN_LEN
#define UART_PROBE_ID_MAX_LEN        CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN

/* Error handling */
#ifdef CONFIG_APP_UART_SENSOR_ERROR_RECOVERY
#define UART_MAX_CONSECUTIVE_ERRORS  CONFIG_APP_UART_SENSOR_MAX_CONSECUTIVE_ERRORS
#define UART_RECOVERY_DELAY_MS       CONFIG_APP_UART_SENSOR_RECOVERY_DELAY_MS
#endif

/* ZBUS channel for UART sensor module communication */
ZBUS_CHAN_DECLARE(UART_SENSOR_CHAN);

enum uart_sensor_msg_type {
	/* Output message types */

	/** Response message containing sensor data from external probe. */
	UART_SENSOR_DATA_RESPONSE = 0x1,

	/** Error message indicating communication or data validation failure. */
	UART_SENSOR_ERROR_RESPONSE = 0x2,

	/** Statistics message containing performance and error counters. */
	UART_SENSOR_STATS_RESPONSE = 0x3,

	/** Generic response message containing text data from UART. */
	UART_SENSOR_GENERIC_RESPONSE = 0x4,

	/* Input message types */

	/** Request to sample sensor data from external probe via UART. */
	UART_SENSOR_DATA_REQUEST = 0x10,
	
	/** Request to reset error counters and statistics. */
	UART_SENSOR_RESET_STATS_REQUEST = 0x11,

	/** Request to get current module status and configuration. */
	UART_SENSOR_STATUS_REQUEST = 0x12,
};

enum uart_sensor_error_type {
	UART_SENSOR_ERROR_NONE = 0,
	UART_SENSOR_ERROR_PARSE_FAILED,
	UART_SENSOR_ERROR_INVALID_DATA,
	UART_SENSOR_ERROR_COMM_TIMEOUT,
	UART_SENSOR_ERROR_DEVICE_NOT_READY,
	UART_SENSOR_ERROR_BUFFER_OVERFLOW,
	UART_SENSOR_ERROR_VALIDATION_FAILED,
	UART_SENSOR_ERROR_REMOTE_ERROR, /* Error reported by the remote device (ERR:...) */
};

struct uart_sensor_msg {
	enum uart_sensor_msg_type type;

	/** Temperature reading from external probe in degrees Celsius. */
	float temperature;

	/** Humidity reading from external probe in percentage (0-100). */
	float humidity;

	/** Probe identifier string (plain name, not formatted). */
	char probe_id[CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN + 1];

	/** Battery level of the external probe in percentage (0-100). */
	float probe_battery;

	/** Timestamp of the sample in milliseconds since epoch. */
	int64_t timestamp;

	/** Error information (if type is UART_SENSOR_ERROR_RESPONSE). */
	enum uart_sensor_error_type error_type;

	/** Additional error details or status information. */
	char error_details[64];
	
	/** Generic response text (if type is UART_SENSOR_GENERIC_RESPONSE). */
	char response_text[128];

	/** Data quality indicators. */
	struct {
		bool temperature_valid;
		bool humidity_valid;
		bool battery_valid;
		bool probe_id_valid;
	} data_quality;
};

struct uart_sensor_stats {
	/** Total number of messages received. */
	uint32_t messages_received;

	/** Number of successfully parsed messages. */
	uint32_t messages_parsed;

	/** Number of messages that failed parsing. */
	uint32_t parse_errors;

	/** Number of messages with invalid data. */
	uint32_t validation_errors;

	/** Number of ZBUS publish failures. */
	uint32_t publish_errors;

	/** Number of UART communication errors. */
	uint32_t comm_errors;

	/** Last error timestamp. */
	int64_t last_error_time;

	/** Module uptime in milliseconds. */
	int64_t uptime_ms;
};

#define MSG_TO_UART_SENSOR_MSG(_msg)	(*(const struct uart_sensor_msg *)_msg)

/* Function declarations */

/** @brief Initialize UART sensor module
 *
 * Initializes UART communication for external sensor probes.
 * This function is called automatically during system initialization.
 *
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_init(void);

/** @brief Request a UART sensor sample and publish the result
 *
 * Triggers immediate publication of the most recent sensor data.
 * If auto-publication is disabled, this is the primary way to get data.
 *
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_sample_request(void);

/** @brief Send a raw command string to the UART sensor.
 *
 * Appends a newline if one is not present.
 *
 * @param cmd Null-terminated command string to send
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_send_command(const char *cmd);

/** @brief Get current UART sensor data
 *
 * Retrieves the most recently received sensor data without triggering
 * a new measurement or publication.
 *
 * @param data Pointer to uart_sensor_msg structure to fill with current data
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_get_current_data(struct uart_sensor_msg *data);

/** @brief Check UART sensor status and configuration
 *
 * Provides detailed status information about the UART sensor module,
 * including device readiness, error counts, and configuration.
 *
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_check_status(void);

/** @brief Get UART sensor statistics
 *
 * Retrieves performance and error statistics for the UART sensor module.
 *
 * @param stats Pointer to uart_sensor_stats structure to fill
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_get_stats(struct uart_sensor_stats *stats);

/** @brief Reset UART sensor statistics
 *
 * Resets all error counters and performance statistics to zero.
 *
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_reset_stats(void);

/** @brief Process UART data line
 *
 * Parses a received UART data line and extracts sensor information.
 * This function is typically called from the UART interrupt handler
 * or processing thread.
 *
 * @param data The null-terminated string received from UART
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_process_data_line(const char *data);

/** @brief Enable or disable automatic data publication
 *
 * Controls whether sensor data is automatically published via ZBUS
 * when received from the UART.
 *
 * @param enable true to enable auto-publication, false to disable
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_set_auto_publish(bool enable);

/** @brief Validate sensor data against configured limits
 *
 * Checks if the provided sensor data is within acceptable ranges
 * as defined by Kconfig settings.
 *
 * @param msg Pointer to sensor message to validate
 * @return true if data is valid, false otherwise
 */
bool uart_sensor_validate_data(const struct uart_sensor_msg *msg);

#ifdef __cplusplus
}
#endif

#endif /* _UART_SENSOR_H_ */
