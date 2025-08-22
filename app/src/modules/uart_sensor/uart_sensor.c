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

/* UART device reference - configurable via device tree */
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* UART message queue and buffers - sizes configurable via Kconfig */
K_MSGQ_DEFINE(uart_msgq, UART_RX_BUF_SIZE, UART_MSG_QUEUE_SIZE, 4);
K_SEM_DEFINE(uart_wake_sem, 0, 1);
K_MUTEX_DEFINE(uart_data_mutex);

static char line_buf[UART_LINE_BUF_SIZE];
static int line_buf_pos = 0;

/* Background processing thread */
static K_THREAD_STACK_DEFINE(uart_thread_stack, UART_THREAD_STACK_SIZE);
static struct k_thread uart_thread_data;
static k_tid_t uart_thread_tid;

/* Module state and statistics */
static struct {
	bool initialized;
	bool auto_publish_enabled;
	struct uart_sensor_stats stats;
	uint32_t consecutive_errors;
	int64_t last_recovery_time;
	struct uart_sensor_msg last_valid_data;
} uart_module_state = {
	.initialized = false,
	.auto_publish_enabled = true,
	.consecutive_errors = 0,
	.last_recovery_time = 0,
};

/* Forward declarations */
static int uart_device_setup(void);
static int uart_device_recovery(void);
static void uart_error_handler(enum uart_sensor_error_type error_type, const char *details);
static bool validate_probe_id(const char *probe_id);
static void publish_error_message(enum uart_sensor_error_type error_type, const char *details);
static void update_statistics(bool parse_success, bool validation_success, bool publish_success);

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
	
	/* Handle TX complete (not used in current implementation) */
	if (uart_irq_tx_complete(dev)) {
		LOG_DBG("UART TX complete");
	}
	
	/* Handle RX data */
	if (uart_irq_rx_ready(dev)) {
		int recv_len;
		uint8_t buffer[64];
		
		recv_len = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (recv_len < 0) {
			LOG_ERR("Failed to read UART FIFO: %d", recv_len);
			uart_error_handler(UART_SENSOR_ERROR_COMM_TIMEOUT, "FIFO read failed");
			return;
		}
		
		if (recv_len == 0) {
			LOG_DBG("No data in UART FIFO");
			return;
		}
		
		LOG_DBG("UART received %d bytes", recv_len);
		
		/* Process received bytes and assemble complete lines */
		for (int i = 0; i < recv_len; i++) {
			uint8_t c = buffer[i];
			
			/* Check for line termination characters */
			bool is_line_end = false;
			const char *delim = UART_MESSAGE_DELIMITER;
			for (int j = 0; delim[j] != '\0'; j++) {
				if (c == delim[j]) {
					is_line_end = true;
					break;
				}
			}
			
			if (is_line_end && line_buf_pos > 0) {
				/* Complete line received - null terminate and queue for processing */
				line_buf[line_buf_pos] = '\0';
				
				if (k_msgq_put(&uart_msgq, line_buf, K_NO_WAIT) != 0) {
					LOG_WRN("UART message queue full, dropping message: %s", line_buf);
					uart_error_handler(UART_SENSOR_ERROR_BUFFER_OVERFLOW, "Message queue full");
				} else {
					LOG_DBG("Queued UART message: %s", line_buf);
				}
				
				line_buf_pos = 0;
			} else if (c >= ' ' && c <= '~' && line_buf_pos < (sizeof(line_buf) - 1)) {
				/* Printable character - add to line buffer */
				line_buf[line_buf_pos++] = c;
			}
			/* Ignore control characters and handle buffer overflow */
		}
	}
}

/* Helper function to convert battery voltage (mV) to percentage */
static int convert_mv_to_percent(uint32_t mv)
{
	const uint32_t min_mv = UART_BATTERY_MIN_MV;
	const uint32_t max_mv = UART_BATTERY_MAX_MV;

	if (mv >= max_mv) {
		return 100;
	}
	if (mv <= min_mv) {
		return 0;
	}

	/* Calculate percentage within the range */
	return (int)(((mv - min_mv) * 100) / (max_mv - min_mv));
}

