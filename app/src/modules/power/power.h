/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _POWER_H_
#define _POWER_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ZBUS channel for power module communication */
ZBUS_CHAN_DECLARE(POWER_CHAN);

enum power_msg_type {
	/* Output message types */

	/* Response message to a request for a battery percentage sample. The sample is found in the
	 * .percentage field of the message.
	 */
	POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE = 0x1,

	/* Input message types */

	/* Request to retrieve the current battery percentage. The response is sent as a
	 * POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE message.
	 */
	POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST,
};

struct power_msg {
	enum power_msg_type type;

	/** Contains the current charge of the battery in percentage. */
	double percentage;

	/** Contains the current battery voltage in volts. */
	double voltage;

	/** Contains the current battery current in milliamps (positive = charging, negative = discharging). */
	double current_ma;

	/** Contains the battery temperature in degrees Celsius. */
	double temperature;

	/** Timestamp of the sample in milliseconds since epoch. */
	int64_t timestamp;
};

#define MSG_TO_POWER_MSG(_msg)	(*(const struct power_msg *)_msg)

/* Function declarations */

/** @brief Request a power sample and publish the result
 *
 * @return 0 on success, negative error code on failure
 */
int power_sample_request(void);

/** @brief Get current power data
 *
 * @param data Pointer to power_msg structure to fill with current data
 * @return 0 on success, negative error code on failure
 */
int power_get_current_data(struct power_msg *data);

#ifdef __cplusplus
}
#endif

#endif /* _POWER_H_ */
