/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <stdio.h>

#include "uart_sensor.h"

LOG_MODULE_REGISTER(uart_sensor, CONFIG_APP_UART_SENSOR_LOG_LEVEL);

/* ZBUS channel definition for UART sensor data */
ZBUS_CHAN_DEFINE(UART_SENSOR_CHAN,
		 struct uart_sensor_msg,  /* Message type */
		 NULL,                    /* Validator */
		 NULL,                    /* User data */
		 ZBUS_OBSERVERS_EMPTY,    /* Initial observers */
		 ZBUS_MSG_INIT(0)         /* Initial message */
);

/* Current UART sensor data */
static struct uart_sensor_msg current_sensor_data = {
	.type = UART_SENSOR_DATA_RESPONSE,
	.temperature = 0.0f,
	.humidity = 0.0f,
	.probe_id = "00:00:00:00:00:00",
	.probe_battery = 0,
	.timestamp = 0
};

static bool module_initialized = false;

/* Generate realistic temperature reading (15-35°C) */
static float generate_temperature(void)
{
	/* Base temperature around 25°C with ±10°C variation */
	float base_temp = 25.0f;
	float variation = (float)(sys_rand32_get() % 2000 - 1000) / 100.0f; /* ±10°C */
	return base_temp + variation;
}

/* Generate realistic humidity reading (30-80%) */
static float generate_humidity(void)
{
	/* Base humidity around 55% with ±25% variation */
	float base_humidity = 55.0f;
	float variation = (float)(sys_rand32_get() % 5000 - 2500) / 100.0f; /* ±25% */
	float humidity = base_humidity + variation;
	
	/* Clamp to valid range */
	return CLAMP(humidity, 0.0f, 100.0f);
}

/* Generate realistic battery percentage (20-100%) */
static int generate_battery_level(void)
{
	static int last_battery = -1;
	
	if (last_battery < 0) {
		/* Initial battery level between 50-100% */
		last_battery = 50 + (sys_rand32_get() % 51);
	}
	
	/* Small random change to simulate battery drain */
	int change = (sys_rand32_get() % 5) - 2; /* -2 to +2 */
	last_battery += change;
	
	/* Clamp to reasonable range (minimum 20% for active sensor) */
	last_battery = CLAMP(last_battery, 20, 100);
	
	return last_battery;
}

/* Generate MAC-like probe ID */
static void generate_probe_id(struct uart_sensor_msg *msg)
{
	if (!msg) {
		LOG_ERR("Invalid message pointer");
		return;
	}

	/* Generate probe ID based on device characteristics */
	uint32_t rand1 = sys_rand32_get();
	uint32_t rand2 = sys_rand32_get();
	
	/* Create a probe ID using device-specific characteristics */
	snprintk(msg->probe_id, sizeof(msg->probe_id), "PROBE_%08X", 
		 (unsigned int)(rand1 ^ rand2));
}

/* Generate all sensor data for a message */
static void generate_sensor_data(struct uart_sensor_msg *msg)
{
	if (!msg) {
		LOG_ERR("Invalid message pointer");
		return;
	}
	
	msg->temperature = generate_temperature();
	msg->humidity = generate_humidity();
	msg->probe_battery = generate_battery_level();
}

/* Validate sensor data ranges */
static bool validate_sensor_data(const struct uart_sensor_msg *msg)
{
	if (!msg) {
		LOG_ERR("Invalid message pointer");
		return false;
	}
	
	bool valid = true;
	
	/* Validate temperature range */
	if (msg->temperature < -40.0f || msg->temperature > 85.0f) {
		LOG_WRN("Temperature out of expected range: %.1f°C", 
			(double)msg->temperature);
		valid = false;
	}
	
	/* Validate humidity range */
	if (msg->humidity < 0.0f || msg->humidity > 100.0f) {
		LOG_WRN("Humidity out of valid range: %.1f%%", (double)msg->humidity);
		valid = false;
	}
	
	/* Validate battery level */
	if (msg->probe_battery < 0 || msg->probe_battery > 100) {
		LOG_WRN("Battery level out of valid range: %d%%", msg->probe_battery);
		valid = false;
	}
	
	return valid;
}

/* Public API functions */
int uart_sensor_sample_request(void)
{
	if (!module_initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	LOG_DBG("UART sensor sample requested");
	
	/* Generate new sensor data */
	generate_sensor_data(&current_sensor_data);
	generate_probe_id(&current_sensor_data);
	
	/* Set timestamp and type */
	current_sensor_data.timestamp = k_uptime_get();
	current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
	
	/* Validate the generated data */
	if (!validate_sensor_data(&current_sensor_data)) {
		LOG_ERR("Generated sensor data validation failed");
		return -EINVAL;
	}
	
	LOG_INF("UART Sensor Data - T:%.1f°C, H:%.1f%%, Batt:%d%%, ID:%s",
		(double)current_sensor_data.temperature,
		(double)current_sensor_data.humidity,
		current_sensor_data.probe_battery,
		current_sensor_data.probe_id);
	
	/* Publish via ZBUS */
	int ret = zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data, K_MSEC(250));
	if (ret != 0) {
		LOG_ERR("Failed to publish UART sensor data: %d", ret);
		return ret;
	}
	
	LOG_DBG("UART sensor data published via ZBUS");
	
	return 0;
}

int uart_sensor_get_current_data(struct uart_sensor_msg *data)
{
	if (!module_initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	if (!data) {
		LOG_ERR("Invalid data pointer");
		return -EINVAL;
	}
	
	/* Copy current data */
	memcpy(data, &current_sensor_data, sizeof(struct uart_sensor_msg));
	
	return 0;
}

/* Module initialization */
static int uart_sensor_module_init(void)
{
	LOG_INF("Initializing UART sensor module");
	
	/* Initialize with sensible default values */
	current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
	current_sensor_data.temperature = 25.0f; /* Room temperature default */
	current_sensor_data.humidity = 50.0f; /* Moderate humidity default */
	current_sensor_data.probe_battery = 85; /* Good battery level default */
	strncpy(current_sensor_data.probe_id, "PROBE_INIT", sizeof(current_sensor_data.probe_id));
	current_sensor_data.timestamp = k_uptime_get();
	
	module_initialized = true;
	
	LOG_INF("UART sensor module initialized successfully");
	
	return 0;
}

/* Initialize the UART sensor module at system startup */
SYS_INIT(uart_sensor_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
