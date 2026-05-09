/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _UART_SENSOR_H_
#define _UART_SENSOR_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART configuration using Kconfig values */
#define UART_RX_BUF_SIZE             CONFIG_APP_UART_SENSOR_RX_BUFFER_SIZE
#define UART_LINE_BUF_SIZE           CONFIG_APP_UART_SENSOR_LINE_BUFFER_SIZE
#define UART_MSG_QUEUE_SIZE          CONFIG_APP_UART_SENSOR_MSG_QUEUE_SIZE
#define UART_THREAD_STACK_SIZE       CONFIG_APP_UART_SENSOR_THREAD_STACK_SIZE
#define UART_THREAD_PRIORITY         CONFIG_APP_UART_SENSOR_THREAD_PRIORITY
#define UART_ZBUS_TIMEOUT_MS         CONFIG_APP_UART_SENSOR_ZBUS_TIMEOUT_MS
#define UART_PROCESSING_TIMEOUT_MS   CONFIG_APP_UART_SENSOR_PROCESSING_TIMEOUT_MS

/* Protocol configuration */
#define UART_PROTOCOL_VERSION        CONFIG_APP_UART_SENSOR_PROTOCOL_VERSION
#define UART_MESSAGE_DELIMITER       CONFIG_APP_UART_SENSOR_MESSAGE_DELIMITER
#define UART_FIELD_SEPARATOR         CONFIG_APP_UART_SENSOR_FIELD_SEPARATOR
#define UART_VALUE_SEPARATOR         CONFIG_APP_UART_SENSOR_VALUE_SEPARATOR

/* Data validation limits */
#ifdef CONFIG_APP_UART_SENSOR_DATA_VALIDATION
#define UART_TEMP_MIN                CONFIG_APP_UART_SENSOR_TEMP_MIN
#define UART_TEMP_MAX                CONFIG_APP_UART_SENSOR_TEMP_MAX
#define UART_HUMIDITY_MIN            CONFIG_APP_UART_SENSOR_HUMIDITY_MIN
#define UART_HUMIDITY_MAX            CONFIG_APP_UART_SENSOR_HUMIDITY_MAX
#endif

/* Battery voltage configuration */
#define UART_BATTERY_MIN_MV          CONFIG_APP_UART_SENSOR_BATTERY_MIN_MV
#define UART_BATTERY_MAX_MV          CONFIG_APP_UART_SENSOR_BATTERY_MAX_MV

/* Probe ID configuration */
#define UART_PROBE_ID_MIN_LEN        CONFIG_APP_UART_SENSOR_PROBE_ID_MIN_LEN
#define UART_PROBE_ID_MAX_LEN        CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN

/* Error handling */
#ifdef CONFIG_APP_UART_SENSOR_ERROR_RECOVERY
#define UART_MAX_CONSECUTIVE_ERRORS  CONFIG_APP_UART_SENSOR_MAX_CONSECUTIVE_ERRORS
#define UART_RECOVERY_DELAY_MS       CONFIG_APP_UART_SENSOR_RECOVERY_DELAY_MS
#endif

/* ESL polling interval (seconds) */
#define ESL_POLL_INTERVAL_SEC        CONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC

/* Maximum number of ESL tags we track */
#define ESL_MAX_TAGS                 CONFIG_APP_UART_SENSOR_ESL_MAX_TAGS

/* ZBUS channel for UART sensor module communication */
ZBUS_CHAN_DECLARE(UART_SENSOR_CHAN);

enum uart_sensor_msg_type {
	/* Output message types */

	/** Response message containing sensor data from ESL tag (NUS status). */
	UART_SENSOR_DATA_RESPONSE = 0x1,

	/** Error message indicating communication or data validation failure. */
	UART_SENSOR_ERROR_RESPONSE = 0x2,

	/** Statistics message containing performance and error counters. */
	UART_SENSOR_STATS_RESPONSE = 0x3,