/* Helper function to validate and sanitize probe ID */
static bool validate_probe_id(const char *probe_id)
{
	if (!probe_id) {
		return false;
	}
	
	size_t len = strlen(probe_id);
	
	/* Check length constraints */
	if (len < UART_PROBE_ID_MIN_LEN || len > UART_PROBE_ID_MAX_LEN) {
		LOG_WRN("Probe ID length %zu out of range [%d, %d]: %s", 
			len, UART_PROBE_ID_MIN_LEN, UART_PROBE_ID_MAX_LEN, probe_id);
		return false;
	}
	
	/* Check for valid characters (printable ASCII) */
	for (size_t i = 0; i < len; i++) {
		if (probe_id[i] < ' ' || probe_id[i] > '~') {
			LOG_WRN("Invalid character in probe ID at position %zu: 0x%02X", i, probe_id[i]);
			return false;
		}
	}
	
	return true;
}

/* Error handling function */
static void uart_error_handler(enum uart_sensor_error_type error_type, const char *details)
{
	uart_module_state.consecutive_errors++;
	uart_module_state.stats.last_error_time = k_uptime_get();
	
	LOG_ERR("UART sensor error (type %d): %s", error_type, details ? details : "Unknown");
	
	/* Update error statistics */
	switch (error_type) {
	case UART_SENSOR_ERROR_PARSE_FAILED:
		uart_module_state.stats.parse_errors++;
		break;
	case UART_SENSOR_ERROR_INVALID_DATA:
		uart_module_state.stats.validation_errors++;
		break;
	case UART_SENSOR_ERROR_COMM_TIMEOUT:
		uart_module_state.stats.comm_errors++;
		break;
	default:
		break;
	}
	
#ifdef CONFIG_APP_UART_SENSOR_ERROR_RECOVERY
	/* Trigger recovery if too many consecutive errors */
	if (uart_module_state.consecutive_errors >= UART_MAX_CONSECUTIVE_ERRORS) {
		int64_t now = k_uptime_get();
		if (now - uart_module_state.last_recovery_time > UART_RECOVERY_DELAY_MS) {
			LOG_WRN("Too many consecutive errors (%u), attempting recovery", 
				uart_module_state.consecutive_errors);
			uart_device_recovery();
			uart_module_state.last_recovery_time = now;
		}
	}
#endif

	/* Publish error message if auto-publish is enabled */
	if (uart_module_state.auto_publish_enabled) {
		publish_error_message(error_type, details);
	}
}

/* Publish error message via ZBUS */
static void publish_error_message(enum uart_sensor_error_type error_type, const char *details)
{
	struct uart_sensor_msg error_msg = {
		.type = UART_SENSOR_ERROR_RESPONSE,
		.error_type = error_type,
		.timestamp = k_uptime_get(),
	};
	
	/* Copy error details safely */
	if (details) {
		strncpy(error_msg.error_details, details, sizeof(error_msg.error_details) - 1);
		error_msg.error_details[sizeof(error_msg.error_details) - 1] = '\0';
	}
	
	int ret = zbus_chan_pub(&UART_SENSOR_CHAN, &error_msg, K_MSEC(UART_ZBUS_TIMEOUT_MS));
	if (ret != 0) {
		LOG_ERR("Failed to publish error message: %d", ret);
		uart_module_state.stats.publish_errors++;
	}
}

