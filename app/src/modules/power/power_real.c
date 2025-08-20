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

/* Charger device reference */
static const struct device *charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static int64_t ref_time;

/* Battery model (using simplified values for now) */
static const struct battery_model battery_model = {
	.nrf_fuel_gauge_version_major = 1,
	.nrf_fuel_gauge_version_minor = 0,
	.capacity = 3000, /* mAh */
	.ocv = {
		3200, 3250, 3300, 3350, 3400, 3450, 3500, 3550, 3600, 3650,
		3700, 3750, 3800, 3850, 3900, 3950, 4000, 4050, 4100, 4200
	},
	.curve_offset = 0,
	.power_down_voltage_mv = 3000,
};

/* Current battery data */
static struct power_msg current_power_data = {
	.type = APP_DATA,
	.percentage = 0.0f,
	.timestamp = 0
};

static bool module_initialized = false;
static bool fuel_gauge_initialized = false;

static int read_charger_sensors(float *voltage, float *current, float *temp)
{
	struct sensor_value value;
	int ret;
	
	if (!device_is_ready(charger_dev)) {
		LOG_ERR("Charger device is not ready");
		return -ENODEV;
	}
	
	ret = sensor_sample_fetch(charger_dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch sensor samples from charger: %d", ret);
		return ret;
	}
	
	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	*voltage = sensor_value_to_float(&value);
	
	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_TEMP, &value);
	*temp = sensor_value_to_float(&value);
	
	sensor_channel_get(charger_dev, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	*current = sensor_value_to_float(&value);
	
	return 0;
}

static int fuel_gauge_init(void)
{
	struct nrf_fuel_gauge_init_parameters params = {
		.model = &battery_model,
		.opt_params = NULL,
		.state = NULL,
	};
	int ret;
	
	LOG_INF("Initializing nRF Fuel Gauge");
	
	ret = read_charger_sensors(&params.v0, &params.i0, &params.t0);
	if (ret < 0) {
		LOG_ERR("Failed to get initial sensor readings for fuel gauge: %d", ret);
		return ret;
	}
	
	ret = nrf_fuel_gauge_init(&params, NULL);
	if (ret < 0) {
		LOG_ERR("Could not initialize fuel gauge: %d", ret);
		return ret;
	}
	
	ref_time = k_uptime_get();
	fuel_gauge_initialized = true;
	LOG_INF("nRF Fuel Gauge initialized successfully");
	
	return 0;
}

static int read_battery_level(void)
{
	float voltage, current, temp, soc;
	int ret;
	
	if (!fuel_gauge_initialized) {
		LOG_WRN("Fuel gauge not initialized, attempting initialization");
		ret = fuel_gauge_init();
		if (ret < 0) {
			return ret;
		}
	}
	
	ret = read_charger_sensors(&voltage, &current, &temp);
	if (ret < 0) {
		LOG_ERR("Failed to read charger sensors: %d", ret);
		return ret;
	}
	
	float delta_s = (float)k_uptime_delta(&ref_time) / 1000.0f;
	soc = nrf_fuel_gauge_process(voltage, current, temp, delta_s, NULL);
	
	if (soc < 0) {
		LOG_ERR("Error processing fuel gauge: %d", (int)soc);
		return (int)soc;
	}
	
	LOG_INF("Battery: V:%.3fV, I:%.3fmA, SoC:%.2f%%", 
		(double)voltage, (double)(current * 1000), (double)soc);
	
	/* Update current power data */
	current_power_data.percentage = soc;
	current_power_data.timestamp = k_uptime_get();
	
	return 0;
}

int power_sample_request(void)
{
	if (!module_initialized) {
		LOG_ERR("Power module not initialized");
		return -ENODEV;
	}

	LOG_DBG("Power sample requested");
	
	int ret = read_battery_level();
	if (ret < 0) {
		LOG_ERR("Failed to read battery level: %d", ret);
		/* Fall back to previous value or default */
		if (current_power_data.timestamp == 0) {
			current_power_data.percentage = 50.0f; /* Default fallback */
			current_power_data.timestamp = k_uptime_get();
		}
	}
	
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
	
	/* Ensure we have fresh data */
	power_sample_request();
	
	/* Copy current data */
	memcpy(data, &current_power_data, sizeof(struct power_msg));
	
	return 0;
}

static int power_module_init(void)
{
	LOG_INF("Initializing power module");
	
	/* Initialize with default values */
	current_power_data.type = APP_DATA;
	current_power_data.timestamp = k_uptime_get();
	current_power_data.percentage = 50.0f; /* Default until first reading */
	
	module_initialized = true;
	
	/* Try to initialize fuel gauge, but don't fail if it doesn't work initially */
	int ret = fuel_gauge_init();
	if (ret < 0) {
		LOG_WRN("Failed to initialize fuel gauge on startup: %d", ret);
		LOG_WRN("Will retry on first sample request");
	}
	
	LOG_INF("Power module initialized successfully");
	
	return 0;
}

/* Initialize the power module at system startup */
SYS_INIT(power_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
