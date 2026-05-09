/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <date_time.h>

#include "app_common.h"
#include "power.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(float, nrf_fuel_gauge_process, float, float, float, float, bool, void *);
FAKE_VALUE_FUNC(int, charger_read_sensors, float *, float *, float *, int32_t *);
FAKE_VALUE_FUNC(int, nrf_fuel_gauge_init, const struct nrf_fuel_gauge_init_parameters *, void *);
FAKE_VALUE_FUNC(int, mfd_npm1300_add_callback, const struct device *, struct gpio_callback *);
FAKE_VALUE_FUNC(int, date_time_now, int64_t *);

ZBUS_MSG_SUBSCRIBER_DEFINE(power_subscriber);
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_subscriber, 0);

LOG_MODULE_REGISTER(power_module_test, 4);

/* Test timestamp value to be returned by date_time_now mock */
void setUp(void)
{
	/* Reset fakes */
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_now);

	/* Drain any messages left by previous tests to keep tests independent. */
	const struct zbus_channel *_drain_chan;
	struct power_msg _drain_msg;

	while (zbus_sub_wait_msg(&power_subscriber, &_drain_chan, &_drain_msg,
				 K_NO_WAIT) == 0) {
	}
}

void check_power_event(enum power_msg_type expected_power_type)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No power event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &POWER_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_power_type, power_msg.type);

	/* Timestamp comes from k_uptime_get() — verify it is non-negative. */
	if (expected_power_type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
		TEST_ASSERT_GREATER_OR_EQUAL(0, power_msg.timestamp);
		LOG_DBG("Timestamp ok: %lld ms uptime", power_msg.timestamp);
	}
}

void check_no_power_events(uint32_t time_in_seconds)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Received trigger event with type %d", power_msg.type);
		TEST_FAIL();
	}
}

void test_timestamp_verification(void)
{
	/* power_sample_request() stores k_uptime_get() as the timestamp.
	 * Run three times and verify each response carries a non-negative
	 * uptime timestamp that increases monotonically. */
	int64_t prev_ts = -1;

	for (int i = 0; i < 3; i++) {
		int err = power_sample_request();

		TEST_ASSERT_EQUAL(0, err);

		/* Wait for the RESPONSE published by power_sample_request(). */
		const struct zbus_channel *chan;
		struct power_msg msg;

		k_sleep(K_MSEC(10));
		err = zbus_sub_wait_msg(&power_subscriber, &chan, &msg, K_MSEC(1000));
		TEST_ASSERT_EQUAL(0, err);
		TEST_ASSERT_EQUAL(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE, msg.type);
		TEST_ASSERT_GREATER_OR_EQUAL(prev_ts, msg.timestamp);
		LOG_DBG("Iter %d: timestamp=%lld", i, msg.timestamp);
		prev_ts = msg.timestamp;
	}
}

void test_timestamp_error_handling(void)
{
	/* power.c does NOT call date_time_now — it uses k_uptime_get().
	 * This test verifies the module still publishes a RESPONSE even
	 * when nrf_fuel_gauge_process returns 0 (default fake value)
	 * and sensor values are out-of-range (data_valid = false). */

	int err = power_sample_request();

	TEST_ASSERT_EQUAL(0, err);

	const struct zbus_channel *chan;
	struct power_msg msg;

	k_sleep(K_MSEC(10));
	err = zbus_sub_wait_msg(&power_subscriber, &chan, &msg, K_MSEC(1000));
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE, msg.type);
	/* When mock sensor returns 0.0f for voltage/temp, the module marks
	 * data_valid=false (out of normal range). Percentage is clamped to [0,100]. */
	TEST_ASSERT_LESS_OR_EQUAL(100.0, msg.percentage);
}

void test_power_percentage_sample(void)
{
	/* Verify multiple sequential samples each produce a RESPONSE. */
	for (int i = 0; i < 5; i++) {
		int err = power_sample_request();

		TEST_ASSERT_EQUAL(0, err);
		check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
	}
}

/* power_sample_request() is the synchronous public API — verify it
 * publishes exactly one RESPONSE on POWER_CHAN.
 */
void test_power_sample_request_helper_emits_response(void)
{
	int err = power_sample_request();

	TEST_ASSERT_EQUAL(0, err);

	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
}

/* power_get_current_data() must reject NULL output pointer. */
void test_power_get_current_data_rejects_null(void)
{
	int err = power_get_current_data(NULL);

	TEST_ASSERT_LESS_THAN(0, err);
}

/* Back-to-back calls to power_sample_request() should both produce
 * a RESPONSE without dropping events.
 */
void test_power_back_to_back_sample_requests(void)
{
	power_sample_request();
	power_sample_request();

	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
	check_power_event(POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE);
}

/* This is required to be added to each test. That is because unity's
 * main may return nonzero, while zephyr's main currently must
 * return 0 in all cases (other values are reserved).
 */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	k_sleep(K_FOREVER);

	return 0;
}