/* Update module statistics */
static void update_statistics(bool parse_success, bool validation_success, bool publish_success)
{
	uart_module_state.stats.messages_received++;
	
	if (parse_success) {
		uart_module_state.stats.messages_parsed++;
		uart_module_state.consecutive_errors = 0; /* Reset on success */
	}
	
	if (!validation_success) {
		uart_module_state.stats.validation_errors++;
	}
	
	if (!publish_success) {
		uart_module_state.stats.publish_errors++;
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
	.probe_id = UART_DEFAULT_PROBE_ID,
	.probe_battery = 0.0f,
	.timestamp = 0,
	.error_type = UART_SENSOR_ERROR_NONE,
	.data_quality = {
		.temperature_valid = false,
		.humidity_valid = false,
		.battery_valid = false,
		.probe_id_valid = false,
	}
};

/* Validate sensor data ranges */
bool uart_sensor_validate_data(const struct uart_sensor_msg *msg)
{
	if (!msg) {
		LOG_ERR("Invalid message pointer");
		return false;
	}
	
#ifdef CONFIG_APP_UART_SENSOR_DATA_VALIDATION
	bool valid = true;
	
	/* Validate temperature range */
	if (msg->temperature < UART_TEMP_MIN || msg->temperature > UART_TEMP_MAX) {
		LOG_WRN("Temperature %.1f°C out of range [%d, %d]", 
			(double)msg->temperature, UART_TEMP_MIN, UART_TEMP_MAX);
		valid = false;
	}
	
	/* Validate humidity range */
	if (msg->humidity < UART_HUMIDITY_MIN || msg->humidity > UART_HUMIDITY_MAX) {
		LOG_WRN("Humidity %.1f%% out of range [%d, %d]", 
			(double)msg->humidity, UART_HUMIDITY_MIN, UART_HUMIDITY_MAX);
		valid = false;
	}
	
	/* Validate battery level */
	if (msg->probe_battery < 0.0f || msg->probe_battery > 100.0f) {
		LOG_WRN("Battery level %.1f%% out of valid range [0, 100]", 
			(double)msg->probe_battery);
		valid = false;
	}
	
	/* Validate probe ID */
	if (!validate_probe_id(msg->probe_id)) {
		LOG_WRN("Invalid probe ID: %s", msg->probe_id);
		valid = false;
	}
	
	return valid;
#else
	/* Validation disabled - always return true */
	return true;
#endif
}

/* UART processing thread - continuously monitors for incoming data */
static void uart_processing_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	
	char rx_buf[UART_RX_BUF_SIZE];
	
	LOG_INF("UART processing thread started (priority %d, stack %d bytes)", 
		UART_THREAD_PRIORITY, UART_THREAD_STACK_SIZE);
	
	/* Initialize statistics */
	uart_module_state.stats.uptime_ms = k_uptime_get();
	
	while (1) {
		/* Wait for UART wake-up signal */
		if (k_sem_take(&uart_wake_sem, K_MSEC(UART_PROCESSING_TIMEOUT_MS)) == 0) {
			LOG_DBG("Woke up from UART activity");
		} else {
			/* Timeout - update uptime and continue */
			uart_module_state.stats.uptime_ms = k_uptime_get();
			continue;
		}
		
		/* Process all pending UART messages */
		while (k_msgq_get(&uart_msgq, &rx_buf, K_NO_WAIT) == 0) {
			LOG_DBG("Processing UART data: %s", rx_buf);
			
			/* Process the received line */
			int ret = uart_sensor_process_data_line(rx_buf);
			
			/* Update statistics based on processing result */
			bool parse_success = (ret == 0);
			update_statistics(parse_success, true, true);
			
			if (ret != 0) {
				LOG_WRN("Failed to process UART data line: %d", ret);
			}
		}
		
		/* Update uptime */
		uart_module_state.stats.uptime_ms = k_uptime_get();
	}
}

/* UART device setup function */
static int uart_device_setup(void)
{
	int err;
	
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	
	LOG_DBG("UART device found and ready");
	
#ifdef CONFIG_APP_UART_SENSOR_POWER_MANAGEMENT
	/* Ensure device is powered on */
	err = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
	if (err && err != -EALREADY) {
		LOG_WRN("Failed to resume UART device: %d (continuing)", err);
	}
	
	/* Configure UART for wake-up capability */
	err = pm_device_wakeup_enable(uart_dev, true);
	if (err) {
		LOG_WRN("Failed to enable UART wake-up, error: %d (continuing anyway)", err);
		/* Continue anyway - some platforms might not support this */
	}
#endif
	
	/* Give the UART some time to stabilize */
	k_msleep(CONFIG_APP_UART_SENSOR_DEVICE_SETTLE_DELAY_MS);
	
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
	
	LOG_INF("UART interrupt-driven handler initialized and listening");
	return 0;
}

