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
#include <zephyr/zbus/zbus.h>
#include <zephyr/sys/util.h>
#include <nrf_fuel_gauge.h>
#include <math.h>

#include "power.h"
#include "lp803448_model.h"

LOG_MODULE_REGISTER(power_module, CONFIG_APP_POWER_LOG_LEVEL);

/* ZBUS channel definition for power data */
ZBUS_CHAN_DEFINE(POWER_CHAN,
		 struct power_msg,     /* Message type */
		 NULL,                 /* Validator */
		 NULL,                 /* User data */
		 ZBUS_OBSERVERS_EMPTY, /* Initial observers */
		 ZBUS_MSG_INIT(0)      /* Initial message */
);

/* Charger device reference */
static const struct device *charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static int64_t ref_time;

/* Current battery data */
static struct power_msg current_power_data = {
	.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE,
	.percentage = 0.0,
	.voltage = 0.0,
	.current_ma = 0.0,
	.temperature = 0.0,
	.timestamp = 0
};

static bool module_initialized = false;
static bool fuel_gauge_initialized = false;

static int read_charger_sensors(float *voltage, float *current, float *temp)
{
	struct sensor_value value;
	int ret;
	
	if (!voltage || !current || !temp) {
		LOG_ERR("Invalid sensor parameter pointers");
		return -EINVAL;
	}
	
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
	
	/* Validate sensor readings */
	if (*voltage < 2.5f || *voltage > 5.0f) {
		LOG_WRN("Unusual voltage reading: %.3fV", (double)*voltage);
	}
	
	if (*temp < -40.0f || *temp > 85.0f) {
		LOG_WRN("Unusual temperature reading: %.1f°C", (double)*temp);
	}
	
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
	
	LOG_INF("Battery: V:%.3fV, I:%.3fmA, T:%.1f°C, SoC:%.2f%%", 
		(double)voltage, (double)(current * 1000), (double)temp, (double)soc);
	
	/* Update current power data with all sensor readings */
	current_power_data.percentage = soc;
	current_power_data.voltage = (double)voltage;
	current_power_data.current_ma = (double)(current * 1000.0f); /* Convert A to mA */
	current_power_data.temperature = (double)temp;
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
			current_power_data.percentage = 50.0; /* Default fallback */
			current_power_data.voltage = 3.7; /* Typical Li-ion voltage */
			current_power_data.current_ma = 0.0; /* Unknown current */
			current_power_data.temperature = 25.0; /* Room temperature */
			current_power_data.timestamp = k_uptime_get();
			LOG_WRN("Using fallback battery data: %.1f%%, %.2fV", 
				current_power_data.percentage, current_power_data.voltage);
		}
		return ret; /* Return error but still allow fallback data to be used */
	}
	
	/* Production-quality validation of all sensor readings */
	bool data_valid = true;
	
	/* Validate battery percentage */
	if (current_power_data.percentage < 0.0 || current_power_data.percentage > 100.0) {
		LOG_WRN("Invalid battery percentage: %.1f%%, clamping to valid range", 
			current_power_data.percentage);
		current_power_data.percentage = CLAMP(current_power_data.percentage, 0.0, 100.0);
		data_valid = false;
	}
	
	/* Validate voltage (reasonable range for Li-ion batteries) */
	if (current_power_data.voltage < 2.5 || current_power_data.voltage > 4.5) {
		LOG_WRN("Voltage out of expected range: %.3fV", current_power_data.voltage);
		data_valid = false;
	}
	
	/* Validate temperature (reasonable operating range) */
	if (current_power_data.temperature < -20.0 || current_power_data.temperature > 60.0) {
		LOG_WRN("Temperature out of expected range: %.1f°C", current_power_data.temperature);
		data_valid = false;
	}
	
	/* Validate current (reasonable range for typical applications) */
	if (fabs(current_power_data.current_ma) > 1000.0) {
		LOG_WRN("High current detected: %.1fmA", current_power_data.current_ma);
	}
	
	if (data_valid) {
		LOG_DBG("All power sensor readings validated successfully");
	}
	
	/* Publish data via ZBUS for other modules */
	ret = zbus_chan_pub(&POWER_CHAN, &current_power_data, K_MSEC(500));
	if (ret != 0) {
		LOG_WRN("Failed to publish power data via ZBUS: %d", ret);
		/* Don't fail the function, just log the warning */
	} else {
		LOG_DBG("Power data published via ZBUS: %.1f%%", current_power_data.percentage);
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
	
	/* Initialize with sensible default values */
	current_power_data.type = POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE;
	current_power_data.percentage = 50.0; /* Default until first reading */
	current_power_data.voltage = 3.7; /* Typical Li-ion voltage */
	current_power_data.current_ma = 0.0; /* Unknown current */
	current_power_data.temperature = 25.0; /* Room temperature default */
	current_power_data.timestamp = k_uptime_get();
	
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
