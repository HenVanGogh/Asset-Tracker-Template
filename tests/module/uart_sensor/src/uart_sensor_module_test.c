/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Contract test for the uart_sensor module's public ZBus interface.
 * Verifies that the channel and message struct declared in uart_sensor.h
 * are usable by downstream modules. See CMakeLists.txt for guidance on
 * extending this into full-module coverage.
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <string.h>

#include "uart_sensor.h"

/* uart_sensor.c normally defines this channel. The contract test owns it. */
ZBUS_CHAN_DEFINE(UART_SENSOR_CHAN,
		 struct uart_sensor_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(UART_SENSOR_CHAN, test_subscriber, 0);

void setUp(void) {}
void tearDown(void)
{
	const struct zbus_channel *chan;
	struct uart_sensor_msg drain;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &drain, K_MSEC(50)) == 0) {
	}
}

void test_data_request_round_trip(void)
{
	struct uart_sensor_msg in = { .type = UART_SENSOR_DATA_REQUEST };
	struct uart_sensor_msg out;
	const struct zbus_channel *chan;

	int err = zbus_chan_pub(&UART_SENSOR_CHAN, &in, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &out, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL_PTR(&UART_SENSOR_CHAN, chan);
	TEST_ASSERT_EQUAL(UART_SENSOR_DATA_REQUEST, out.type);
}

void test_message_type_enum_values_are_unique(void)
{
	/* Guard against accidental enum collisions when new message types are
	 * added. Spot-check several values declared in uart_sensor.h.
	 */
	TEST_ASSERT_NOT_EQUAL(UART_SENSOR_DATA_RESPONSE, UART_SENSOR_ERROR_RESPONSE);
	TEST_ASSERT_NOT_EQUAL(UART_SENSOR_ESL_TAG_FOUND, UART_SENSOR_ESL_TAG_CONNECTED);
	TEST_ASSERT_NOT_EQUAL(UART_SENSOR_DATA_REQUEST, UART_SENSOR_DATA_RESPONSE);
}

extern int unity_main(void);

int main(void) { (void)unity_main(); return 0; }