#ifdef CONFIG_APP_UART_SENSOR_ERROR_RECOVERY
/* UART device recovery function */
static int uart_device_recovery(void)
{
	LOG_INF("Attempting UART device recovery");
	
	/* Disable interrupts */
	uart_irq_rx_disable(uart_dev);
	uart_irq_tx_disable(uart_dev);
	
	/* Reset error counters */
	uart_module_state.consecutive_errors = 0;
	
	/* Re-setup the device */
	int ret = uart_device_setup();
	if (ret == 0) {
		LOG_INF("UART device recovery successful");
	} else {
		LOG_ERR("UART device recovery failed: %d", ret);
	}
	
	return ret;
}
#endif

/* UART initialization function */
int uart_sensor_init(void)
{
	if (uart_module_state.initialized) {
		LOG_WRN("UART sensor module already initialized");
		return 0;
	}
	
	LOG_INF("Initializing UART sensor module");
	
	/* Setup UART device */
	int err = uart_device_setup();
	if (err) {
		LOG_ERR("Failed to setup UART device: %d", err);
		return err;
	}
	
	uart_module_state.initialized = true;
	uart_module_state.auto_publish_enabled = CONFIG_APP_UART_SENSOR_AUTO_PUBLISH;
	
	LOG_INF("UART sensor module initialized successfully");
	return 0;
}

/* Parse UART data line and update sensor data */
int uart_sensor_process_data_line(const char *data)
{
	char sensor_name[CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN + 1];
	float temperature, humidity;
	uint32_t probe_battery_mv = 0;
	int items_parsed;
	bool parse_success = false;
	bool validation_success = false;
	bool publish_success = false;

	if (!data) {
		LOG_ERR("Invalid UART data pointer");
		uart_error_handler(UART_SENSOR_ERROR_PARSE_FAILED, "Null data pointer");
		return -EINVAL;
	}

	/* Parse using configurable separators - format: "name:temp,hum,batt_mv" */
	const char *field_sep = UART_FIELD_SEPARATOR;
	const char *value_sep = UART_VALUE_SEPARATOR;
	
	/* Build format string dynamically based on configuration */
	char format_str[64];
	snprintf(format_str, sizeof(format_str), "%%%d[^%s]%s%%f%s%%f%s%%u",
		CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN, field_sep, field_sep, value_sep, value_sep);

	items_parsed = sscanf(data, format_str, sensor_name, &temperature, &humidity, &probe_battery_mv);

	if (items_parsed == 4) {
		parse_success = true;
		LOG_INF("Parsed UART data: Name=%s, Temp=%.1f°C, Hum=%.1f%%, Batt=%umV", 
			sensor_name, (double)temperature, (double)humidity, probe_battery_mv);

		/* Validate probe ID */
		if (!validate_probe_id(sensor_name)) {
			LOG_WRN("Invalid probe ID, using default: %s", UART_DEFAULT_PROBE_ID);
			strncpy(sensor_name, UART_DEFAULT_PROBE_ID, sizeof(sensor_name) - 1);
			sensor_name[sizeof(sensor_name) - 1] = '\0';
		}

		/* Update current sensor data with thread safety */
		k_mutex_lock(&uart_data_mutex, K_FOREVER);
		
		current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
		current_sensor_data.temperature = temperature;
		current_sensor_data.humidity = humidity;
		current_sensor_data.probe_battery = (float)convert_mv_to_percent(probe_battery_mv);
		current_sensor_data.error_type = UART_SENSOR_ERROR_NONE;
		current_sensor_data.timestamp = k_uptime_get();
		
		/* Use probe name as-is (no formatting to MAC-style) */
		strncpy(current_sensor_data.probe_id, sensor_name, 
			sizeof(current_sensor_data.probe_id) - 1);
		current_sensor_data.probe_id[sizeof(current_sensor_data.probe_id) - 1] = '\0';
		
		/* Clear error details */
		memset(current_sensor_data.error_details, 0, sizeof(current_sensor_data.error_details));
		
		/* Validate sensor data */
		validation_success = uart_sensor_validate_data(&current_sensor_data);
		
		/* Update data quality indicators */
#ifdef CONFIG_APP_UART_SENSOR_DATA_VALIDATION
		current_sensor_data.data_quality.temperature_valid = 
			(temperature >= UART_TEMP_MIN && temperature <= UART_TEMP_MAX);
		current_sensor_data.data_quality.humidity_valid = 
			(humidity >= UART_HUMIDITY_MIN && humidity <= UART_HUMIDITY_MAX);
#else
		current_sensor_data.data_quality.temperature_valid = true;
		current_sensor_data.data_quality.humidity_valid = true;
#endif
		current_sensor_data.data_quality.battery_valid = 
			(current_sensor_data.probe_battery >= 0.0f && current_sensor_data.probe_battery <= 100.0f);
		current_sensor_data.data_quality.probe_id_valid = validate_probe_id(sensor_name);
		
		/* Store as last valid data */
		memcpy(&uart_module_state.last_valid_data, &current_sensor_data, sizeof(current_sensor_data));
		
		k_mutex_unlock(&uart_data_mutex);

		LOG_INF("Updated sensor data: ID=%s, T=%.1f°C, H=%.1f%%, Bat=%.1f%%", 
			current_sensor_data.probe_id, (double)current_sensor_data.temperature, 
			(double)current_sensor_data.humidity, (double)current_sensor_data.probe_battery);

#ifdef CONFIG_APP_UART_SENSOR_REJECT_INVALID_DATA
		if (!validation_success) {
			LOG_WRN("Rejecting invalid sensor data");
			uart_error_handler(UART_SENSOR_ERROR_VALIDATION_FAILED, "Data validation failed");
			update_statistics(parse_success, validation_success, false);
			return -EINVAL;
		}
#endif

		/* Publish the data via ZBUS if auto-publish is enabled */
		if (uart_module_state.auto_publish_enabled) {
			int err = zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data, 
						K_MSEC(UART_ZBUS_TIMEOUT_MS));
			if (err) {
				LOG_ERR("Failed to publish UART sensor data: %d", err);
				uart_error_handler(UART_SENSOR_ERROR_COMM_TIMEOUT, "ZBUS publish failed");
				publish_success = false;
			} else {
				publish_success = true;
				LOG_DBG("UART sensor data published successfully");
			}
		} else {
			publish_success = true; /* Consider successful if auto-publish is disabled */
		}

		update_statistics(parse_success, validation_success, publish_success);
		return 0;
	} else {
		LOG_WRN("Failed to parse UART data: '%s'. Parsed %d items, expected 4", 
			data, items_parsed);
		uart_error_handler(UART_SENSOR_ERROR_PARSE_FAILED, "Invalid data format");
		update_statistics(false, false, false);
		return -EINVAL;
	}
}

