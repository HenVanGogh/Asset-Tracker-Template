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
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/pm/device.h>
#include <string.h>
#include <stdio.h>

#include "uart_sensor.h"

LOG_MODULE_REGISTER(uart_sensor, CONFIG_APP_UART_SENSOR_LOG_LEVEL);

/* UART device reference */
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* UART message queue and buffers */
K_MSGQ_DEFINE(uart_msgq, UART_RX_BUF_SIZE, 10, 4);
K_SEM_DEFINE(uart_wake_sem, 0, 1);

static char line_buf[UART_RX_BUF_SIZE];
static int line_buf_pos = 0;

/* Background processing thread */
#define UART_THREAD_STACK_SIZE 2048
#define UART_THREAD_PRIORITY   5

static K_THREAD_STACK_DEFINE(uart_thread_stack, UART_THREAD_STACK_SIZE);
static struct k_thread uart_thread_data;
static k_tid_t uart_thread_tid;

/* Interrupt-driven UART callback */
static void uart_isr_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	
	if (!uart_irq_update(dev)) {
		LOG_DBG("No UART interrupt pending");
		return;
	}
	
	/* Signal that UART activity occurred - this wakes up the system */
	k_sem_give(&uart_wake_sem);
	
	/* Handle errors first */
	if (uart_irq_tx_complete(dev)) {
		LOG_DBG("UART TX complete");
		/* We don't use TX, so this shouldn't happen normally */
	}
	
	if (uart_irq_rx_ready(dev)) {
		int recv_len;
		uint8_t buffer[64];
		
		recv_len = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (recv_len < 0) {
			LOG_ERR("Failed to read UART FIFO: %d", recv_len);
			return;
		}
		
		if (recv_len == 0) {
			LOG_DBG("No data in UART FIFO");
			return;
		}
		
		LOG_DBG("UART received %d bytes", recv_len);
		
		for (int i = 0; i < recv_len; i++) {
			uint8_t c = buffer[i];
			
			if ((c == '\n' || c == '\r') && line_buf_pos > 0) {
				line_buf[line_buf_pos] = '\0';
				/* Try to put the line in the message queue */
				if (k_msgq_put(&uart_msgq, &line_buf, K_NO_WAIT) != 0) {
					LOG_WRN("UART message queue full, dropping line: %s", line_buf);
				} else {
					LOG_DBG("UART line queued: %s", line_buf);
				}
				line_buf_pos = 0;
			} else if (c >= ' ' && line_buf_pos < (sizeof(line_buf) - 1)) {
				line_buf[line_buf_pos++] = c;
			}
			/* Ignore control characters and overflow */
		}
	}
}

/* Helper function to convert battery voltage (mV) to percentage */
static int convert_mv_to_percent(uint32_t mv)
{
	const uint32_t min_mv = 3000;  /* 3.0V = 0% */
	const uint32_t max_mv = 4200;  /* 4.2V = 100% */

	if (mv >= max_mv) {
		return 100;
	}
	if (mv <= min_mv) {
		return 0;
	}

	/* Calculate percentage within the range */
	return (int)(((mv - min_mv) * 100) / (max_mv - min_mv));
}

/* Helper function to format sensor name into MAC-style hex ID */
static void format_probe_id(const char *original_name, char *formatted_id, size_t formatted_id_size)
{
	const char *prefix = "nRF_52840_";
	const char *name_to_format = original_name;
	size_t prefix_len = strlen(prefix);

	/* Check if the name starts with the prefix and advance the pointer if it does */
	if (strncmp(original_name, prefix, prefix_len) == 0) {
		name_to_format = original_name + prefix_len;
	}

	int name_len = strlen(name_to_format);
	int out_pos = 0;

	/* Ensure the output buffer is clean */
	memset(formatted_id, 0, formatted_id_size);

	/* Format the first 16 characters of the remaining name into a MAC-like address */
	for (int i = 0; i < 16; i++) {
		/* Use the character from the name, or 0 if the name is shorter */
		uint8_t char_to_convert = (i < name_len) ? name_to_format[i] : 0;
		
		/* Append the 2-digit hex value and a colon (if not the last one) */
		out_pos += snprintf(&formatted_id[out_pos], formatted_id_size - out_pos,
				  "%02X%s", char_to_convert, (i < 15) ? ":" : "");
	}
}

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
		LOG_WRN("Battery level out of valid range: %.1f%%", (double)msg->probe_battery);
		valid = false;
	}
	
	return valid;
}

