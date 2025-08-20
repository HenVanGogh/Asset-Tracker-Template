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
#include <nrf_fuel_gauge.h>
#include <date_time.h>
#include <math.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include "lp803448_model.h"
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

/* Observe channels */
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power, 0);

#define MAX_MSG_SIZE sizeof(struct power_msg)

BUILD_ASSERT(CONFIG_APP_POWER_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* State machine states for power module */
enum power_state {
	POWER_STATE_INIT = 0,
	POWER_STATE_RUNNING,
	POWER_STATE_ERROR,
};

/* Power module context */
static struct {
	struct smf_ctx sm_ctx;
	int wdt_id;
	float battery_percentage;
	bool fuel_gauge_initialized;
} power_ctx;

/* Forward declarations */
static void state_init_entry(void *obj);
static void state_init_run(void *obj);
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_error_entry(void *obj);
static void state_error_run(void *obj);

static void power_message_handler(const void *data);

/* State machine states */
static const struct smf_state power_states[] = {
	[POWER_STATE_INIT] = SMF_CREATE_STATE(state_init_entry, state_init_run, NULL, NULL, NULL),
	[POWER_STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL, NULL, NULL),
	[POWER_STATE_ERROR] = SMF_CREATE_STATE(state_error_entry, state_error_run, NULL, NULL, NULL),
};

/* Initialize nRF fuel gauge */
static int fuel_gauge_init(void)
{
	int err;

	LOG_DBG("Initializing nRF fuel gauge");

	/* Initialize the nRF fuel gauge library */
	err = nrf_fuel_gauge_init(&lp803448_model, NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_init, error: %d", err);
		return err;
	}

	power_ctx.fuel_gauge_initialized = true;
	LOG_INF("nRF fuel gauge initialized successfully");

	return 0;
}

/* Read battery percentage from fuel gauge */
static float read_battery_percentage(void)
{
	if (!power_ctx.fuel_gauge_initialized) {
		LOG_WRN("Fuel gauge not initialized");
		return -1.0f;
	}

	/* Get battery percentage from fuel gauge */
	float percentage = nrf_fuel_gauge_process();
	
	LOG_DBG("Battery percentage: %.1f%%", percentage);
	
	/* Validate percentage range */
	if (percentage < 0.0f || percentage > 100.0f) {
		LOG_WRN("Invalid battery percentage: %.1f%%, using cached value", percentage);
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
		.percentage = percentage,
		.timestamp = timestamp
	};

	int err = zbus_chan_pub(&POWER_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	LOG_INF("Battery percentage published: %.1f%%", percentage);
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

/* State machine implementations */
static void state_init_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("Power module initializing");
}

static void state_init_run(void *obj)
{
	ARG_UNUSED(obj);

	int err = fuel_gauge_init();
	if (err) {
		LOG_ERR("Failed to initialize fuel gauge, error: %d", err);
		smf_set_state(&power_ctx.sm_ctx, &power_states[POWER_STATE_ERROR]);
		return;
	}

	/* Read initial battery percentage */
	float percentage = read_battery_percentage();
	if (percentage >= 0.0f) {
		LOG_INF("Initial battery percentage: %.1f%%", percentage);
	}

	smf_set_state(&power_ctx.sm_ctx, &power_states[POWER_STATE_RUNNING]);
}

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("Power module running");
}

static void state_running_run(void *obj)
{
	ARG_UNUSED(obj);
	/* State machine runs in message processing context */
}

static void state_error_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_ERR("Power module entered error state");
}

static void state_error_run(void *obj)
{
	ARG_UNUSED(obj);
	
	/* Try to recover by re-initializing */
	k_sleep(K_SECONDS(5));
	
	int err = fuel_gauge_init();
	if (err == 0) {
		LOG_INF("Fuel gauge recovery successful");
		smf_set_state(&power_ctx.sm_ctx, &power_states[POWER_STATE_RUNNING]);
	} else {
		LOG_ERR("Fuel gauge recovery failed, error: %d", err);
	}
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

	/* Initialize state machine */
	smf_set_initial(&power_ctx.sm_ctx, &power_states[POWER_STATE_INIT]);

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
		err = zbus_sub_wait_msg(&power, &chan, NULL, 
					K_MSEC(CONFIG_APP_POWER_MSG_PROCESSING_TIMEOUT_SECONDS * 1000));
		if (err == 0) {
			/* Process message if received */
			if (chan == &POWER_CHAN) {
				struct power_msg msg;
				err = zbus_chan_read(&POWER_CHAN, &msg, K_NO_WAIT);
				if (err == 0) {
					power_message_handler(&msg);
				} else {
					LOG_WRN("zbus_chan_read, error: %d", err);
				}
			}
		} else if (err != -EAGAIN) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
		}

		/* Run state machine */
		smf_run_state(&power_ctx.sm_ctx);
	}
}

/* Define power module thread */
K_THREAD_DEFINE(power_module_thread, 
		CONFIG_APP_POWER_THREAD_STACK_SIZE,
		power_thread, 
		NULL, NULL, NULL, 
		K_PRIO_COOP(CONFIG_APP_POWER_THREAD_PRIORITY), 0, 0);