/* Public API functions */

/* Check UART status and configuration */
int uart_sensor_check_status(void)
{
	if (!uart_module_state.initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	
	LOG_INF("UART sensor status: device ready, interrupts enabled");
	LOG_INF("Auto-publish: %s, Messages processed: %u, Errors: %u", 
		uart_module_state.auto_publish_enabled ? "enabled" : "disabled",
		uart_module_state.stats.messages_parsed,
		uart_module_state.stats.parse_errors + uart_module_state.stats.validation_errors);
	
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
	if (!uart_module_state.initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	LOG_DBG("UART sensor sample requested - publishing latest data");
	
	/* Update timestamp and publish current data */
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data.timestamp = k_uptime_get();
	struct uart_sensor_msg data_to_publish = current_sensor_data;
	k_mutex_unlock(&uart_data_mutex);
	
	/* Publish the current data via ZBUS */
	int ret = zbus_chan_pub(&UART_SENSOR_CHAN, &data_to_publish, K_MSEC(UART_ZBUS_TIMEOUT_MS));
	if (ret != 0) {
		LOG_ERR("Failed to publish UART sensor data: %d", ret);
		uart_module_state.stats.publish_errors++;
		return ret;
	}
	
	LOG_INF("Published latest UART sensor data - T:%.1f°C, H:%.1f%%, Bat:%.1f%%, ID:%s",
		(double)data_to_publish.temperature,
		(double)data_to_publish.humidity,
		(double)data_to_publish.probe_battery,
		data_to_publish.probe_id);
	
	return 0;
}

int uart_sensor_get_current_data(struct uart_sensor_msg *data)
{
	if (!uart_module_state.initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	if (!data) {
		LOG_ERR("Invalid data pointer");
		return -EINVAL;
	}
	
	/* Copy current data with thread safety */
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	memcpy(data, &current_sensor_data, sizeof(struct uart_sensor_msg));
	k_mutex_unlock(&uart_data_mutex);
	
	return 0;
}

int uart_sensor_get_stats(struct uart_sensor_stats *stats)
{
	if (!uart_module_state.initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	if (!stats) {
		LOG_ERR("Invalid stats pointer");
		return -EINVAL;
	}
	
	/* Copy statistics */
	memcpy(stats, &uart_module_state.stats, sizeof(struct uart_sensor_stats));
	stats->uptime_ms = k_uptime_get() - uart_module_state.stats.uptime_ms;
	
	return 0;
}

int uart_sensor_reset_stats(void)
{
	if (!uart_module_state.initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	/* Reset all statistics */
	memset(&uart_module_state.stats, 0, sizeof(uart_module_state.stats));
	uart_module_state.stats.uptime_ms = k_uptime_get();
	uart_module_state.consecutive_errors = 0;
	
	LOG_INF("UART sensor statistics reset");
	return 0;
}

int uart_sensor_set_auto_publish(bool enable)
{
	if (!uart_module_state.initialized) {
		LOG_ERR("UART sensor module not initialized");
		return -ENODEV;
	}
	
	uart_module_state.auto_publish_enabled = enable;
	LOG_INF("UART sensor auto-publish %s", enable ? "enabled" : "disabled");
	
	return 0;
}

/* Module initialization */
static int uart_sensor_module_init(void)
{
	int err;
	
	LOG_INF("Initializing UART sensor module (v%d)", UART_PROTOCOL_VERSION);
	LOG_INF("Configuration: RX buffer %d bytes, Line buffer %d bytes, Message queue %d",
		UART_RX_BUF_SIZE, UART_LINE_BUF_SIZE, UART_MSG_QUEUE_SIZE);
	
	/* Check if UART device is ready */
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}
	
	/* Initialize UART communication */
	err = uart_sensor_init();
	if (err) {
		LOG_ERR("Failed to initialize UART communication: %d", err);
		return err;
	}
	
	/* Initialize with sensible default values */
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
	current_sensor_data.temperature = 25.0f; /* Room temperature default */
	current_sensor_data.humidity = 50.0f; /* Moderate humidity default */
	current_sensor_data.probe_battery = 85.0f; /* Good battery level default */
	current_sensor_data.error_type = UART_SENSOR_ERROR_NONE;
	strncpy(current_sensor_data.probe_id, UART_DEFAULT_PROBE_ID, sizeof(current_sensor_data.probe_id));
	current_sensor_data.probe_id[sizeof(current_sensor_data.probe_id) - 1] = '\0';
	current_sensor_data.timestamp = k_uptime_get();
	
	/* Initialize data quality indicators */
	current_sensor_data.data_quality.temperature_valid = true;
	current_sensor_data.data_quality.humidity_valid = true;
	current_sensor_data.data_quality.battery_valid = true;
	current_sensor_data.data_quality.probe_id_valid = true;
	
	/* Clear error details */
	memset(current_sensor_data.error_details, 0, sizeof(current_sensor_data.error_details));
	k_mutex_unlock(&uart_data_mutex);
	
	/* Initialize statistics */
	memset(&uart_module_state.stats, 0, sizeof(uart_module_state.stats));
	uart_module_state.stats.uptime_ms = k_uptime_get();
	
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
	
	uart_module_state.initialized = true;
	
	LOG_INF("UART sensor module initialized successfully");
	LOG_INF("Features: Auto-publish %s, Data validation %s, Error recovery %s",
		CONFIG_APP_UART_SENSOR_AUTO_PUBLISH ? "enabled" : "disabled",
		CONFIG_APP_UART_SENSOR_DATA_VALIDATION ? "enabled" : "disabled",
		CONFIG_APP_UART_SENSOR_ERROR_RECOVERY ? "enabled" : "disabled");
	
	return 0;
}

/* Initialize the UART sensor module at system startup */
SYS_INIT(uart_sensor_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