/* UART processing thread - continuously monitors for incoming data */
static void uart_processing_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	
	char rx_buf[UART_RX_BUF_SIZE];
	
	LOG_INF("UART processing thread started");
	
	while (1) {
		/* Wait for UART wake-up signal */
		k_sem_take(&uart_wake_sem, K_FOREVER);
		LOG_DBG("Woke up from UART activity");
		
		/* Process all pending UART messages */
		while (k_msgq_get(&uart_msgq, &rx_buf, K_NO_WAIT) == 0) {
			LOG_DBG("UART data received: %s", rx_buf);
			
			/* Process the received data line */
			int err = uart_sensor_process_data_line(rx_buf);
			if (err == 0) {
				/* Data processed successfully - it's already published via ZBUS 
				 * in uart_sensor_process_data_line() */
				LOG_INF("UART beacon data processed and published to MQTT");
			} else {
				LOG_WRN("Failed to process UART data: %s (error: %d)", rx_buf, err);
			}
		}
	}
}

/* UART initialization function */
int uart_sensor_init(void)
{
	int err;
	
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	
	LOG_INF("UART device found and ready");
	
	/* Ensure device is powered on */
	err = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		LOG_WRN("Failed to resume UART device: %d (continuing)", err);
	}
	
	/* Give the UART some time to stabilize */
	k_msleep(10);
	
	/* Configure UART for wake-up capability */
	err = pm_device_wakeup_enable(uart_dev, true);
	if (err) {
		LOG_WRN("Failed to enable UART wake-up, error: %d (continuing anyway)", err);
		/* Continue anyway - some platforms might not support this */
	}
	
	/* Clear any pending interrupts */
	uart_irq_update(uart_dev);
	
	/* Set the interrupt-driven callback */
	uart_irq_callback_user_data_set(uart_dev, uart_isr_callback, NULL);
	
	/* Disable all interrupts first */
	uart_irq_tx_disable(uart_dev);
	uart_irq_rx_disable(uart_dev);
	
	/* Clear FIFO */
	while (uart_irq_rx_ready(uart_dev)) {
		uint8_t dummy;
		uart_fifo_read(uart_dev, &dummy, 1);
	}
	
	/* Enable RX interrupt only */
	uart_irq_rx_enable(uart_dev);
	
	LOG_INF("UART interrupt-driven handler initialized and listening on UART1");
	return 0;
}

