/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <nrf_fuel_gauge.h>

#include "power.h"

LOG_MODULE_REGISTER(power_module, CONFIG_APP_POWER_LOG_LEVEL);

/* Current battery data */
static struct power_msg current_power_data = {
	.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	.percentage = 50.0,
	.timestamp = 0
};

static bool module_initialized = false;

/* Simulate realistic battery percentage between 20% and 95% */
static int simulate_battery_percentage(void)
{
	static int last_percentage = -1;
	
	if (last_percentage < 0) {
		last_percentage = 20 + (sys_rand32_get() % 76); // 20-95%
	}
	
	/* Small random change to simulate battery drain/charge */
	int change = (sys_rand32_get() % 11) - 5; // -5 to +5
	last_percentage += change;
	
	/* Keep within realistic bounds */
	if (last_percentage < 20) last_percentage = 20;
	if (last_percentage > 95) last_percentage = 95;
	
	return last_percentage;
}

static void update_power_data(void)
{
	if (!module_initialized) {
		LOG_WRN("Power module not initialized");
		return;
	}

	/* Update battery percentage with simulation */
	current_power_data.percentage = (double)simulate_battery_percentage();
	
	/* Update timestamp */
	current_power_data.timestamp = k_uptime_get();
	
	LOG_INF("Power data updated: %.1f%%", current_power_data.percentage);
}

int power_sample_request(void)
{
	if (!module_initialized) {
		LOG_ERR("Power module not initialized");
		return -ENODEV;
	}

	LOG_DBG("Power sample requested");
	
	/* Update the power data with fresh values */
	update_power_data();
	
	return 0;
}

int power_get_current_data(struct power_msg *data)
{
	if (!module_initialized) {
		LOG_ERR("Power module not initialized");
		return -ENODEV;
	}
	
	if (data == NULL) {
		LOG_ERR("Invalid data pointer");
		return -EINVAL;
	}
	
	/* Update data before returning */
	update_power_data();
	
	/* Copy current data */
	memcpy(data, &current_power_data, sizeof(struct power_msg));
	
	return 0;
}

static int power_module_init(void)
{
	LOG_INF("Initializing simple power module");
	
	/* Initialize with default values */
	current_power_data.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE;
	current_power_data.percentage = (double)simulate_battery_percentage();
	current_power_data.timestamp = k_uptime_get();
	
	module_initialized = true;
	
	LOG_INF("Simple power module initialized successfully");
	
	return 0;
}

/* Initialize the power module at system startup */
SYS_INIT(power_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