	/** Generic response message containing text data from UART (shell output). */
	UART_SENSOR_GENERIC_RESPONSE = 0x4,

	/** ESL tag discovered during BLE scan. */
	UART_SENSOR_ESL_TAG_FOUND = 0x5,

	/** ESL tag connected. */
	UART_SENSOR_ESL_TAG_CONNECTED = 0x6,

	/** ESL tag configured and synchronized. */
	UART_SENSOR_ESL_TAG_CONFIGURED = 0x7,

	/** ESL NUS response (status/sensors from tag). */
	UART_SENSOR_ESL_NUS_RESPONSE = 0x8,

	/** nRF5340 boot detected. */
	UART_SENSOR_ESL_AP_READY = 0x9,

	/** ESL tag disconnected from BLE. */
	UART_SENSOR_ESL_TAG_DISCONNECTED = 0xA,

	/** Sensor name received via #SENSOR_NAME event from nRF5340. */
	UART_SENSOR_ESL_NAME_RESPONSE = 0xB,

	/** Sensor whitelist management event from nRF5340 (#SENSOR_MGMT:*). */
	UART_SENSOR_SENSOR_MGMT_EVENT = 0xC,

	/** Sensor blocked by whitelist (#SENSOR_BLOCKED:*). */
	UART_SENSOR_SENSOR_BLOCKED = 0xD,

	/* Input message types */

	/** Request to sample sensor data from all ESL tags. */
	UART_SENSOR_DATA_REQUEST = 0x10,

	/** Request to reset error counters and statistics. */
	UART_SENSOR_RESET_STATS_REQUEST = 0x11,

	/** Request to get current module status and configuration. */
	UART_SENSOR_STATUS_REQUEST = 0x12,
};

enum uart_sensor_error_type {
	UART_SENSOR_ERROR_NONE = 0,
	UART_SENSOR_ERROR_PARSE_FAILED,
	UART_SENSOR_ERROR_INVALID_DATA,
	UART_SENSOR_ERROR_COMM_TIMEOUT,
	UART_SENSOR_ERROR_DEVICE_NOT_READY,
	UART_SENSOR_ERROR_BUFFER_OVERFLOW,
	UART_SENSOR_ERROR_VALIDATION_FAILED,
	UART_SENSOR_ERROR_REMOTE_ERROR,
};

/** ESL tag info from NUS status response */
struct esl_tag_info {
	uint16_t esl_addr;
	char     mac[18];            /* "XX:XX:XX:XX:XX:XX" */
	uint32_t uptime_s;
	uint16_t battery_mv;
	uint8_t  flags;
	float    temperature;        /* °C, from NUS sensors or ECP sensor */
	int8_t   rssi;
	int64_t  last_seen;          /* uptime when last heard from */
	bool     valid;
	bool     connected;          /* true while BLE link is active */
	char     name[13];           /* human-readable sensor name, from #SENSOR_NAME event */
};

struct uart_sensor_msg {
	enum uart_sensor_msg_type type;

	/** Temperature reading from ESL tag in degrees Celsius. */
	float temperature;

	/** Humidity reading (not always available from ESL tags). */
	float humidity;

	/** Probe/tag identifier string. */
	char probe_id[CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN + 1];

	/** Battery level of the tag in percentage (0-100). */
	float probe_battery;

	/** Timestamp of the sample in milliseconds since boot. */
	int64_t timestamp;

	/** Error information (if type is UART_SENSOR_ERROR_RESPONSE). */
	enum uart_sensor_error_type error_type;

	/** Additional error details or status information. */
	char error_details[64];

	/** Generic response text (raw shell output from nRF5340). */
	char response_text[128];

	/** ESL tag detailed info (for ESL_NUS_RESPONSE type). */
	struct esl_tag_info tag_info;

	/** Data quality indicators. */
	struct {
		bool temperature_valid;
		bool humidity_valid;
		bool battery_valid;
		bool probe_id_valid;
	} data_quality;
};

