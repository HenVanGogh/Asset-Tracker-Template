/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/device.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>
#include <math.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app_common.h"
#include "power.h"

/* Register log module */
LOG_MODULE_REGISTER(power, CONFIG_APP_POWER_LOG_LEVEL);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(power);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(POWER_CHAN,
		 struct power_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Observe power channel to receive requests */
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power, 0);

#define MAX_MSG_SIZE sizeof(struct power_msg)

BUILD_ASSERT(CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Power module context */
static struct {
	int wdt_id;
	float battery_percentage;
	bool initialized;
} power_ctx = {
	.battery_percentage = 50.0f, /* Default to 50% */
	.initialized = false
};

/* Simple battery percentage simulation for testing */
static float simulate_battery_percentage(void)
{
	static float simulated_percentage = 85.0f;
	static int32_t last_time = 0;
	int32_t current_time = k_uptime_get_32();
	
	/* Simple simulation: decrease by 0.1% every 30 seconds */
	if (current_time - last_time > 30000) {
		simulated_percentage -= 0.1f;
		if (simulated_percentage < 0.0f) {
			simulated_percentage = 100.0f; /* Reset to full for testing */
		}
		last_time = current_time;
	}
	
	return simulated_percentage;
}

/* Read battery percentage */
static float read_battery_percentage(void)
{
	/* For Thingy91X, we would normally use nRF fuel gauge here,
	 * but for now we'll use a simple simulation for testing the button functionality
	 */
	float percentage = simulate_battery_percentage();
	
	LOG_DBG("Battery percentage: %.1f%%", (double)percentage);
	
	/* Validate percentage range */
	if (percentage < 0.0f || percentage > 100.0f) {
		LOG_WRN("Invalid battery percentage: %.1f%%, using cached value", (double)percentage);
		return power_ctx.battery_percentage;
	}
	
	power_ctx.battery_percentage = percentage;
	return percentage;
}

/* Send power response message */
static void send_power_response(float percentage, int64_t timestamp)
{
	struct power_msg msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
		.percentage = (double)percentage,
		.timestamp = timestamp
	};

	int err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	LOG_INF("Battery percentage published: %.1f%%", (double)percentage);
}

/* Handle power sample requests */
static void power_message_handler(const void *data)
{
	const struct power_msg *msg = (const struct power_msg *)data;
	int64_t timestamp;
	float percentage;

	switch (msg->type) {
	case POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST:
		LOG_DBG("Battery percentage sample requested");

		/* Get current timestamp */
		int err = date_time_now(&timestamp);
		if (err) {
			LOG_WRN("date_time_now, error: %d, using k_uptime_get()", err);
			timestamp = k_uptime_get();
		}

		/* Read battery percentage */
		percentage = read_battery_percentage();
		if (percentage < 0.0f) {
			LOG_ERR("Failed to read battery percentage");
			return;
		}

		/* Send response */
		send_power_response(percentage, timestamp);
		break;

	default:
		LOG_WRN("Unknown power message type: %d", msg->type);
		break;
	}
}

/* Initialize power module */
static int power_init(void)
{
	LOG_DBG("Initializing power module");

	/* Read initial battery percentage */
	float percentage = read_battery_percentage();
	if (percentage >= 0.0f) {
		LOG_INF("Initial battery percentage: %.1f%%", (double)percentage);
	}

	power_ctx.initialized = true;
	LOG_INF("Power module initialized successfully");

	return 0;
}

/* Watchdog timeout callback */
static void watchdog_timeout_handler(int channel_id, void *user_data)
{
	LOG_ERR("Power module watchdog timeout");
	SEND_FATAL_ERROR();
}

/* Power module thread */
static void power_thread(void *arg1, void *arg2, void *arg3)
{
	const struct zbus_channel *chan;
	int err;

	LOG_INF("Power module thread started");

	/* Initialize power module */
	err = power_init();
	if (err) {
		LOG_ERR("Failed to initialize power module, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Register watchdog */
	power_ctx.wdt_id = task_wdt_add(CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS * 1000,
					watchdog_timeout_handler, 
					NULL);
	if (power_ctx.wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", power_ctx.wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	while (true) {
		/* Feed watchdog */
		err = task_wdt_feed(power_ctx.wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
		}

		/* Wait for messages from other modules - use const void *msg_data */
		const void *msg_data;
		err = zbus_sub_wait_msg(&power, &chan, &msg_data, 
					K_MSEC(CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS * 1000));
		if (err == 0) {
			/* Process message if received */
			if (chan == &POWER_CHAN) {
				/* Cast the received message data to power_msg */
				const struct power_msg *msg = (const struct power_msg *)msg_data;
				power_message_handler(msg);
			}
		} else if (err != -EAGAIN) {
			/* Only log non-timeout errors */
			LOG_DBG("zbus_sub_wait_msg timeout or error: %d", err);
		}
	}
}

/* Define power module thread with default priority */
K_THREAD_DEFINE(power_module_thread, 
		CONFIG_APP_POWER_THREAD_STACK_SIZE,
		power_thread, 
		NULL, NULL, NULL, 
		K_PRIO_COOP(7), 0, 0);
