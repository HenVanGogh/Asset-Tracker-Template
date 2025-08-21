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

/* UART configuration */
#define UART_RX_BUF_SIZE 256

/* ZBUS channel for UART sensor module communication */
ZBUS_CHAN_DECLARE(UART_SENSOR_CHAN);

enum uart_sensor_msg_type {
	/* Output message types */

	/* Response message containing sensor data from external probe. */
	UART_SENSOR_DATA_RESPONSE = 0x1,

	/* Input message types */

	/* Request to sample sensor data from external probe via UART. */
	UART_SENSOR_DATA_REQUEST,
};

struct uart_sensor_msg {
	enum uart_sensor_msg_type type;

	/** Temperature reading from external probe in degrees Celsius. */
	float temperature;

	/** Humidity reading from external probe in percentage (0-100). */
	float humidity;

	/** Probe identifier string (MAC-like format). */
	char probe_id[32];

	/** Battery level of the external probe in percentage (0-100). */
	float probe_battery;

	/** Timestamp of the sample in milliseconds since epoch. */
	int64_t timestamp;
};

#define MSG_TO_UART_SENSOR_MSG(_msg)	(*(const struct uart_sensor_msg *)_msg)

/* Function declarations */

/** @brief Initialize UART sensor module
 *
 * Initializes UART communication for external sensor probes
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_init(void);

/** @brief Request a UART sensor sample and publish the result
 *
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_sample_request(void);

/** @brief Get current UART sensor data
 *
 * @param data Pointer to uart_sensor_msg structure to fill with current data
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_get_current_data(struct uart_sensor_msg *data);

/** @brief Check UART sensor status
 *
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_check_status(void);

/** @brief Process UART data line
 *
 * Parses a received UART data line and extracts sensor information
 * @param data The null-terminated string received from UART
 * @return 0 on success, negative error code on failure
 */
int uart_sensor_process_data_line(const char *data);

#ifdef __cplusplus
}
#endif

#endif /* _UART_SENSOR_H_ */