struct uart_sensor_stats {
	uint32_t messages_received;
	uint32_t messages_parsed;
	uint32_t parse_errors;
	uint32_t validation_errors;
	uint32_t publish_errors;
	uint32_t comm_errors;
	int64_t last_error_time;
	int64_t uptime_ms;
};

#define MSG_TO_UART_SENSOR_MSG(_msg)	(*(const struct uart_sensor_msg *)_msg)

/* ---- Public API ---- */

int uart_sensor_init(void);
int uart_sensor_sample_request(void);
int uart_sensor_send_command(const char *cmd);
int uart_sensor_get_current_data(struct uart_sensor_msg *data);
int uart_sensor_check_status(void);
int uart_sensor_get_stats(struct uart_sensor_stats *stats);
int uart_sensor_reset_stats(void);
int uart_sensor_process_data_line(const char *data);
int uart_sensor_set_auto_publish(bool enable);
bool uart_sensor_validate_data(const struct uart_sensor_msg *msg);

/** @brief Send an ESL AP shell command (prepends "esl_c " if not present). */
int uart_sensor_esl_command(const char *esl_cmd);

/** @brief Trigger a scan for ESL tags (sends: esl_c acl scan 1 1). */
int uart_sensor_esl_scan_start(void);

/** @brief Stop ESL scan. */
int uart_sensor_esl_scan_stop(void);

/** @brief Request NUS status from an ESL tag (sends: esl_c nus status <id>). */
int uart_sensor_esl_nus_status(uint16_t esl_id);

/** @brief Request NUS sensors from an ESL tag (sends: esl_c nus sensors <id>). */
int uart_sensor_esl_nus_sensors(uint16_t esl_id);

/**
 * @brief Request the human-readable name from an ESL tag via NUS GET_NAME (0x06).
 *        Pass esl_id=0xFFFF to request from all currently tracked tags.
 *        The response arrives asynchronously as a #SENSOR_NAME SPI event and is
 *        published to MQTT via UART_SENSOR_ESL_NAME_RESPONSE.
 */
int uart_sensor_esl_get_name(uint16_t esl_id);

/**
 * @brief Force-push the current LTE UTC epoch to the nRF5340 AP via SPI
 *        (AP_SET_TIME:<epoch>).  Call once at boot when LTE time is available,
 *        or on demand.  The boot-guard flag is reset so the send always happens.
 */
int uart_sensor_esl_set_ap_time(void);

/** @brief Start/stop automatic ESL polling (every ESL_POLL_INTERVAL_SEC). */
int uart_sensor_esl_poll_start(void);
int uart_sensor_esl_poll_stop(void);

/** @brief Get the number of known tags. */
int uart_sensor_esl_get_tag_count(void);

/** @brief Get info for a specific tag by ESL address. */
int uart_sensor_esl_get_tag_info(uint16_t esl_addr, struct esl_tag_info *info);

/** @brief Get info for a specific tag by zero-based array index (0..count-1). */
int uart_sensor_esl_get_tag_info_at(int index, struct esl_tag_info *info);

/** @brief Set the expected number of ESL tags to discover.
 *  Scanning will repeat until this many tags are found.
 *  Default comes from CONFIG_APP_UART_SENSOR_ESL_EXPECTED_TAGS.
 */
int uart_sensor_esl_set_expected_tags(int n);

/** @brief Return the currently configured expected tag count. */
int uart_sensor_esl_get_expected_tags(void);

/** @brief Runtime sensor configuration — all persist to flash via MQTT commands. */
int uart_sensor_esl_set_poll_interval(int s);
int uart_sensor_esl_get_poll_interval(void);
int uart_sensor_esl_set_scan_retry_interval(int s);
int uart_sensor_esl_get_scan_retry_interval(void);
int uart_sensor_esl_set_nus_failures(int n);
int uart_sensor_esl_get_nus_failures(void);

