/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Contract test for custom_mqtt's public ZBus interface. See CMakeLists.txt
 * for guidance on extending this into full-module coverage.
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <string.h>

#include "custom_mqtt.h"

ZBUS_CHAN_DEFINE(CUSTOM_MQTT_CHAN,
		 struct custom_mqtt_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(CUSTOM_MQTT_CHAN, test_subscriber, 0);

void setUp(void) {}
void tearDown(void)
{
	const struct zbus_channel *chan;
	struct custom_mqtt_msg drain;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &drain, K_MSEC(50)) == 0) {
	}
}

void test_data_send_round_trip(void)
{
	char payload[] = "hello";
	struct custom_mqtt_msg in = {
		.type = CUSTOM_MQTT_EVT_DATA_SEND,
		.data_send = { .data = payload, .len = sizeof(payload) - 1 },
	};
	struct custom_mqtt_msg out;
	const struct zbus_channel *chan;

	int err = zbus_chan_pub(&CUSTOM_MQTT_CHAN, &in, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &out, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(CUSTOM_MQTT_EVT_DATA_SEND, out.type);
	TEST_ASSERT_EQUAL(sizeof(payload) - 1, out.data_send.len);
}

void test_error_event_carries_error_code(void)
{
	struct custom_mqtt_msg in = {
		.type = CUSTOM_MQTT_EVT_ERROR,
		.error = { .err_code = -ETIMEDOUT },
	};
	struct custom_mqtt_msg out;
	const struct zbus_channel *chan;

	int err = zbus_chan_pub(&CUSTOM_MQTT_CHAN, &in, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &out, K_SECONDS(1));
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(CUSTOM_MQTT_EVT_ERROR, out.type);
	TEST_ASSERT_EQUAL(-ETIMEDOUT, out.error.err_code);
}

extern int unity_main(void);

int main(void) { (void)unity_main(); return 0; }
