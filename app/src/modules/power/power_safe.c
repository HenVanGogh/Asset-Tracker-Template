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
		 ZBUS_MSG_INIT(.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE, .percentage = 50.0, .timestamp = 0)
);

/* Add observer */
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power, 0);

/* Power module context */
static struct {
	int wdt_id;
	float battery_percentage;
	bool initialized;
	struct k_mutex mutex;
} power_ctx = {
	.battery_percentage = 85.0f,
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

/* Read battery percentage safely */
static float read_battery_percentage(void)
{
	k_mutex_lock(&power_ctx.mutex, K_FOREVER);
	
	float percentage = simulate_battery_percentage();
	
	LOG_DBG("Battery percentage: %.1f%%", (double)percentage);
	
	/* Validate percentage range */
	if (percentage >= 0.0f && percentage <= 100.0f) {
		power_ctx.battery_percentage = percentage;
	}
	
	float result = power_ctx.battery_percentage;
	k_mutex_unlock(&power_ctx.mutex);
	
	return result;
}

/* Send power response message safely */
static int send_power_response(float percentage, int64_t timestamp)
{
	struct power_msg msg = {
		.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
		.percentage = (double)percentage,
		.timestamp = timestamp
	};
	
	int err = zbus_chan_pub(&POWER_CHAN, &msg, K_MSEC(1000));
	if (err) {
		LOG_ERR("Failed to publish power response: %d", err);
		return err;
	}
	
	LOG_DBG("Published power response: %.1f%% at timestamp %" PRId64, 
		(double)percentage, timestamp);
	return 0;
}

/* Handle power messages safely */
static void power_message_handler(const struct power_msg *msg)
{
	if (!msg) {
		LOG_ERR("Received NULL message");
		return;
	}
	
	LOG_DBG("Processing power message type: %d", msg->type);
	
	switch (msg->type) {
	case POWER_BATTERY_PERCENTAGE_SAMPLE_REQUEST: {
		int64_t timestamp;
		int err = date_time_now(&timestamp);
		if (err) {
			LOG_WRN("Failed to get timestamp: %d", err);
			timestamp = k_uptime_get();
		}
		
		float percentage = read_battery_percentage();
		
		LOG_INF("Power sample requested, responding with %.1f%%", (double)percentage);
		
		err = send_power_response(percentage, timestamp);
		if (err) {
			LOG_ERR("Failed to send power response: %d", err);
		}
		break;
	}
	case POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE:
		/* This is our own response, ignore it to avoid loops */
		LOG_DBG("Ignoring own power response message");
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
	
	/* Initialize mutex */
	k_mutex_init(&power_ctx.mutex);
	
	/* Get initial battery reading */
	float initial_percentage = read_battery_percentage();
	LOG_INF("Initial battery percentage: %.1f%%", (double)initial_percentage);
	
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

		/* Wait for messages */
		const void *msg_data;
		err = zbus_sub_wait_msg(&power, &chan, &msg_data, K_SECONDS(3));
		if (err == 0) {
			/* Message received */
			if (chan == &POWER_CHAN) {
				const struct power_msg *msg = (const struct power_msg *)msg_data;
				power_message_handler(msg);
			} else {
				LOG_WRN("Received message from unexpected channel");
			}
		} else if (err != -EAGAIN) {
			LOG_DBG("zbus_sub_wait_msg error: %d", err);
		}
		
		/* Small delay to prevent tight loop */
		k_msleep(10);
	}
}

/* Define power module thread */
K_THREAD_DEFINE(power_module_thread, 
		CONFIG_APP_POWER_THREAD_STACK_SIZE,
		power_thread, 
		NULL, NULL, NULL, 
		K_PRIO_COOP(7), 0, 0);