/**
 * @brief Enable/disable debug echo mode.
 *
 * When enabled, every unrecognized UART RX line is published to MQTT
 * as {"type":"uart_debug","line":"..."}. Disabled by default (production-safe).
 */
int uart_sensor_set_debug_echo(bool enable);

/* ---- Sensor whitelist management (forwarded to nRF5340 gateway) ---- */

/** @brief Send a sensor_mgmt command to the nRF5340 gateway.
 *  The cmd string is sent as-is (e.g. "sensor_mgmt add AA:BB:CC:DD:EE:FF").
 */
int uart_sensor_mgmt_command(const char *cmd);

/** @brief Query gateway sensor whitelist status. */
int uart_sensor_mgmt_status(void);

/** @brief Set whitelist mode ("whitelist" or "open"). */
int uart_sensor_mgmt_set_mode(const char *mode);

/** @brief Add a sensor BLE address to the whitelist. */
int uart_sensor_mgmt_add(const char *ble_addr);

/** @brief Remove a sensor BLE address from the whitelist. */
int uart_sensor_mgmt_remove(const char *ble_addr);

/** @brief List all sensors in the whitelist. */
int uart_sensor_mgmt_list(void);

/** @brief Clear all sensors from the whitelist. */
int uart_sensor_mgmt_clear(void);

/** @brief Mark provisioning complete on the gateway. */
int uart_sensor_mgmt_provision_done(void);

/** @brief Import a BLE bond key for a sensor. */
int uart_sensor_mgmt_set_key(const char *ble_addr, const char *hex_key);

/** @brief Export a BLE bond key for a sensor. */
int uart_sensor_mgmt_get_key(const char *ble_addr);

/* ---- Low-level SPI bus access (for SPI DFU transport) ---- */

/**
 * @brief Lock the SPI bus mutex.
 *
 * The caller must call uart_sensor_spi_unlock() when done.  While locked,
 * the DRDY work handler and all other SPI users will block.
 *
 * @param timeout  How long to wait for the mutex.
 * @return 0 on success, negative errno on timeout.
 */
int uart_sensor_spi_lock(k_timeout_t timeout);

/** @brief Unlock the SPI bus mutex. */
void uart_sensor_spi_unlock(void);

/**
 * @brief Perform a raw 520-byte full-duplex SPI transaction.
 *
 * The caller MUST hold the SPI bus lock (via uart_sensor_spi_lock()).
 * The function asserts CS, transceives, and deasserts CS.
 *
 * @param tx  Pointer to 520-byte TX frame (pre-built by caller).
 * @param rx  Pointer to 520-byte RX buffer.
 * @return 0 on success, negative errno on SPI error.
 */
int uart_sensor_spi_xfer_locked(const uint8_t *tx, uint8_t *rx);

/**
 * @brief Check if DATA_READY GPIO from nRF5340 is currently asserted.
 * @return true if DATA_READY is HIGH, false otherwise.
 */
bool uart_sensor_spi_drdy_active(void);

/**
 * @brief Send a FRAME_SMP_FWD (0x07) response back to the nRF5340.
 *
 * Called by the spi_fwd module after MCUmgr processes an SMP request
 * received from the PC via the nRF5340 USB bridge.  Locks the SPI bus,
 * builds the response frame, transceives, and processes any simultaneous
 * RX frame from the nRF5340.
 *
 * @param payload  Raw SMP response payload (8-byte header + CBOR body).
 * @param len      Payload length in bytes (max 512).
 * @return 0 on success, negative errno on error.
 */
int uart_sensor_spi_send_smp_fwd(const uint8_t *payload, uint16_t len);

/**
 * @brief Submit a work item to the SPI workqueue.
 *
 * Allows external modules (e.g. spi_fwd) to run work on the dedicated
 * SPI workqueue instead of the system workqueue.
 *
 * @param work  Pointer to the k_work item to submit.
 */
void uart_sensor_spi_submit_work(struct k_work *work);

#ifdef __cplusplus
}
#endif

#endif /* _UART_SENSOR_H_ */