/* Parse UART data line and update sensor data */
int uart_sensor_process_data_line(const char *data)
{
	char sensor_name[32];
	float temperature, humidity;
	uint32_t probe_battery_mv = 0;
	int items_parsed;

	if (!data) {
		LOG_ERR("Invalid UART data pointer");
		return -EINVAL;
	}

	/* Use sscanf to parse the "name:temp,hum,batt_mv" format */
	items_parsed = sscanf(data, "%31[^:]:%f,%f,%u", sensor_name, &temperature, &humidity,
			      &probe_battery_mv);

	if (items_parsed == 4) {
		LOG_INF("Parsed UART data: Name=%s, Temp=%.1f°C, Hum=%.1f%%, Batt=%umV", 
			sensor_name, (double)temperature, (double)humidity, probe_battery_mv);

		/* Format the sensor name into a MAC-like hex ID */
		char formatted_probe_id[48];
		format_probe_id(sensor_name, formatted_probe_id, sizeof(formatted_probe_id));

		/* Update current sensor data */
		current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
		current_sensor_data.temperature = temperature;
		current_sensor_data.humidity = humidity;
		current_sensor_data.probe_battery = (float)convert_mv_to_percent(probe_battery_mv);
		strncpy(current_sensor_data.probe_id, formatted_probe_id, 
			sizeof(current_sensor_data.probe_id) - 1);
		current_sensor_data.probe_id[sizeof(current_sensor_data.probe_id) - 1] = '\0';
		current_sensor_data.timestamp = k_uptime_get();

		LOG_INF("Updated sensor data: ID=%s, T=%.1f°C, H=%.1f%%, Bat=%.1f%%", 
			current_sensor_data.probe_id, (double)current_sensor_data.temperature, 
			(double)current_sensor_data.humidity, (double)current_sensor_data.probe_battery);

		/* Publish the data via ZBUS */
		int err = zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data, K_SECONDS(1));
		if (err) {
			LOG_ERR("Failed to publish UART sensor data: %d", err);
			return err;
		}

		return 0;
	} else {
		LOG_WRN("Failed to parse UART data: '%s'. Parsed %d items, expected 4", 
			data, items_parsed);
		return -EINVAL;
	}
}

/* Public API functions */

/* Check UART status and configuration */
int uart_sensor_check_status(void)
{
	if (!module_initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	
	LOG_INF("UART sensor status: device ready, interrupts enabled");
	
	/* Check if there's any pending data */
	if (uart_irq_rx_ready(uart_dev)) {
		LOG_INF("UART has pending RX data");
	} else {
		LOG_INF("UART RX FIFO empty");
	}
	
	return 0;
}

int uart_sensor_sample_request(void)
{
	if (!module_initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	LOG_DBG("UART sensor sample requested - data is processed automatically via background thread");
	
	/* Since we have a background thread processing UART data automatically,
	 * this function now just publishes the current/latest data */
	current_sensor_data.timestamp = k_uptime_get();
	
	/* Publish the current data via ZBUS */
	int ret = zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data, K_MSEC(250));
	if (ret != 0) {
		LOG_ERR("Failed to publish UART sensor data: %d", ret);
		return ret;
	}
	
	LOG_INF("Published latest UART sensor data - T:%.1f°C, H:%.1f%%, Bat:%.1f%%, ID:%s",
		(double)current_sensor_data.temperature,
		(double)current_sensor_data.humidity,
		(double)current_sensor_data.probe_battery,
		current_sensor_data.probe_id);
	
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
	int err;
	
	LOG_INF("Initializing UART sensor module");
	
	/* Check if UART device is ready */
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	
	/* Initialize UART interrupt-driven operation */
	err = uart_sensor_init();
	if (err) {
		LOG_ERR("Failed to initialize UART communication: %d", err);
		return err;
	}
	
	/* Initialize with sensible default values */
	current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
	current_sensor_data.temperature = 25.0f; /* Room temperature default */
	current_sensor_data.humidity = 50.0f; /* Moderate humidity default */
	current_sensor_data.probe_battery = 85.0f; /* Good battery level default */
	strncpy(current_sensor_data.probe_id, "PROBE_INIT", sizeof(current_sensor_data.probe_id));
	current_sensor_data.timestamp = k_uptime_get();
	
	/* Start the background UART processing thread */
	uart_thread_tid = k_thread_create(&uart_thread_data, uart_thread_stack,
					  K_THREAD_STACK_SIZEOF(uart_thread_stack),
					  uart_processing_thread, NULL, NULL, NULL,
					  UART_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!uart_thread_tid) {
		LOG_ERR("Failed to create UART processing thread");
		return -ENOMEM;
	}
	
	k_thread_name_set(uart_thread_tid, "uart_sensor");
	
	module_initialized = true;
	
	LOG_INF("UART sensor module initialized successfully");
	
	return 0;
}

/* Initialize the UART sensor module at system startup */
SYS_INIT(uart_sensor_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
