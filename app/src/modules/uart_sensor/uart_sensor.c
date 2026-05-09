/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * ESL AP Sensor Module — SPI Inter-Chip Bridge
 *
 * Communicates with the nRF5340 ESL Access Point over SPI (master side).
 * The nRF5340 runs the Zephyr shell with "esl_c" commands; this module
 * sends framed commands over SPI and parses #-prefixed URC NOTIFY frames.
 *
 * Key operations:
 * - On boot: wait for ESL AP "ready" NOTIFY, start scan, start PAwR
 * - Every ESL_POLL_INTERVAL_SEC: poll NUS status for each known tag
 * - Publish tag data to ZBUS → custom_mqtt → cloud
 * - Forward arbitrary shell commands from MQTT passthrough
 *
 * SPI frame format (520 bytes fixed):
 *   [0]      MAGIC   0xA5
 *   [1]      TYPE    (CMD=0x01, CMD_RESP=0x02, NOTIFY=0x03,
 *                     READ_REQ=0x04, NOOP=0x05, INVALID=0xFF)
 *   [2-3]    LEN     payload length, little-endian uint16
 *   [4-515]  PAYLOAD zero-padded to 512 bytes
 *   [516]    CRC8    CRC-8/ATM (poly=0x07, init=0x00) over bytes [0..515]
 *   [517-519] RSVD    zero padding
 *
 * DATA_READY signal: nRF5340 P1.08 → nRF9151 P0.04 (rising edge = data queued)
 * SPI CS:            nRF9151 P0.05 → nRF5340 P1.06 (active LOW, manual GPIO)
 * SPI bus:           nRF9151 SPI3 (P0.13/14/15 = SCK/MOSI/MISO)
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "uart_sensor.h"
#include "custom_mqtt.h"
#if defined(CONFIG_APP_SPI_FWD)
#include "spi_fwd.h"
#endif
#include <date_time.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(uart_sensor, CONFIG_APP_UART_SENSOR_LOG_LEVEL);

/* ---- SPI Frame Protocol ---- */
#define SPI_FRAME_SIZE      520
#define SPI_PAYLOAD_MAX     512
#define SPI_MAGIC           0xA5

#define SPI_OFF_MAGIC       0
#define SPI_OFF_TYPE        1
#define SPI_OFF_LEN_LO      2
#define SPI_OFF_LEN_HI      3
#define SPI_OFF_PAYLOAD     4
#define SPI_OFF_CRC         (SPI_OFF_PAYLOAD + SPI_PAYLOAD_MAX)  /* byte 516 */

#define SPI_TYPE_CMD        0x01   /* nRF91 → nRF53: shell command */
#define SPI_TYPE_CMD_RESP   0x02   /* nRF53 → nRF91: command response */
#define SPI_TYPE_NOTIFY     0x03   /* nRF53 → nRF91: URC notification */
#define SPI_TYPE_READ_REQ   0x04   /* nRF91 → nRF53: poll for pending data */
#define SPI_TYPE_NOOP       0x05   /* either: keepalive / empty */
#define SPI_TYPE_SMP        0x06   /* nRF91 → nRF53: MCUmgr SMP client (spi_dfu) */
#define SPI_TYPE_SMP_FWD    0x07   /* bidirectional: MCUmgr SMP forward via USB bridge */
#define SPI_TYPE_INVALID    0xFF   /* framing error */

/* ---- Hardware ---- */
static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));

/* CS GPIO: P0.05, active LOW (assert = drive LOW = enable nRF5340 SPIS) */
static const struct gpio_dt_spec esl_cs_gpio =
        GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), esl_ap_cs_gpios);

/* DATA_READY GPIO: P0.04, active HIGH (nRF5340 signals pending data) */
static const struct gpio_dt_spec esl_drdy_gpio =
        GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), esl_ap_drdy_gpios);

/* SPI master configuration — manual CS, 4 MHz, MSB first, 8-bit words */
static const struct spi_config spi_cfg = {
        .frequency = MHZ(4),
        .operation = SPI_WORD_SET(8) | SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB,
        .slave     = 0,
        /* cs.gpio.port = NULL → driver does NOT touch any CS pin; we do it manually */
};

/* SPI frame buffers (allocated once, reused per transaction) */
static uint8_t spi_tx_frame[SPI_FRAME_SIZE];
static uint8_t spi_rx_frame[SPI_FRAME_SIZE];

/* Mutex protecting the SPI bus (prevents concurrent transactions) */
static K_MUTEX_DEFINE(spi_bus_mutex);

/* ---- Buffers & Synchronization ---- */
K_MSGQ_DEFINE(uart_msgq, UART_RX_BUF_SIZE, UART_MSG_QUEUE_SIZE, 4);
K_SEM_DEFINE(uart_wake_sem, 0, 1);
K_MUTEX_DEFINE(uart_data_mutex);

/* Background processing thread */
static K_THREAD_STACK_DEFINE(uart_thread_stack, UART_THREAD_STACK_SIZE);
static struct k_thread uart_thread_data;
static k_tid_t uart_thread_tid;

/* ---- ESL Tag Database (in-RAM) ---- */
static struct esl_tag_info tag_db[ESL_MAX_TAGS];
static int tag_count;

/* Last ESL address seen in a #CONFIGURED:, #TAG: or #TAG_SCANNED: event.
 * Used to associate subsequent #SENSOR_REPORT:* and #NUS_STATUS:/#NUS_SENSORS:
 * messages with the correct tag (nRF5340 does not include the address in those
 * URCs). Starts at 0x0000 (unknown) and is updated upon every tag event. */
static uint16_t last_polled_esl_addr;

/* Address of the most recent unicast `nus get_name 0x<addr>` we issued.
 * The gateway sometimes emits #SENSOR_NAME:0x0000,<name> when the response
 * arrives via the broadcast subevent slot (resp_addr is reconstructed from
 * group_id + last_nus_target_low[group_id] and resolves to 0 for group 0
 * before the per-group target table has been written by the gateway).
 * In that case we attribute the reply to the pending request. */
static uint16_t pending_get_name_addr;

/* ---- Dedicated work queue for SPI transactions (sysworkq stack is too small) ---- */
#define SPI_WQ_STACK_SIZE 4096
#define SPI_WQ_PRIORITY   5
K_THREAD_STACK_DEFINE(spi_wq_stack, SPI_WQ_STACK_SIZE);
static struct k_work_q spi_work_q;

/* ---- ESL polling work ---- */
static struct k_work_delayable esl_poll_work;
static bool esl_poll_active;

/* ---- NUS response timeout tracking ---- */
static int64_t last_nus_response_time;
static int64_t last_poll_send_time;
static int nus_consecutive_poll_failures;
#define NUS_MAX_POLL_FAILURES 3  /* Rescan after this many missed polls */

/* ---- Debug echo mode ---- */
static bool uart_debug_echo_enabled; /* Publish all unrecognized UART RX to MQTT when true */

/* ---- Time sync state ---- */
static bool ap_time_sent_this_boot;   /* true once we've successfully pushed time to AP */
static bool lte_time_available;       /* true once date_time is synced */

int uart_sensor_set_debug_echo(bool enable)
{
	uart_debug_echo_enabled = enable;
	LOG_INF("UART debug echo: %s", enable ? "ENABLED" : "disabled");
	return 0;
}

/* ---- Sensor whitelist management (forwarded to nRF5340 gateway) ---- */

int uart_sensor_mgmt_command(const char *cmd)
{
	if (!cmd) {
		return -EINVAL;
	}
	/* If caller already prefixed "sensor_mgmt", send as-is */
	if (strncmp(cmd, "sensor_mgmt ", 12) == 0) {
		return uart_sensor_send_command(cmd);
	}
	char buf[UART_LINE_BUF_SIZE];
	int n = snprintf(buf, sizeof(buf), "sensor_mgmt %s", cmd);

	if (n < 0 || n >= (int)sizeof(buf)) {
		return -ENOMEM;
	}
	return uart_sensor_send_command(buf);
}

int uart_sensor_mgmt_status(void)
{
	return uart_sensor_send_command("sensor_mgmt status");
}

int uart_sensor_mgmt_set_mode(const char *mode)
{
	if (!mode) {
		return -EINVAL;
	}
	char buf[64];

	snprintf(buf, sizeof(buf), "sensor_mgmt mode %s", mode);
	return uart_sensor_send_command(buf);
}

int uart_sensor_mgmt_add(const char *ble_addr)
{
	if (!ble_addr) {
		return -EINVAL;
	}
	char buf[64];

	snprintf(buf, sizeof(buf), "sensor_mgmt add %s", ble_addr);
	return uart_sensor_send_command(buf);
}

int uart_sensor_mgmt_remove(const char *ble_addr)
{
	if (!ble_addr) {
		return -EINVAL;
	}
	char buf[64];

	snprintf(buf, sizeof(buf), "sensor_mgmt remove %s", ble_addr);
	return uart_sensor_send_command(buf);
}

int uart_sensor_mgmt_list(void)
{
	return uart_sensor_send_command("sensor_mgmt list");
}

int uart_sensor_mgmt_clear(void)
{
	return uart_sensor_send_command("sensor_mgmt clear");
}

int uart_sensor_mgmt_provision_done(void)
{
	return uart_sensor_send_command("sensor_mgmt provision_done");
}

int uart_sensor_mgmt_set_key(const char *ble_addr, const char *hex_key)
{
	if (!ble_addr || !hex_key) {
		return -EINVAL;
	}
	char buf[UART_LINE_BUF_SIZE];
	int n = snprintf(buf, sizeof(buf), "sensor_mgmt set_key %s %s",
			 ble_addr, hex_key);

	if (n < 0 || n >= (int)sizeof(buf)) {
		return -ENOMEM;
	}
	return uart_sensor_send_command(buf);
}

int uart_sensor_mgmt_get_key(const char *ble_addr)
{
	if (!ble_addr) {
		return -EINVAL;
	}
	char buf[64];

	snprintf(buf, sizeof(buf), "sensor_mgmt get_key %s", ble_addr);
	return uart_sensor_send_command(buf);
}

/* Startup sequencing work */
static struct k_work_delayable esl_startup_work;
static int esl_startup_step;
static bool esl_ap_ready;  /* true once we see the shell prompt */

/* Gateway provisioning state, learned from "#SENSOR_MGMT:STATUS,...,PROVISIONED=<0|1>".
 * When true, the whitelist is sealed and the gateway already knows the fleet:
 * we MUST NOT scan/onboard on boot \u2014 doing so steals PA-train air-time and
 * can cause #PAWR_GAP events (see NRF91_USAGE_GUIDE.md \u00a72/\u00a77).
 * Tri-state: -1 = unknown, 0 = not provisioned, 1 = provisioned.
 */
static int gateway_provisioned = -1;

/* Monotonic uptime of the last on-demand scan request \u2014 used to enforce the
 * \u22655 min back-off recommended by NRF91_USAGE_GUIDE.md \u00a78. */
static int64_t last_scan_request_ms;
#define ESL_SCAN_MIN_INTERVAL_MS  (5 * 60 * 1000)

/* ---- Time sync helpers ---- */

/**
 * Send current LTE UTC epoch to the nRF5340 AP via SPI.
 * Called proactively (on AP ready + LTE time available) and on #AP_GET_TIME.
 * Always attempts the send; does not guard on ap_time_sent_this_boot so that
 * demand requests (#AP_GET_TIME) are always honoured.
 */
static void send_time_to_ap(void)
{
	int64_t unix_time_ms = 0;
	int rc = date_time_now(&unix_time_ms);

	if (rc != 0) {
		LOG_WRN("AP time sync: LTE time not available yet (err %d)", rc);
		return;
	}

	int64_t epoch_s = unix_time_ms / 1000;

	/* Sanity: reject obviously wrong epochs */
	if (epoch_s < 1700000000LL || epoch_s > 4000000000LL) {
		LOG_WRN("AP time sync: epoch %lld out of valid range — not sending", epoch_s);
		return;
	}

	char cmd[48];

	snprintf(cmd, sizeof(cmd), "AP_SET_TIME:%lld", epoch_s);
	LOG_INF("AP time sync: sending '%s'", cmd);
	uart_sensor_send_command(cmd);
	ap_time_sent_this_boot = true;
}

/**
 * Proactive time send: only fires once per boot if both AP is ready AND LTE
 * time is available.  Used at AP startup and from the date_time callback.
 */
static void try_send_time_to_ap(void)
{
	if (!esl_ap_ready || !lte_time_available || ap_time_sent_this_boot) {
		return;
	}
	send_time_to_ap();
}

/** date_time event callback — called when LTE clock syncs. */
static void date_time_event_handler(const struct date_time_evt *evt)
{
	if (evt->type == DATE_TIME_OBTAINED_MODEM ||
	    evt->type == DATE_TIME_OBTAINED_NTP ||
	    evt->type == DATE_TIME_OBTAINED_EXT) {
		LOG_INF("AP time sync: LTE time synced (source %d)", evt->type);
		lte_time_available = true;
		try_send_time_to_ap();
	}
}

/* ---- Smart scan discovery ---- */
/* Number of tags we expect to find; scan retries until this many are connected. */
static int expected_tag_count = CONFIG_APP_UART_SENSOR_ESL_EXPECTED_TAGS;
/* True while nRF5340 scan window is open (between #SCAN:Start and #SCAN:Stop). */
static bool scan_active;
/* Retry work: scheduled when connected count drops below expected. */
static struct k_work_delayable scan_retry_work;

/* ---- Runtime-tunable parameters (changeable via MQTT; persist to flash) ---- */
static int esl_poll_interval_sec    = CONFIG_APP_UART_SENSOR_ESL_POLL_INTERVAL_SEC;
static int scan_retry_interval_sec  = CONFIG_APP_UART_SENSOR_ESL_SCAN_RETRY_INTERVAL_SEC;
static int nus_max_poll_failures_rt = NUS_MAX_POLL_FAILURES;

/* NUS settle work: fires 200 ms after the last NUS sub-response to publish exactly once. */
static struct k_work_delayable nus_settle_work;

/* ---- Module state ---- */
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
};

/* ZBUS channel */
ZBUS_CHAN_DEFINE(UART_SENSOR_CHAN,
		 struct uart_sensor_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/* Current message (published via ZBUS) */
static struct uart_sensor_msg current_sensor_data;

/* Forward declarations */
static int uart_device_setup(void);
static void uart_error_handler(enum uart_sensor_error_type error_type, const char *details);
static void publish_error_message(enum uart_sensor_error_type error_type, const char *details);
static void update_statistics(bool parse_success, bool validation_success, bool publish_success);
static void esl_poll_work_fn(struct k_work *work);
static void esl_startup_work_fn(struct k_work *work);
static int esl_tag_db_find_or_add(uint16_t esl_addr);
static float esl_battery_pct_from_mv(uint32_t batt_mv);
static void esl_store_sensor_snapshot(uint16_t esl_addr, int tag_index, bool publish);
static int esl_count_connected(void);
static void esl_discovery_check(void);
static void scan_retry_work_fn(struct k_work *work);
static void nus_settle_work_fn(struct k_work *work);

/* ===========================================================================
 * SPI Frame Helpers
 * =========================================================================== */

/**
 * Build a 520-byte SPI frame into @buf.
 * @type:    one of SPI_TYPE_*
 * @payload: pointer to payload string (may be NULL for zero-length frames)
 * @len:     payload length in bytes (0..SPI_PAYLOAD_MAX)
 */
static void build_frame(uint8_t *buf, uint8_t type,
			const char *payload, size_t len)
{
	memset(buf, 0, SPI_FRAME_SIZE);
	buf[SPI_OFF_MAGIC]  = SPI_MAGIC;
	buf[SPI_OFF_TYPE]   = type;
	buf[SPI_OFF_LEN_LO] = (uint8_t)(len & 0xFF);
	buf[SPI_OFF_LEN_HI] = (uint8_t)((len >> 8) & 0xFF);
	if (payload && len > 0) {
		memcpy(&buf[SPI_OFF_PAYLOAD], payload,
		       MIN(len, (size_t)SPI_PAYLOAD_MAX));
	}
	buf[SPI_OFF_CRC] = crc8(buf, SPI_OFF_CRC, 0x07, 0x00, false);
}

/**
 * Validate received frame: check MAGIC byte and CRC.
 */
static bool frame_valid(const uint8_t *buf)
{
	if (buf[SPI_OFF_MAGIC] != SPI_MAGIC) {
		return false;
	}
	return buf[SPI_OFF_CRC] == crc8(buf, SPI_OFF_CRC, 0x07, 0x00, false);
}

/* ===========================================================================
 * SPI RX processing — feeds parsed line into UART msgq / sem
 * (same downstream path as the former UART ISR)
 * =========================================================================== */

/**
 * Decode a received SPI frame and push text payload (NOTIFY or CMD_RESP)
 * to the processing thread via the existing message queue.
 */
static void process_rx_frame(const uint8_t *buf)
{
	if (!frame_valid(buf)) {
		LOG_DBG("SPI RX: invalid frame (magic=0x%02X crc=got:0x%02X exp:0x%02X)",
			buf[SPI_OFF_MAGIC], buf[SPI_OFF_CRC],
			crc8(buf, SPI_OFF_CRC, 0x07, 0x00, false));
		return;
	}

	uint8_t  type = buf[SPI_OFF_TYPE];
	uint16_t len  = (uint16_t)buf[SPI_OFF_LEN_LO] |
			((uint16_t)buf[SPI_OFF_LEN_HI] << 8);

	if (type == SPI_TYPE_NOOP || type == SPI_TYPE_INVALID) {
		return;
	}

	if ((type == SPI_TYPE_NOTIFY || type == SPI_TYPE_CMD_RESP) &&
	    len > 0 && len <= SPI_PAYLOAD_MAX) {
		/* Copy payload into a scratch buffer — the ring may pack multiple
		 * URC lines into one frame (up to 512 bytes).  Split on CR/LF and
		 * enqueue every non-empty line as a separate msgq item so the
		 * parser always sees exactly one URC per call. */
		static char payload_buf[SPI_PAYLOAD_MAX + 1];

		memcpy(payload_buf, &buf[SPI_OFF_PAYLOAD], len);
		payload_buf[len] = '\0';

		char *p = payload_buf;
		bool enqueued_any = false;

		while (p < payload_buf + len && *p != '\0') {
			/* Length of this segment (up to next CR or LF) */
			size_t seg = strcspn(p, "\r\n");

			if (seg > 0) {
				/* Item must fit in the msgq slot (UART_RX_BUF_SIZE) */
				size_t copy = MIN(seg, (size_t)(UART_RX_BUF_SIZE - 1));
				char item[UART_RX_BUF_SIZE];

				memcpy(item, p, copy);
				item[copy] = '\0';

				LOG_INF("<<< SPI RX [0x%02X]: %s", type, item);
				if (k_msgq_put(&uart_msgq, item, K_NO_WAIT) == 0) {
					enqueued_any = true;
				} else {
					LOG_WRN("SPI RX: msgq full, drop: %.40s", item);
				}
			}

			p += seg;
			/* Skip all CR/LF separators between lines */
			while (*p == '\r' || *p == '\n') {
				p++;
			}
		}

		if (enqueued_any) {
			k_sem_give(&uart_wake_sem);
		}
#if defined(CONFIG_APP_SPI_FWD)
	} else if (type == SPI_TYPE_SMP_FWD) {
		/* MCUmgr SMP request forwarded by nRF5340 from a PC via USB.
		 * Feed the raw SMP payload to the spi_fwd module which runs
		 * the nRF9151's MCUmgr server and sends the response back. */
		if (len > 0 && len <= SPI_PAYLOAD_MAX) {
			LOG_INF("SPI RX: FRAME_SMP_FWD len=%u → MCUmgr", len);
			spi_fwd_rx(&buf[SPI_OFF_PAYLOAD], len);
		} else {
			LOG_WRN("SPI RX: FRAME_SMP_FWD bad len=%u, dropped", len);
		}
#endif /* CONFIG_APP_SPI_FWD */
	} else if (type != SPI_TYPE_NOOP) {
		LOG_DBG("SPI RX: type=0x%02X len=%u (no text)", type, len);
	}
}

/* ===========================================================================
 * DATA_READY GPIO interrupt + read work
 * =========================================================================== */

static struct k_work        spi_rx_work;
static struct gpio_callback drdy_gpio_cb;

/**
 * Work handler: drain all queued NOTIFY frames from nRF5340 by sending
 * READ_REQ transactions until DATA_READY goes LOW.
 */
static void spi_rx_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	while (gpio_pin_get_dt(&esl_drdy_gpio) > 0) {
		k_mutex_lock(&spi_bus_mutex, K_FOREVER);

		build_frame(spi_tx_frame, SPI_TYPE_READ_REQ, NULL, 0);

		const struct spi_buf     tx_buf = { .buf = spi_tx_frame,
						    .len = SPI_FRAME_SIZE };
		const struct spi_buf     rx_buf = { .buf = spi_rx_frame,
						    .len = SPI_FRAME_SIZE };
		const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
		const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

		gpio_pin_set_dt(&esl_cs_gpio, 1);  /* Assert CS (active LOW) */
		int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
		gpio_pin_set_dt(&esl_cs_gpio, 0);  /* Deassert CS */

		k_mutex_unlock(&spi_bus_mutex);

		if (ret != 0) {
			LOG_ERR("SPI READ_REQ error: %d", ret);
			break;
		}
		process_rx_frame(spi_rx_frame);
	}
}

/**
 * GPIO ISR for DATA_READY rising edge.
 * Only submits work — never calls spi_transceive from interrupt context.
 */
static void drdy_isr(const struct device *port, struct gpio_callback *cb,
		     gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit_to_queue(&spi_work_q, &spi_rx_work);
}

/* ===========================================================================
 * TX — build CMD frame and transceive over SPI
 * =========================================================================== */

int uart_sensor_send_command(const char *cmd)
{
	if (!uart_module_state.initialized || !cmd) {
		return -EINVAL;
	}

	size_t len = strlen(cmd);

	if (len == 0 || len >= (size_t)(SPI_PAYLOAD_MAX - 1)) {
		return -EINVAL;
	}

	/* Payload: command string + newline terminator (static: SPI serialised by mutex) */
	static char payload[SPI_PAYLOAD_MAX];
	int n = snprintf(payload, sizeof(payload), "%s\n", cmd);

	if (n < 0 || n >= (int)sizeof(payload)) {
		return -ENOMEM;
	}

	LOG_INF(">>> SPI TX (CMD): %s", cmd);

	k_mutex_lock(&spi_bus_mutex, K_FOREVER);

	build_frame(spi_tx_frame, SPI_TYPE_CMD, payload, (size_t)n);

	const struct spi_buf     tx_buf = { .buf = spi_tx_frame,
					    .len = SPI_FRAME_SIZE };
	const struct spi_buf     rx_buf = { .buf = spi_rx_frame,
					    .len = SPI_FRAME_SIZE };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	gpio_pin_set_dt(&esl_cs_gpio, 1);  /* Assert CS */
	int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
	gpio_pin_set_dt(&esl_cs_gpio, 0);  /* Deassert CS */

	k_mutex_unlock(&spi_bus_mutex);

	if (ret != 0) {
		LOG_ERR("SPI CMD transceive error: %d", ret);
		return ret;
	}

	/* Full-duplex: simultaneous RX may carry a queued NOTIFY or NOOP */
	process_rx_frame(spi_rx_frame);

	/* Check if more data is pending — DATA_READY still asserted */
	if (gpio_pin_get_dt(&esl_drdy_gpio) > 0) {
		k_work_submit_to_queue(&spi_work_q, &spi_rx_work);
	}

	return 0;
}

/* ===========================================================================
 * Low-level SPI bus access — used by spi_dfu module for DFU transport
 * =========================================================================== */

int uart_sensor_spi_lock(k_timeout_t timeout)
{
	return k_mutex_lock(&spi_bus_mutex, timeout);
}

void uart_sensor_spi_unlock(void)
{
	k_mutex_unlock(&spi_bus_mutex);
}

int uart_sensor_spi_xfer_locked(const uint8_t *tx, uint8_t *rx)
{
	if (!tx || !rx) {
		return -EINVAL;
	}

	const struct spi_buf     tx_buf = { .buf = (void *)tx,
					    .len = SPI_FRAME_SIZE };
	const struct spi_buf     rx_buf = { .buf = rx,
					    .len = SPI_FRAME_SIZE };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	gpio_pin_set_dt(&esl_cs_gpio, 1);  /* Assert CS */
	int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
	gpio_pin_set_dt(&esl_cs_gpio, 0);  /* Deassert CS */

	return ret;
}

bool uart_sensor_spi_drdy_active(void)
{
	return gpio_pin_get_dt(&esl_drdy_gpio) > 0;
}

int uart_sensor_spi_send_smp_fwd(const uint8_t *payload, uint16_t len)
{
	if (!payload || len == 0 || len > SPI_PAYLOAD_MAX) {
		return -EINVAL;
	}

	LOG_INF(">>> SPI TX (SMP_FWD): %u bytes", len);

	k_mutex_lock(&spi_bus_mutex, K_FOREVER);

	build_frame(spi_tx_frame, SPI_TYPE_SMP_FWD, (const char *)payload, len);

	const struct spi_buf     tx_buf = { .buf = spi_tx_frame,
					    .len = SPI_FRAME_SIZE };
	const struct spi_buf     rx_buf = { .buf = spi_rx_frame,
					    .len = SPI_FRAME_SIZE };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	gpio_pin_set_dt(&esl_cs_gpio, 1);
	int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
	gpio_pin_set_dt(&esl_cs_gpio, 0);

	k_mutex_unlock(&spi_bus_mutex);

	if (ret != 0) {
		LOG_ERR("SPI SMP_FWD transceive error: %d", ret);
		return ret;
	}

	/* Full-duplex: simultaneous RX may carry a NOTIFY or NOOP */
	process_rx_frame(spi_rx_frame);

	if (gpio_pin_get_dt(&esl_drdy_gpio) > 0) {
		k_work_submit_to_queue(&spi_work_q, &spi_rx_work);
	}

	return 0;
}

void uart_sensor_spi_submit_work(struct k_work *work)
{
	k_work_submit_to_queue(&spi_work_q, work);
}

/* Convenience: send "esl_c <subcmd>" */
int uart_sensor_esl_command(const char *esl_cmd)
{
	char buf[UART_LINE_BUF_SIZE];
	int n;

	if (!esl_cmd) {
		return -EINVAL;
	}

	/* If the caller already prefixed "esl_c", send as-is */
	if (strncmp(esl_cmd, "esl_c ", 6) == 0) {
		return uart_sensor_send_command(esl_cmd);
	}

	n = snprintf(buf, sizeof(buf), "esl_c %s", esl_cmd);
	if (n < 0 || n >= (int)sizeof(buf)) {
		return -ENOMEM;
	}
	return uart_sensor_send_command(buf);
}

/* ===========================================================================
 * ESL-specific command helpers
 * =========================================================================== */

int uart_sensor_esl_scan_start(void)
{
	LOG_INF("ESL: starting scan (oneshot)");
	last_scan_request_ms = k_uptime_get();
	return uart_sensor_esl_command("acl scan 1 1");
}

int uart_sensor_esl_scan_stop(void)
{
	LOG_INF("ESL: stopping scan");
	return uart_sensor_esl_command("acl scan 0 0");
}

int uart_sensor_esl_nus_status(uint16_t esl_id)
{
	char cmd[48];

	/* Gateway requires unicast hex address (0x<addr>); decimal silently
	 * resolves to broadcast (0x0000) and the sensor will not reply. */
	snprintf(cmd, sizeof(cmd), "nus status 0x%04x", esl_id);
	LOG_INF("ESL: requesting NUS status for tag 0x%04X", esl_id);
	return uart_sensor_esl_command(cmd);
}

int uart_sensor_esl_nus_sensors(uint16_t esl_id)
{
	char cmd[48];

	snprintf(cmd, sizeof(cmd), "nus sensors 0x%04x", esl_id);
	LOG_INF("ESL: requesting NUS sensors for tag 0x%04X", esl_id);
	return uart_sensor_esl_command(cmd);
}

int uart_sensor_esl_get_name(uint16_t esl_id)
{
	char cmd[48];
	/* Per-tag dedup state: skip duplicate requests within the cooldown
	 * window. Triggered from many sites (parse_configured, SENSOR_SYNCED,
	 * startup fan-out, MQTT command) which previously caused the same
	 * `nus get_name` to be issued 2-3 times back-to-back. */
	static int64_t last_get_name_ms[ESL_MAX_TAGS];
	static uint16_t last_get_name_addr[ESL_MAX_TAGS];
	const int64_t COOLDOWN_MS = 30 * 1000;
	const int64_t now = k_uptime_get();

	/* esl_id == 0xFFFF means "all known tags" — fan out one unicast
	 * request per tag (the gateway has no broadcast variant for this). */
	if (esl_id == 0xFFFF) {
		int sent = 0;

		for (int i = 0; i < tag_count; i++) {
			if (!tag_db[i].valid || tag_db[i].esl_addr == 0xFFFF) {
				continue;
			}
			/* Skip if we already learned the name. */
			if (tag_db[i].name[0] != '\0') {
				continue;
			}
			/* Skip if a request for this addr is still in cooldown. */
			if (i < ESL_MAX_TAGS &&
			    last_get_name_addr[i] == tag_db[i].esl_addr &&
			    (now - last_get_name_ms[i]) < COOLDOWN_MS) {
				continue;
			}
			pending_get_name_addr = tag_db[i].esl_addr;
			snprintf(cmd, sizeof(cmd), "nus get_name 0x%04x",
				 tag_db[i].esl_addr);
			(void)uart_sensor_esl_command(cmd);
			if (i < ESL_MAX_TAGS) {
				last_get_name_addr[i] = tag_db[i].esl_addr;
				last_get_name_ms[i] = now;
			}
			sent++;
			/* Pace per gateway requirement: ≥2 s between NUS cmds */
			k_sleep(K_SECONDS(2));
		}
		LOG_INF("ESL: requested sensor name for %d tag(s)", sent);
		return 0;
	}

	/* Single-target request — apply the same dedup. */
	int idx = -1;

	for (int i = 0; i < tag_count && i < ESL_MAX_TAGS; i++) {
		if (tag_db[i].esl_addr == esl_id) {
			idx = i;
			break;
		}
	}
	if (idx >= 0) {
		if (tag_db[idx].name[0] != '\0') {
			LOG_DBG("ESL get_name 0x%04X: name already known ('%s'), skipping",
				esl_id, tag_db[idx].name);
			return 0;
		}
		if (last_get_name_addr[idx] == esl_id &&
		    (now - last_get_name_ms[idx]) < COOLDOWN_MS) {
			LOG_DBG("ESL get_name 0x%04X: in cooldown (%lld ms ago), skipping",
				esl_id, (long long)(now - last_get_name_ms[idx]));
			return 0;
		}
		last_get_name_addr[idx] = esl_id;
		last_get_name_ms[idx] = now;
	}

	pending_get_name_addr = esl_id;
	snprintf(cmd, sizeof(cmd), "nus get_name 0x%04x", esl_id);
	LOG_INF("ESL: requesting sensor name for tag 0x%04X", esl_id);
	return uart_sensor_esl_command(cmd);
}

int uart_sensor_esl_set_ap_time(void)
{
	/* Force an immediate time push regardless of the sent-this-boot flag */
	ap_time_sent_this_boot = false;
	lte_time_available = true;
	send_time_to_ap();
	return 0;
}

/* ===========================================================================
 * ESL Tag Database — simple array of known tags
 * =========================================================================== */

static int esl_tag_db_find_or_add(uint16_t esl_addr)
{
	for (int i = 0; i < tag_count; i++) {
		if (tag_db[i].esl_addr == esl_addr) {
			return i;
		}
	}
	if (tag_count >= ESL_MAX_TAGS) {
		LOG_WRN("Tag DB full (%d), cannot add 0x%04X", ESL_MAX_TAGS, esl_addr);
		return -ENOMEM;
	}
	int idx = tag_count++;
	memset(&tag_db[idx], 0, sizeof(tag_db[idx]));
	tag_db[idx].esl_addr = esl_addr;
	tag_db[idx].valid = true;
	LOG_INF("Tag DB: added esl_addr=0x%04X at index %d", esl_addr, idx);
	return idx;
}

static float esl_battery_pct_from_mv(uint32_t batt_mv)
{
	if (batt_mv == 0) {
		return 0.0f;
	}

	float range_mv = (float)(UART_BATTERY_MAX_MV - UART_BATTERY_MIN_MV);

	if (range_mv <= 0.0f) {
		return 0.0f;
	}

	float batt_pct = ((float)batt_mv - (float)UART_BATTERY_MIN_MV) * 100.0f / range_mv;

	if (batt_pct > 100.0f) {
		batt_pct = 100.0f;
	}
	if (batt_pct < 0.0f) {
		batt_pct = 0.0f;
	}

	return batt_pct;
}

static void esl_store_sensor_snapshot(uint16_t esl_addr, int tag_index, bool publish)
{
	if (tag_index < 0) {
		return;
	}

	struct uart_sensor_msg snapshot = { 0 };

	snapshot.type = UART_SENSOR_ESL_NUS_RESPONSE;
	snapshot.timestamp = k_uptime_get();
	snprintf(snapshot.probe_id, sizeof(snapshot.probe_id),
		 "ESL_0x%04X", esl_addr);
	snapshot.tag_info = tag_db[tag_index];
	snapshot.temperature = tag_db[tag_index].temperature;
	snapshot.probe_battery = esl_battery_pct_from_mv(tag_db[tag_index].battery_mv);
	snapshot.data_quality.probe_id_valid = true;
	snapshot.data_quality.temperature_valid = (tag_db[tag_index].temperature != 0.0f);
	snapshot.data_quality.battery_valid = (tag_db[tag_index].battery_mv > 0);

	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data = snapshot;
	k_mutex_unlock(&uart_data_mutex);

	if (publish && uart_module_state.auto_publish_enabled) {
		zbus_chan_pub(&UART_SENSOR_CHAN, &snapshot, K_MSEC(UART_ZBUS_TIMEOUT_MS));
	}
}

int uart_sensor_esl_get_tag_count(void)
{
	return tag_count;
}

int uart_sensor_esl_get_expected_tags(void)
{
	return expected_tag_count;
}

int uart_sensor_esl_get_tag_info(uint16_t esl_addr, struct esl_tag_info *info)
{
	if (!info) {
		return -EINVAL;
	}
	for (int i = 0; i < tag_count; i++) {
		if (tag_db[i].esl_addr == esl_addr) {
			memcpy(info, &tag_db[i], sizeof(*info));
			return 0;
		}
	}
	return -ENOENT;
}

int uart_sensor_esl_get_tag_info_at(int index, struct esl_tag_info *info)
{
	if (!info || index < 0 || index >= tag_count) {
		return -EINVAL;
	}
	memcpy(info, &tag_db[index], sizeof(*info));
	return 0;
}

/* ===========================================================================
 * ESL Response Parser
 *
 * The nRF5340 shell outputs lines like:
 *   #SCAN:Start
 *   #TAG_SCANNED: 1,E9:76:EE:E9:06:5F (random) to list
 *   Connected:Conn_idx:00
 *   #CONFIGURED: 1,0x0000,0x0000
 *   #NUS_RSP:CMD=0x01,STATUS=OK,DATA=0x600f2610
 *   #NUS_STATUS:UP=3936s,BATT=4134mV,FLAGS=0x01
 *   ESL_AP:~$
 *   > esl_c ...   (command echo)
 *   Controller version: 8270
 *
 * We parse the #-prefixed URCs and extract structured data.
 * Everything else is forwarded as UART_SENSOR_GENERIC_RESPONSE.
 * =========================================================================== */

/* NOTE: publish_generic() removed — the nRF9151 no longer publishes
 * unstructured/generic UART text to MQTT. Only parsed, structured data
 * (ESL_TAG_FOUND, ESL_NUS_RESPONSE, ESL_TAG_CONFIGURED, etc.) is sent
 * to the cloud. This eliminates data/battery waste from forwarding
 * shell errors, boot banners, and other noise. */

/**
 * Parse "#NUS_STATUS:UP=3936s,BATT=4134mV,FLAGS=0x01" style response.
 * Also handles the more detailed "#NUS_RSP:CMD=0x01,STATUS=OK,DATA=0x600f2610"
 */
static int parse_nus_status(const char *line)
{
	uint32_t uptime = 0;
	uint16_t batt = 0;
	uint8_t flags = 0;
	const char *p;
	uint16_t esl_addr = last_polled_esl_addr;

	p = strstr(line, "ADDR=");
	if (p) {
		unsigned int parsed_addr = 0;

		if (sscanf(p, "ADDR=0x%x", &parsed_addr) == 1 ||
		    sscanf(p, "ADDR=%u", &parsed_addr) == 1) {
			esl_addr = (uint16_t)parsed_addr;
		}
	}

	/* Try #NUS_STATUS: format first */
	p = strstr(line, "UP=");
	if (p) {
		uptime = (uint32_t)strtoul(p + 3, NULL, 10);
	}
	p = strstr(line, "BATT=");
	if (p) {
		batt = (uint16_t)strtoul(p + 5, NULL, 10);
	}
	p = strstr(line, "FLAGS=");
	if (p) {
		flags = (uint8_t)strtoul(p + 6, NULL, 0);
	}

	if (batt == 0 && uptime == 0) {
		return -EINVAL; /* Didn't parse anything useful */
	}

	if (esl_addr == 0x0000 || esl_addr == 0xFFFF) {
		LOG_WRN("Ignoring NUS status with unknown ESL address: %s", line);
		return -EINVAL;
	}

	/* Optional temperature field: TEMP=22.50 (in °C) */
	float temperature = 0.0f;
	bool has_temp = false;
	p = strstr(line, "TEMP=");
	if (p) {
		temperature = strtof(p + 5, NULL);
		has_temp = true;
	}

	/* Prefer the ADDR field emitted by current gateway firmware. Older firmware
	 * did not include it, so fall back to last_polled_esl_addr for legacy logs. */
	int idx = esl_tag_db_find_or_add(esl_addr);
	if (idx >= 0) {
		tag_db[idx].uptime_s = uptime;
		tag_db[idx].battery_mv = batt;
		tag_db[idx].flags = flags;
		tag_db[idx].last_seen = k_uptime_get();
		if (has_temp) {
			tag_db[idx].temperature = temperature;
		}
	}

	/* Publish as sensor data */
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data.type = UART_SENSOR_ESL_NUS_RESPONSE;
	current_sensor_data.probe_battery = (batt > 0) ?
		(float)((batt - UART_BATTERY_MIN_MV) * 100) /
		(UART_BATTERY_MAX_MV - UART_BATTERY_MIN_MV) : 0.0f;
	if (current_sensor_data.probe_battery > 100.0f) {
		current_sensor_data.probe_battery = 100.0f;
	}
	if (current_sensor_data.probe_battery < 0.0f) {
		current_sensor_data.probe_battery = 0.0f;
	}
	current_sensor_data.timestamp = k_uptime_get();
	snprintf(current_sensor_data.probe_id,
		 sizeof(current_sensor_data.probe_id),
		 "ESL_0x%04X", esl_addr);
	if (idx >= 0) {
		current_sensor_data.tag_info = tag_db[idx];
	}
	current_sensor_data.data_quality.battery_valid = (batt > 0);
	current_sensor_data.data_quality.probe_id_valid = true;
	if (has_temp) {
		current_sensor_data.tag_info.temperature = temperature;
	}
	k_mutex_unlock(&uart_data_mutex);

	if (has_temp) {
		LOG_INF("NUS Status: UP=%us, BATT=%umV, FLAGS=0x%02X, TEMP=%.2f°C",
			uptime, batt, flags, (double)temperature);
	} else {
		LOG_INF("NUS Status: UP=%us, BATT=%umV, FLAGS=0x%02X",
			uptime, batt, flags);
	}

	/* Track response for timeout detection */
	last_nus_response_time = k_uptime_get();
	nus_consecutive_poll_failures = 0;

	if (uart_module_state.auto_publish_enabled) {
		k_work_reschedule_for_queue(&spi_work_q, &nus_settle_work, K_MSEC(200));
	}
	update_statistics(true, true, true);
	return 0;
}

/**
 * Parse "#TAG_SCANNED: 1,E9:76:EE:E9:06:5F (random) to list"
 */
static int parse_tag_scanned(const char *line)
{
	/* Format: "#TAG_SCANNED: <idx>,<mac> (<type>) to list" */
	const char *p = line + strlen("#TAG_SCANNED:");
	while (*p == ' ') p++;

	int idx_unused;
	char mac[18] = {0};
	if (sscanf(p, "%d,%17s", &idx_unused, mac) < 2) {
		return -EINVAL;
	}
	/* Strip trailing " (random)" or " (public)" from mac */
	char *space = strchr(mac, ' ');
	if (space) *space = '\0';

	LOG_INF("Tag scanned: %s", mac);

	/* Try to add to DB with address 0xFFFF (unassigned) until configured */
	int db_idx = -1;
	for (int i = 0; i < tag_count; i++) {
		if (strcmp(tag_db[i].mac, mac) == 0) {
			db_idx = i;
			break;
		}
	}
	if (db_idx < 0 && tag_count < ESL_MAX_TAGS) {
		db_idx = tag_count++;
		memset(&tag_db[db_idx], 0, sizeof(tag_db[db_idx]));
		tag_db[db_idx].esl_addr = 0xFFFF; /* Not yet assigned */
		tag_db[db_idx].valid = true;
	}
	if (db_idx >= 0) {
		strncpy(tag_db[db_idx].mac, mac, sizeof(tag_db[db_idx].mac) - 1);
		tag_db[db_idx].last_seen = k_uptime_get();
	}

	/* Publish event */
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data.type = UART_SENSOR_ESL_TAG_FOUND;
	strncpy(current_sensor_data.probe_id, mac, sizeof(current_sensor_data.probe_id) - 1);
	current_sensor_data.timestamp = k_uptime_get();
	if (db_idx >= 0) {
		current_sensor_data.tag_info = tag_db[db_idx];
	}
	k_mutex_unlock(&uart_data_mutex);

	if (uart_module_state.auto_publish_enabled) {
		zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data,
			      K_MSEC(UART_ZBUS_TIMEOUT_MS));
	}
	return 0;
}

/**
 * Parse "#CONFIGURED: <success>,0x<conn_idx>,0x<esl_addr>"
 *
 * Gateway source (esl_client.c):
 *   printk("#CONFIGURED: 1,0x%04x,0x%04x\n", config_work->conn_idx,
 *          config_work->param.esl_addr);
 *   printk("#CONFIGURED: 0,0x%02x,0x%04x\n …",  // failure path
 *
 * So field 1 is success/fail (1 = configured OK), field 2 is the connection
 * index (NOT the ESL address \u2014 our previous parser had this swapped, which
 * caused a phantom 0x0000 tag to appear in the DB), field 3 is the real
 * ESL address that was provisioned.
 */
static int parse_configured(const char *line)
{
	const char *p = line + strlen("#CONFIGURED:");
	while (*p == ' ') p++;

	int success = 0;
	unsigned int conn_idx = 0, esl_addr = 0;
	if (sscanf(p, "%d,0x%x,0x%x", &success, &conn_idx, &esl_addr) < 3) {
		LOG_WRN("Could not parse #CONFIGURED: %s", line);
		return -EINVAL;
	}

	if (success != 1) {
		LOG_WRN("Tag configure FAILED: conn_idx=0x%02X esl_addr=0x%04X",
			conn_idx, esl_addr);
		return 0;
	}

	if (esl_addr == 0x0000 || esl_addr == 0xFFFF) {
		LOG_WRN("Ignoring #CONFIGURED with invalid esl_addr=0x%04X", esl_addr);
		return 0;
	}

	LOG_INF("Tag configured: conn_idx=0x%02X esl_addr=0x%04X", conn_idx, esl_addr);

	/* Track which tag is now active so subsequent SENSOR_REPORT/NUS_STATUS can
	 * be associated with the correct tag address. */
	last_polled_esl_addr = (uint16_t)esl_addr;

	int db_idx = esl_tag_db_find_or_add((uint16_t)esl_addr);
	if (db_idx >= 0) {
		tag_db[db_idx].last_seen = k_uptime_get();
	}

	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data.type = UART_SENSOR_ESL_TAG_CONFIGURED;
	snprintf(current_sensor_data.probe_id,
		 sizeof(current_sensor_data.probe_id),
		 "ESL_0x%04X", esl_addr);
	current_sensor_data.timestamp = k_uptime_get();
	k_mutex_unlock(&uart_data_mutex);

	if (uart_module_state.auto_publish_enabled) {
		zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data,
			      K_MSEC(UART_ZBUS_TIMEOUT_MS));
	}
	/* Request human-readable name for this newly configured tag.
	 * Use the unicast helper \u2014 bare "nus get_name" defaults to 0x0001
	 * and is silently dropped for any other address. */
	(void)uart_sensor_esl_get_name((uint16_t)esl_addr);
	return 0;
}

/* Main line processor — dispatches to specific parsers */
int uart_sensor_process_data_line(const char *data)
{
	if (!data || data[0] == '\0') {
		return -EINVAL;
	}

	uart_module_state.stats.messages_received++;

	/* ---- Detect shell prompt — purely informational when startup is master-driven ---- */
	if (strstr(data, "ESL_AP:~$") != NULL || strstr(data, "uart:~$") != NULL) {
		LOG_DBG("nRF5340 shell prompt received (startup already running)");
		return 0; /* Don't forward prompt noise */
	}

	/* ---- Ignore command echoes (">" prefix or "esl_c" echo) ---- */
	if (data[0] == '>' || strncmp(data, "esl_c ", 6) == 0) {
		LOG_DBG("Shell echo: %s", data);
		return 0;
	}

	/* ---- Parse #-prefixed URC responses ---- */

	/* #TAG_SCANNED: ... */
	if (strncmp(data, "#TAG_SCANNED:", 13) == 0) {
		parse_tag_scanned(data);
		return 0;
	}

	/* #NUS_STATUS: or #NUS_RSP: */
	if (strncmp(data, "#NUS_STATUS:", 12) == 0 ||
	    strncmp(data, "#NUS_RSP:", 9) == 0) {
		parse_nus_status(data);
		return 0;
	}

	/* #NUS_SENSORS: — NUS command 0x02 response from ESL tag via PAwR
	 * Real nRF5340 format: #NUS_SENSORS:TEMP=%d.%02d,BATT=%umV,CNT=%d
	 * Example: #NUS_SENSORS:TEMP=22.50,BATT=3600mV,CNT=3 */
	if (strncmp(data, "#NUS_SENSORS:", 13) == 0) {
		float temp = 0.0f;
		uint32_t batt_mv = 0;
		uint32_t cnt = 0;
		bool has_temp = false;
		const char *p;

		p = strstr(data, "TEMP=");
		if (p) {
			temp = strtof(p + 5, NULL);
			has_temp = true;
		}
		/* BATT=3600mV — strtoul stops at 'm', so we get the numeric value */
		p = strstr(data, "BATT=");
		if (p) {
			batt_mv = (uint32_t)strtoul(p + 5, NULL, 10);
		}
		p = strstr(data, "CNT=");
		if (p) {
			cnt = (uint32_t)strtoul(p + 4, NULL, 10);
		}

		int idx = esl_tag_db_find_or_add(last_polled_esl_addr);
		if (idx >= 0) {
			if (has_temp) {
				tag_db[idx].temperature = temp;
			}
			if (batt_mv > 0) {
				tag_db[idx].battery_mv = (uint16_t)batt_mv;
			}
			tag_db[idx].last_seen = k_uptime_get();
		}

		float batt_pct = (batt_mv > 0) ?
			(float)((batt_mv - UART_BATTERY_MIN_MV) * 100) /
			(UART_BATTERY_MAX_MV - UART_BATTERY_MIN_MV) : 0.0f;
		if (batt_pct > 100.0f) batt_pct = 100.0f;
		if (batt_pct < 0.0f) batt_pct = 0.0f;

		LOG_INF("ESL NUS Sensors: TEMP=%.2f°C, BATT=%umV (%.0f%%), CNT=%u",
			(double)temp, batt_mv, (double)batt_pct, cnt);

		k_mutex_lock(&uart_data_mutex, K_FOREVER);
		current_sensor_data.type = UART_SENSOR_ESL_NUS_RESPONSE;
		if (has_temp) {
			current_sensor_data.tag_info.temperature = temp;
			current_sensor_data.data_quality.temperature_valid = true;
		}
		if (batt_mv > 0) {
			current_sensor_data.probe_battery = batt_pct;
			current_sensor_data.data_quality.battery_valid = true;
			if (idx >= 0) {
				current_sensor_data.tag_info = tag_db[idx];
			}
		}
		current_sensor_data.timestamp = k_uptime_get();
		snprintf(current_sensor_data.probe_id,
			 sizeof(current_sensor_data.probe_id),
			 "ESL_0x%04X", last_polled_esl_addr);
		current_sensor_data.data_quality.probe_id_valid = true;
		k_mutex_unlock(&uart_data_mutex);

		last_nus_response_time = k_uptime_get();
		nus_consecutive_poll_failures = 0;
		if (uart_module_state.auto_publish_enabled) {
			k_work_reschedule_for_queue(&spi_work_q, &nus_settle_work, K_MSEC(200));
		}
		update_statistics(true, true, true);
		return 0;
	}

	/* #SENSOR_REPORT:TEMP,0x<ADDR>,<val> — PaWR periodic temperature from ESL tag */
	if (strncmp(data, "#SENSOR_REPORT:TEMP,", 20) == 0) {
		unsigned int esl_addr_u = 0;
		float temp = 0.0f;

		if (sscanf(data + 20, "0x%x,%f", &esl_addr_u, &temp) != 2) {
			LOG_WRN("Bad SENSOR_REPORT TEMP format: %s", data);
			return 0;
		}
		uint16_t esl_addr = (uint16_t)esl_addr_u;
		if (esl_addr == 0x0000 || esl_addr == 0xFFFF) {
			LOG_WRN("Ignoring SENSOR_REPORT TEMP for invalid addr 0x%04X", esl_addr);
			return 0;
		}
		int idx = esl_tag_db_find_or_add(esl_addr);

		if (idx >= 0) {
			tag_db[idx].temperature = temp;
			tag_db[idx].last_seen = k_uptime_get();
		}
		esl_store_sensor_snapshot(esl_addr, idx, false);
		LOG_INF("PaWR TEMP: ESL_0x%04X %.2f C", esl_addr, (double)temp);
		return 0;
	}

	/* #SENSOR_REPORT:BATT,0x<ADDR>,<mV> — PaWR periodic battery voltage from ESL tag */
	if (strncmp(data, "#SENSOR_REPORT:BATT,", 20) == 0) {
		unsigned int esl_addr_u = 0;
		unsigned int batt_mv_u = 0;

		if (sscanf(data + 20, "0x%x,%u", &esl_addr_u, &batt_mv_u) != 2) {
			LOG_WRN("Bad SENSOR_REPORT BATT format: %s", data);
			return 0;
		}
		uint16_t esl_addr = (uint16_t)esl_addr_u;
		if (esl_addr == 0x0000 || esl_addr == 0xFFFF) {
			LOG_WRN("Ignoring SENSOR_REPORT BATT for invalid addr 0x%04X", esl_addr);
			return 0;
		}
		uint32_t batt_mv = (uint32_t)batt_mv_u;
		int idx = esl_tag_db_find_or_add(esl_addr);

		if (idx >= 0) {
			tag_db[idx].battery_mv = (uint16_t)batt_mv;
			tag_db[idx].last_seen = k_uptime_get();
		}
		float batt_pct = esl_battery_pct_from_mv(batt_mv);

		LOG_INF("PaWR BATT: ESL_0x%04X %u mV (%.0f%%)", esl_addr, batt_mv, (double)batt_pct);
		esl_store_sensor_snapshot(esl_addr, idx, true);
		return 0;
	}

	/* #CONFIGURED: */
	if (strncmp(data, "#CONFIGURED:", 12) == 0) {
		parse_configured(data);
		return 0;
	}

	/* #SCAN:Start / #SCAN:Stop — track window, trigger discovery check on Stop */
	if (strncmp(data, "#SCAN:", 6) == 0) {
		const char *evt = data + 6;

		if (strcmp(evt, "Start") == 0) {
			scan_active = true;
			LOG_INF("ESL Scan started");
		} else if (strcmp(evt, "Stop") == 0) {
			scan_active = false;
			LOG_INF("ESL Scan stopped — checking discovery");
			esl_discovery_check();
		} else {
			LOG_INF("ESL Scan: %s", evt);
		}
		return 0;
	}

	/* Connected/Disconnected — BLE connection events during provisioning */
	if (strncmp(data, "#SENSOR_SYNCED:0x", 17) == 0) {
		/* Tag has joined the PAwR train. Per the gateway protocol guide
		 * (NRF91_USAGE_GUIDE.md §3) we should immediately request the
		 * human-readable name once \u2014 BEFORE the next auto-poll round.
		 */
		unsigned int esl_addr_u = 0;

		if (sscanf(data + 17, "%x", &esl_addr_u) == 1) {
			uint16_t addr = (uint16_t)esl_addr_u;
			int idx = esl_tag_db_find_or_add(addr);

			if (idx >= 0) {
				tag_db[idx].connected = true;
				tag_db[idx].last_seen = k_uptime_get();
			}
			LOG_INF("ESL synced: 0x%04X \u2014 fetching name", addr);
			(void)uart_sensor_esl_get_name(addr);
			esl_discovery_check();
		} else {
			LOG_WRN("Bad SENSOR_SYNCED format: %s", data);
		}
		return 0;
	}

	if (strncmp(data, "#SENSOR_DESYNC:0x", 17) == 0) {
		/* Tag dropped off PAwR. Do NOT auto-rescan \u2014 the gateway and the
		 * sensor will retry sync on their own. Just mark stale for MQTT.
		 */
		unsigned int esl_addr_u = 0;

		if (sscanf(data + 17, "%x", &esl_addr_u) == 1) {
			uint16_t addr = (uint16_t)esl_addr_u;

			for (int di = 0; di < tag_count; di++) {
				if (tag_db[di].esl_addr == addr) {
					tag_db[di].connected = false;
					LOG_WRN("ESL desynced: 0x%04X (waiting for auto-resync)",
						addr);
					break;
				}
			}
		}
		return 0;
	}

	if (strncmp(data, "Connected:", 10) == 0) {
		LOG_INF("ESL BLE connect: %s", data);
		return 0;
	}
	if (strncmp(data, "Disconnected:", 13) == 0) {
		const char *after = data + 13; /* skip "Disconnected:" */

		/* "Disconnected:Conn_idx:0xNN" — normal end of provisioning BLE connection.
		 * This is NOT a tag loss event; do not publish esl_tag_disconnected.
		 * The tag has been provisioned and will now communicate over PAwR. */
		if (strncmp(after, "Conn_idx:", 9) == 0) {
			LOG_INF("ESL provisioning disconnect: %s", data);
			/* Rescan only if we haven't reached the expected count yet */
			int conn = esl_count_connected();
			if (conn < expected_tag_count) {
				k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work,
							    K_SECONDS(15));
			}
			return 0;
		}

		/* "Disconnected: AA:BB:CC:DD:EE:FF ..." — actual BLE disconnect with MAC */
		LOG_INF("ESL disconnected: %s", data);
		char mac[18] = {0};
		/* Skip optional space after colon */
		const char *p = after;
		if (*p == ' ') p++;
		size_t i = 0;
		while (i < 17 && p[i] && p[i] != ' ' && p[i] != '\r' && p[i] != '\n') {
			mac[i] = p[i];
			i++;
		}
		mac[i] = '\0';

		/* Mark tag as disconnected in database */
		for (int di = 0; di < tag_count; di++) {
			if (strncmp(tag_db[di].mac, mac, 17) == 0) {
				tag_db[di].connected = false;
				LOG_INF("Marked ESL 0x%04X (%s) as disconnected",
					tag_db[di].esl_addr, mac);
				break;
			}
		}

		int conn = esl_count_connected();
		if (conn < expected_tag_count) {
			LOG_WRN("Tag lost: %d/%d connected — rescan in 15 s", conn, expected_tag_count);
			k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work, K_SECONDS(15));
		}

		if (uart_module_state.auto_publish_enabled) {
			struct uart_sensor_msg disc_msg = {
				.type = UART_SENSOR_ESL_TAG_DISCONNECTED,
				.timestamp = k_uptime_get(),
			};
			strncpy(disc_msg.tag_info.mac, mac, sizeof(disc_msg.tag_info.mac) - 1);
			strncpy(disc_msg.probe_id, mac, sizeof(disc_msg.probe_id) - 1);
			zbus_chan_pub(&UART_SENSOR_CHAN, &disc_msg, K_MSEC(UART_ZBUS_TIMEOUT_MS));
		}
		return 0;
	}

	/* #TAG: connection event — format: #TAG:0xAAAA,XX:XX:XX:XX:XX:XX ... */
	if (strncmp(data, "#TAG:", 5) == 0) {
		LOG_INF("ESL Tag event: %s", data);
		/* Parse: #TAG:0x<esl_addr>,<mac> ... */
		const char *p = data + 5;
		unsigned int esl_addr = 0;
		char mac[18] = {0};
		if (sscanf(p, "0x%x,%17s", &esl_addr, mac) >= 2) {
			/* Strip trailing spaces from mac if any */
			char *sp = strchr(mac, ' ');
			if (sp) *sp = '\0';

			int db_idx = esl_tag_db_find_or_add((uint16_t)esl_addr);
			if (db_idx >= 0) {
				strncpy(tag_db[db_idx].mac, mac, sizeof(tag_db[db_idx].mac) - 1);
				tag_db[db_idx].last_seen = k_uptime_get();
			}

			/* Track active tag address for subsequent SENSOR_REPORT/NUS_STATUS */
			last_polled_esl_addr = (uint16_t)esl_addr;

			/* Mark tag as BLE-connected */
			tag_db[db_idx].connected = true;

			/* Publish as ESL_TAG_CONNECTED */
			k_mutex_lock(&uart_data_mutex, K_FOREVER);
			current_sensor_data.type = UART_SENSOR_ESL_TAG_CONNECTED;
			strncpy(current_sensor_data.probe_id, mac,
				sizeof(current_sensor_data.probe_id) - 1);
			current_sensor_data.timestamp = k_uptime_get();
			if (db_idx >= 0) {
				current_sensor_data.tag_info = tag_db[db_idx];
			}
			k_mutex_unlock(&uart_data_mutex);

			if (uart_module_state.auto_publish_enabled) {
				zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data,
					      K_MSEC(UART_ZBUS_TIMEOUT_MS));
			}

			/* Check if we now have all expected tags */
			esl_discovery_check();
		}
		return 0;
	}

	/* #PAWR_TX_FAIL:consec=N — PAwR transmit failures from nRF5340 */
	if (strncmp(data, "#PAWR_TX_FAIL:", 14) == 0) {
		int consec = 0;
		const char *fp = strstr(data, "consec=");

		if (fp) {
			consec = (int)strtol(fp + 7, NULL, 10);
		}
		LOG_WRN("PAWR TX fail: consec=%d", consec);
		if (consec >= 3) {
			LOG_WRN("PAWR TX fail threshold reached — scheduling urgent rescan in 5 s");
			k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work, K_SECONDS(5));
		}
		return 0;
	}

	/* #RSP_KEY: */
	if (strncmp(data, "#RSP_KEY:", 9) == 0) {
		LOG_DBG("ESL Response key received");
		return 0;
	}

	/* #DISCOVERY: */
	if (strncmp(data, "#DISCOVERY:", 11) == 0) {
		LOG_DBG("ESL Discovery: %s", data);
		return 0;
	}

	/* #PAST: */
	if (strncmp(data, "#PAST:", 6) == 0) {
		LOG_INF("ESL PAST: %s", data);
		return 0;
	}

	/* #BASIC_STATE: */
	if (strncmp(data, "#BASIC_STATE:", 13) == 0) {
		LOG_INF("ESL Basic State: %s", data);
		return 0;
	}

	/* #RESPONSE: / #SLOT: — NUS sub-responses */
	if (strncmp(data, "#RESPONSE:", 10) == 0 ||
	    strncmp(data, "#SLOT:", 6) == 0) {
		LOG_DBG("ESL NUS sub-response: %s", data);
		return 0;
	}

	/* SERVICE_NEEDED / SYNCHRONIZED / ACTIVE_LED etc — basic state flags */
	if (strstr(data, ":on") != NULL || strstr(data, ":off") != NULL) {
		LOG_DBG("ESL flag: %s", data);
		return 0;
	}

	/* #AP_GET_TIME — nRF5340 is requesting UTC time; respond with current epoch */
	if (strcmp(data, "#AP_GET_TIME") == 0) {
		LOG_INF("AP requested time (#AP_GET_TIME) — responding");
		send_time_to_ap();
		return 0;
	}

	/* #AP_EPOCH_SET:<epoch> — AP confirming it received our AP_SET_TIME */
	if (strncmp(data, "#AP_EPOCH_SET:", 14) == 0) {
		LOG_INF("AP confirmed epoch set: %s", data + 14);
		return 0;
	}

	/* #SENSOR_NAME:0x<addr>,<name>  or  #SENSOR_NAME:ERR=0x<code> */
	if (strncmp(data, "#SENSOR_NAME:", 13) == 0) {
		const char *body = data + 13;

		if (strncmp(body, "ERR=", 4) == 0) {
			LOG_WRN("Sensor name request error: %s", body);
			return 0;
		}

		uint16_t esl_addr = 0;
		char sname[13] = {0};

		if (sscanf(body, "0x%4hx,%12s", &esl_addr, sname) == 2) {
			/* Trim any trailing whitespace */
			for (int si = (int)strlen(sname) - 1;
			     si >= 0 && (sname[si] == ' ' || sname[si] == '\r' || sname[si] == '\n');
			     si--) {
				sname[si] = '\0';
			}

			/* The gateway reconstructs resp_addr from
			 *   (group_id << 8) | last_nus_target_low[group_id]
			 * which can resolve to 0x0000 (e.g. for group 0 before
			 * the per-group target table has been populated, or
			 * when the response arrives via the broadcast slot).
			 * In that case attribute the reply to the most recent
			 * unicast `nus get_name 0x<addr>` we issued. */
			if (esl_addr == 0x0000 && pending_get_name_addr != 0x0000) {
				LOG_INF("SENSOR_NAME: attributing 0x0000 reply to pending 0x%04X",
					pending_get_name_addr);
				esl_addr = pending_get_name_addr;
			}
			/* Clear pending regardless \u2014 a fresh request will set it again. */
			pending_get_name_addr = 0x0000;

			if (esl_addr == 0x0000) {
				LOG_WRN("Ignoring SENSOR_NAME for broadcast addr 0x0000 (no pending request): '%s'",
					sname);
				return 0;
			}

			int db_idx = esl_tag_db_find_or_add(esl_addr);

			if (db_idx >= 0) {
				strncpy(tag_db[db_idx].name, sname, sizeof(tag_db[db_idx].name) - 1);
				tag_db[db_idx].name[sizeof(tag_db[db_idx].name) - 1] = '\0';
				LOG_INF("Sensor name: 0x%04X = '%s'", esl_addr, sname);
				/* Persist to flash so name survives reboot */
				char stg_key[32];
				snprintf(stg_key, sizeof(stg_key),
					 "app/tag/%04x/name", (unsigned)esl_addr);
				settings_save_one(stg_key, sname, strlen(sname) + 1);
			}

			/* Publish name-event to MQTT via ZBUS */
			struct uart_sensor_msg name_msg = {
				.type      = UART_SENSOR_ESL_NAME_RESPONSE,
				.timestamp = k_uptime_get(),
			};

			name_msg.tag_info.esl_addr = esl_addr;
			strncpy(name_msg.tag_info.name, sname, sizeof(name_msg.tag_info.name) - 1);
			snprintf(name_msg.probe_id, sizeof(name_msg.probe_id),
				 "ESL_0x%04X", esl_addr);
			if (db_idx >= 0) {
				name_msg.tag_info = tag_db[db_idx];
			}
			zbus_chan_pub(&UART_SENSOR_CHAN, &name_msg,
				      K_MSEC(UART_ZBUS_TIMEOUT_MS));
		} else {
			LOG_WRN("Could not parse #SENSOR_NAME: %s", data);
		}
		return 0;
	}

	/* ---- Sensor whitelist management events ---- */

	/* #SENSOR_MGMT:STATUS,MODE=<mode>,COUNT=<n>,PROVISIONED=<0|1>
	 * #SENSOR_MGMT:ADDED=<addr>,COUNT=<n>
	 * #SENSOR_MGMT:REMOVED=<addr>,COUNT=<n>
	 * #SENSOR_MGMT:CLEARED
	 * #SENSOR_MGMT:MODE=<mode>
	 * #SENSOR_MGMT:PROVISIONED,COUNT=<n>,MODE=<mode>
	 * #SENSOR_MGMT:ENTRY=<idx>,<addr>
	 * #SENSOR_MGMT:LIST_DONE,COUNT=<n>
	 * #SENSOR_MGMT:KEY_SET=<addr>
	 */
	if (strncmp(data, "#SENSOR_MGMT:", 13) == 0) {
		const char *evt = data + 13;

		LOG_INF("Sensor mgmt event: %s", evt);

		/* Capture provisioning state so the boot sequence can decide
		 * whether scanning is even allowed (see NRF91_USAGE_GUIDE.md \u00a72). */
		if (strncmp(evt, "STATUS,", 7) == 0) {
			const char *pp = strstr(evt, "PROVISIONED=");

			if (pp) {
				gateway_provisioned = (pp[12] == '1') ? 1 : 0;
				LOG_INF("Gateway provisioned=%d", gateway_provisioned);
			}
		} else if (strncmp(evt, "PROVISIONED", 11) == 0) {
			gateway_provisioned = 1;
			LOG_INF("Gateway just transitioned to PROVISIONED");
		} else if (strncmp(evt, "CLEARED", 7) == 0) {
			gateway_provisioned = 0;
		}

		/* Publish to MQTT via zbus using response_text */
		struct uart_sensor_msg mgmt_msg = {
			.type      = UART_SENSOR_SENSOR_MGMT_EVENT,
			.timestamp = k_uptime_get(),
		};
		strncpy(mgmt_msg.response_text, evt,
			sizeof(mgmt_msg.response_text) - 1);
		zbus_chan_pub(&UART_SENSOR_CHAN, &mgmt_msg,
			      K_MSEC(UART_ZBUS_TIMEOUT_MS));
		return 0;
	}

	/* #SENSOR_BLOCKED:<addr>,reason=not_in_whitelist */
	if (strncmp(data, "#SENSOR_BLOCKED:", 16) == 0) {
		const char *body = data + 16;

		LOG_INF("Sensor blocked: %s", body);

		struct uart_sensor_msg blk_msg = {
			.type      = UART_SENSOR_SENSOR_BLOCKED,
			.timestamp = k_uptime_get(),
		};
		strncpy(blk_msg.response_text, body,
			sizeof(blk_msg.response_text) - 1);
		zbus_chan_pub(&UART_SENSOR_CHAN, &blk_msg,
			      K_MSEC(UART_ZBUS_TIMEOUT_MS));
		return 0;
	}

	/* #PAST_FAIL / #PAST_FALLBACK */
	if (strncmp(data, "#PAST_FAIL:", 11) == 0 ||
	    strncmp(data, "#PAST_FALLBACK", 14) == 0 ||
	    strncmp(data, "#PAST_START:", 12) == 0) {
		LOG_WRN("ESL PAST issue: %s", data);
		return 0;
	}

	/* Controller version: NNNN (boot-time message) */
	if (strncmp(data, "Controller version:", 19) == 0) {
		LOG_INF("nRF5340: %s", data);
		return 0;
	}

	/* ---- Filter ANSI escape / Zephyr shell error messages ---- */
	/* nRF5340 shell prints errors as: \x1b[1;31m<cmd>: command not found\x1b[m
	 * The ESC byte is filtered by ISR, so we see "[1;31m..." */
	if (strstr(data, "[1;31m") != NULL ||
	    strstr(data, ": command not found") != NULL ||
	    strstr(data, ": unknown parameter") != NULL) {
		LOG_WRN("nRF5340 shell error (not forwarded): %s", data);
		return 0; /* Discard — do NOT publish to MQTT */
	}

	/* #CONF#BASIC_STATE: — combined configure+state URC from some nRF5340 fw versions
	 * Format: #CONF#BASIC_STATE:<n>,0x<esl_addr>,0x<group>
	 * Treat identically to #CONFIGURED: */
	if (strncmp(data, "#CONF#BASIC_STATE:", 18) == 0) {
		char configured_line[128];
		snprintf(configured_line, sizeof(configured_line),
			 "#CONFIGURED:%s", data + 18);
		parse_configured(configured_line);
		return 0;
	}

	/* ---- Filter ANSI reset sequences ---- */
	if (strcmp(data, "[m") == 0 || strcmp(data, "[0m") == 0) {
		return 0;
	}

	/* ---- Filter boot banners ---- */
	if (strncmp(data, "*** Booting", 11) == 0 ||
	    strncmp(data, "*** Using", 9) == 0) {
		LOG_INF("nRF5340 boot: %s", data);
		return 0;
	}

	/* ---- Filter #PAST_SKIP (informational, not actionable) ---- */
	if (strncmp(data, "#PAST_SKIP:", 11) == 0) {
		LOG_DBG("ESL PAST skip: %s", data);
		return 0;
	}

	/* ---- Legacy sensor format (<name>:<temp>,<hum>,<bat_mv>) ----
	 * IMPORTANT: Only match plain name:value strings. Any line starting
	 * with '#' is a URC and must NOT reach this parser — add new #-handlers
	 * above. Lines starting with '#' that fall through here cause false
	 * matches (e.g. #CONF#BASIC_STATE: wrongly parsed as sensor data). */
	if (data[0] != '#') {
		char sensor_name[CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN + 1];
		float temperature, humidity;
		uint32_t batt_mv;
		char fmtstr[64];

		snprintf(fmtstr, sizeof(fmtstr), "%%%d[^:]%%*c%%f,%%f,%%u",
			 CONFIG_APP_UART_SENSOR_PROBE_ID_MAX_LEN);

		if (sscanf(data, fmtstr, sensor_name, &temperature,
			   &humidity, &batt_mv) == 4) {
			LOG_INF("Legacy sensor: %s T=%.1f H=%.1f Bat=%u",
				sensor_name, (double)temperature,
				(double)humidity, batt_mv);

			k_mutex_lock(&uart_data_mutex, K_FOREVER);
			current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
			current_sensor_data.temperature = temperature;
			current_sensor_data.humidity = humidity;
			current_sensor_data.probe_battery =
				(float)((batt_mv - UART_BATTERY_MIN_MV) * 100) /
				(UART_BATTERY_MAX_MV - UART_BATTERY_MIN_MV);
			strncpy(current_sensor_data.probe_id, sensor_name,
				sizeof(current_sensor_data.probe_id) - 1);
			current_sensor_data.timestamp = k_uptime_get();
			current_sensor_data.data_quality.temperature_valid = true;
			current_sensor_data.data_quality.humidity_valid = true;
			current_sensor_data.data_quality.battery_valid = true;
			current_sensor_data.data_quality.probe_id_valid = true;
			k_mutex_unlock(&uart_data_mutex);

			if (uart_module_state.auto_publish_enabled) {
				zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data,
					      K_MSEC(UART_ZBUS_TIMEOUT_MS));
			}
			update_statistics(true, true, true);
			return 0;
		}
	} /* end if data[0] != '#' */

	/* ---- Quietly absorb known-but-uninteresting telemetry events from the
	 * gateway that would otherwise flood the log as "unrecognized". These
	 * are diagnostic URCs we don't currently act on; keep at DBG so they
	 * are still available with verbose logging. ---- */
	if (data[0] == '#' && (
		strncmp(data, "#PAWR_POLL:",   11) == 0 ||
		strncmp(data, "#PAWR_RSP:",    10) == 0 ||
		strncmp(data, "#PAWR:",         6) == 0 ||
		strncmp(data, "#PAWR_GAP:",    10) == 0 ||
		strncmp(data, "#RADIO_STATE:", 13) == 0 ||
		strncmp(data, "#SCAN_SUMMARY:",14) == 0 ||
		strncmp(data, "#NUS_GET_NAME:",14) == 0 ||
		strncmp(data, "#NUS_SET_TIME:",14) == 0 ||
		strncmp(data, "#NUS_STATUS:",  12) == 0 ||
		strncmp(data, "#NUS_SENSORS:", 13) == 0)) {
		LOG_DBG("UART event (silent): %s", data);
		return 0;
	}

	/* Surface gateway shell errors so EBUSY/etc. are visible to the operator. */
	if (strncmp(data, ">ERR:", 5) == 0 || strncmp(data, "ERR:", 4) == 0) {
		LOG_WRN("Gateway shell error: %s", data);
		return 0;
	}

	/* ---- Anything else: log locally; publish to MQTT only when debug echo is on ---- */
	LOG_INF("UART unrecognized (not published): %s", data);

	if (uart_debug_echo_enabled && uart_module_state.auto_publish_enabled) {
		k_mutex_lock(&uart_data_mutex, K_FOREVER);
		current_sensor_data.type = UART_SENSOR_GENERIC_RESPONSE;
		strncpy(current_sensor_data.response_text, data,
			sizeof(current_sensor_data.response_text) - 1);
		current_sensor_data.response_text[sizeof(current_sensor_data.response_text) - 1] = '\0';
		current_sensor_data.timestamp = k_uptime_get();
		k_mutex_unlock(&uart_data_mutex);
		zbus_chan_pub(&UART_SENSOR_CHAN, &current_sensor_data,
			      K_MSEC(UART_ZBUS_TIMEOUT_MS));
	}

	update_statistics(true, true, true);
	return 0;
}

/* ===========================================================================
 * ESL Startup Sequence (runs step-by-step after shell prompt detected)
 *
 * Step 0: Send newline to get clean prompt
 * Step 1: Start PAwR advertising
 * Step 2: Start oneshot scan
 * Step 3: Done — enable periodic polling
 * =========================================================================== */

static void esl_startup_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	switch (esl_startup_step) {
	case 0:
		LOG_INF("ESL startup step 0: sending newline for clean prompt");
		uart_sensor_send_command("");
		esl_startup_step = 1;
		k_work_reschedule_for_queue(&spi_work_q, &esl_startup_work, K_SECONDS(1));
		break;

	case 1:
		LOG_INF("ESL startup step 1: starting PAwR + querying whitelist state");
		uart_sensor_esl_command("pawr start_pawr");
		/* Ask the gateway whether the whitelist is sealed.  The reply
		 * "#SENSOR_MGMT:STATUS,...,PROVISIONED=<0|1>" sets
		 * gateway_provisioned and gates the next step. */
		uart_sensor_mgmt_status();
		esl_startup_step = 2;
		k_work_reschedule_for_queue(&spi_work_q, &esl_startup_work, K_SECONDS(2));
		break;

	case 2:
		if (gateway_provisioned == 1) {
			LOG_INF("ESL startup step 2: gateway PROVISIONED — skipping scan");
		} else {
			LOG_INF("ESL startup step 2: gateway not provisioned (%d) — starting scan",
				gateway_provisioned);
			last_scan_request_ms = k_uptime_get();
			uart_sensor_esl_scan_start();
		}
		esl_startup_step = 3;
		k_work_reschedule_for_queue(&spi_work_q, &esl_startup_work, K_SECONDS(5));
		break;

	case 3:
		LOG_INF("ESL startup complete — starting periodic poll (every %ds)",
			esl_poll_interval_sec);
		uart_sensor_esl_poll_start();
		/* Only run discovery (which may schedule another scan) if the
		 * gateway is NOT already provisioned. When sealed, the fleet is
		 * already known and scanning would waste PA-train air-time. */
		if (gateway_provisioned != 1) {
			esl_discovery_check();
		}
		/* Try to push UTC time to AP now that it is fully booted */
		try_send_time_to_ap();
		/* Request name(s) for any already-provisioned tags.
		 * uart_sensor_esl_get_name(0xFFFF) fans out one unicast
		 * "nus get_name 0x<addr>" per known tag (paced \u22652 s).
		 * Bare "nus get_name" defaults to broadcast 0x0001 and is
		 * silently dropped for any other address \u2014 do NOT use it. */
		if (tag_count > 0) {
			(void)uart_sensor_esl_get_name(0xFFFF);
		}
		break;

	default:
		break;
	}
}

/* ===========================================================================
 * ESL Periodic Polling — polls NUS status for all known tags
 * =========================================================================== */

static void esl_poll_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/* ---- Check if previous poll got a NUS response ---- */
	if (last_poll_send_time > 0 && last_nus_response_time < last_poll_send_time) {
		nus_consecutive_poll_failures++;
		LOG_WRN("NUS poll: no response since last poll (failure %d/%d)",
			nus_consecutive_poll_failures, nus_max_poll_failures_rt);

		if (nus_consecutive_poll_failures >= nus_max_poll_failures_rt) {
			LOG_WRN("NUS: %d consecutive failures — marking tags disconnected and rescanning",
				nus_max_poll_failures_rt);
			/* Mark all configured tags as disconnected */
			for (int mi = 0; mi < tag_count; mi++) {
				tag_db[mi].connected = false;
			}
			nus_consecutive_poll_failures = 0;
			k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work, K_NO_WAIT);
			goto reschedule;
		}
	} else if (last_poll_send_time > 0) {
		/* Previous poll succeeded */
		if (nus_consecutive_poll_failures > 0) {
			LOG_INF("NUS poll: response received, resetting failure counter");
		}
		nus_consecutive_poll_failures = 0;
	}

	LOG_INF("ESL poll: querying %d known tag(s)", tag_count);

	if (tag_count == 0) {
		/* No tags yet — ensure scan retry work is running */
		if (!scan_active && !k_work_delayable_is_pending(&scan_retry_work)) {
			LOG_INF("ESL poll: no tags yet, triggering scan retry");
			k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work, K_NO_WAIT);
		}
	}

	/* Record poll time before sending commands */
	last_poll_send_time = k_uptime_get();

	/* Health-probe each known tag with a single NUS status request.
	 *
	 * NOTE: The gateway auto-emits "#SENSOR_REPORT:TEMP/BATT,0x<addr>,<v>"
	 * on its own schedule once any tag is synced \u2014 we DO NOT request
	 * "nus sensors" here because that would race the auto-poll and the
	 * gateway returns -16 (EBUSY). One "nus status" per tag is enough
	 * to refresh uptime/flags and confirm the tag is alive.
	 *
	 * Address must be the real ESL address (hex) \u2014 NOT the array index.
	 * The gateway treats decimal/zero as broadcast and silently drops
	 * the request.
	 */
	for (int i = 0; i < tag_count; i++) {
		if (!tag_db[i].valid || tag_db[i].esl_addr == 0xFFFF) {
			continue;
		}
		uart_sensor_esl_nus_status(tag_db[i].esl_addr);
		/* Per gateway protocol: \u22652 s between successive NUS cmds */
		k_sleep(K_SECONDS(2));
	}

reschedule:
	if (esl_poll_active) {
		k_work_reschedule_for_queue(&spi_work_q, &esl_poll_work,
					    K_SECONDS(esl_poll_interval_sec));
	}
}

int uart_sensor_esl_poll_start(void)
{
	esl_poll_active = true;
	k_work_reschedule_for_queue(&spi_work_q, &esl_poll_work,
				    K_SECONDS(esl_poll_interval_sec));
	LOG_INF("ESL polling started (interval: %ds)", esl_poll_interval_sec);
	return 0;
}

int uart_sensor_esl_poll_stop(void)
{
	esl_poll_active = false;
	k_work_cancel_delayable(&esl_poll_work);
	LOG_INF("ESL polling stopped");
	return 0;
}

/* ===========================================================================
 * Smart Scan Discovery — helpers and retry work
 * =========================================================================== */

/** Count tags currently marked as BLE-connected. */
static int esl_count_connected(void)
{
	int n = 0;

	for (int i = 0; i < tag_count; i++) {
		if (tag_db[i].valid && tag_db[i].connected) {
			n++;
		}
	}
	return n;
}

/**
 * Check whether all expected tags are connected.
 * - If yes: cancel any pending scan retry.
 * - If no and no scan is running: schedule a retry.
 */
static void esl_discovery_check(void)
{
	int conn = esl_count_connected();

	if (expected_tag_count <= 0) {
		return; /* mechanism disabled */
	}

	if (conn >= expected_tag_count) {
		LOG_INF("Discovery: all %d/%d expected tags connected — cancelling scan retry",
			conn, expected_tag_count);
		(void)k_work_cancel_delayable(&scan_retry_work);
	} else if (!scan_active && !k_work_delayable_is_pending(&scan_retry_work)) {
		LOG_INF("Discovery: %d/%d tags connected — scheduling scan retry in %d s",
			conn, expected_tag_count,
			scan_retry_interval_sec);
		k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work,
					    K_SECONDS(scan_retry_interval_sec));
	}
}

/** Retry work: fires when tags are missing; starts a new scan if needed. */
static void scan_retry_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int conn = esl_count_connected();

	if (conn >= expected_tag_count) {
		LOG_INF("Scan retry: all %d expected tags now connected — stopping retries",
			expected_tag_count);
		return;
	}

	if (gateway_provisioned == 1) {
		/* Whitelist sealed — never on-demand scan. Tags will auto-resync
		 * on PAwR by themselves. */
		LOG_DBG("Scan retry suppressed: gateway provisioned");
		return;
	}

	if (scan_active) {
		/* nRF5340 scan still running — wait for #SCAN:Stop */
		LOG_INF("Scan retry: scan in progress — waiting for #SCAN:Stop");
		return;
	}

	int64_t now = k_uptime_get();
	int64_t since = now - last_scan_request_ms;

	if (last_scan_request_ms != 0 && since < ESL_SCAN_MIN_INTERVAL_MS) {
		int64_t wait_ms = ESL_SCAN_MIN_INTERVAL_MS - since;

		LOG_INF("Scan retry: backing off %lld ms (>=5 min between scans)",
			wait_ms);
		k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work,
					    K_MSEC(wait_ms));
		return;
	}

	LOG_INF("Scan retry: %d/%d tags connected — starting scan",
		conn, expected_tag_count);
	last_scan_request_ms = now;
	uart_sensor_esl_scan_start();
	/* #SCAN:Stop will call esl_discovery_check() to schedule the next retry if needed.
	 * Schedule a fallback in case #SCAN:Stop is never received from nRF5340. */
	k_work_reschedule_for_queue(&spi_work_q, &scan_retry_work,
				    K_SECONDS(scan_retry_interval_sec + 35));
}

int uart_sensor_esl_set_expected_tags(int n)
{
	if (n < 0 || n > ESL_MAX_TAGS) {
		return -EINVAL;
	}
	LOG_INF("Expected tag count: %d → %d", expected_tag_count, n);
	expected_tag_count = n;
	if (uart_module_state.initialized) {
		esl_discovery_check();
	}
	return 0;
}

/** NUS settle work — fires 200 ms after the last sub-response to publish once. */
static void nus_settle_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	struct uart_sensor_msg snap = current_sensor_data;
	k_mutex_unlock(&uart_data_mutex);
	if (uart_module_state.auto_publish_enabled) {
		zbus_chan_pub(&UART_SENSOR_CHAN, &snap, K_MSEC(UART_ZBUS_TIMEOUT_MS));
	}
}

/** Set ESL NUS polling interval in seconds; takes effect on next reschedule. */
int uart_sensor_esl_set_poll_interval(int s)
{
	if (s < 10 || s > 86400) {
		return -EINVAL;
	}
	LOG_INF("ESL poll interval: %d → %d s", esl_poll_interval_sec, s);
	esl_poll_interval_sec = s;
	if (esl_poll_active) {
		k_work_reschedule_for_queue(&spi_work_q, &esl_poll_work, K_SECONDS(s));
	}
	return 0;
}

/** Get current ESL NUS polling interval. */
int uart_sensor_esl_get_poll_interval(void)
{
	return esl_poll_interval_sec;
}

/** Set scan retry interval when tags are missing. */
int uart_sensor_esl_set_scan_retry_interval(int s)
{
	if (s < 5 || s > 86400) {
		return -EINVAL;
	}
	LOG_INF("Scan retry interval: %d → %d s", scan_retry_interval_sec, s);
	scan_retry_interval_sec = s;
	return 0;
}

/** Get current scan retry interval. */
int uart_sensor_esl_get_scan_retry_interval(void)
{
	return scan_retry_interval_sec;
}

/** Set consecutive NUS failures threshold before triggering rescan. */
int uart_sensor_esl_set_nus_failures(int n)
{
	if (n < 1 || n > 20) {
		return -EINVAL;
	}
	LOG_INF("NUS max failures: %d → %d", nus_max_poll_failures_rt, n);
	nus_max_poll_failures_rt = n;
	return 0;
}

/** Get current NUS max failures threshold. */
int uart_sensor_esl_get_nus_failures(void)
{
	return nus_max_poll_failures_rt;
}

/* ===========================================================================
 * Processing Thread — dequeues lines from ISR and dispatches to parser
 * =========================================================================== */

static void uart_processing_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	char rx_buf[UART_RX_BUF_SIZE];

	LOG_INF("UART processing thread started");

	while (1) {
		k_sem_take(&uart_wake_sem, K_MSEC(UART_PROCESSING_TIMEOUT_MS));

		while (k_msgq_get(&uart_msgq, &rx_buf, K_NO_WAIT) == 0) {
			LOG_INF("<<< UART RX: %s", rx_buf);
			uart_sensor_process_data_line(rx_buf);
		}
	}
}

/* ===========================================================================
 * Standard API — kept compatible with rest of the app
 * =========================================================================== */

static int uart_device_setup(void)
{
	int ret;

	if (!device_is_ready(spi_dev)) {
		LOG_ERR("SPI3 device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&esl_cs_gpio)) {
		LOG_ERR("SPI CS GPIO (P0.05) not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&esl_drdy_gpio)) {
		LOG_ERR("DATA_READY GPIO (P0.04) not ready");
		return -ENODEV;
	}

	/* CS: output, initially deasserted (GPIO_OUTPUT_INACTIVE = HIGH for ACTIVE_LOW) */
	ret = gpio_pin_configure_dt(&esl_cs_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("CS GPIO configure error: %d", ret);
		return ret;
	}

	/* DATA_READY: input, interrupt on rising edge */
	ret = gpio_pin_configure_dt(&esl_drdy_gpio, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("DATA_READY GPIO configure error: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&esl_drdy_gpio, GPIO_INT_EDGE_RISING);
	if (ret != 0) {
		LOG_ERR("DATA_READY interrupt configure error: %d", ret);
		return ret;
	}

	gpio_init_callback(&drdy_gpio_cb, drdy_isr, BIT(esl_drdy_gpio.pin));
	ret = gpio_add_callback(esl_drdy_gpio.port, &drdy_gpio_cb);
	if (ret != 0) {
		LOG_ERR("DATA_READY add callback error: %d", ret);
		return ret;
	}

	k_work_init(&spi_rx_work, spi_rx_work_fn);

	/* Start the dedicated SPI work queue */
	k_work_queue_start(&spi_work_q, spi_wq_stack,
			   K_THREAD_STACK_SIZEOF(spi_wq_stack),
			   SPI_WQ_PRIORITY, NULL);
	k_thread_name_set(&spi_work_q.thread, "spi_wq");

	/* Handle case where DATA_READY is already HIGH at startup */
	if (gpio_pin_get_dt(&esl_drdy_gpio) > 0) {
		LOG_INF("DATA_READY already HIGH at init — draining pending frames");
		k_work_submit_to_queue(&spi_work_q, &spi_rx_work);
	}

	LOG_INF("SPI bridge initialized: SPI3 @ 4 MHz, CS=P0.05, DRDY=P0.04");
	return 0;
}

#ifdef CONFIG_APP_UART_SENSOR_ERROR_RECOVERY
static int uart_device_recovery(void)
{
	/* Disable DATA_READY interrupt, reset counters, re-init */
	gpio_pin_interrupt_configure_dt(&esl_drdy_gpio, GPIO_INT_DISABLE);
	uart_module_state.consecutive_errors = 0;
	return uart_device_setup();
}
#endif

static void uart_error_handler(enum uart_sensor_error_type error_type, const char *details)
{
	uart_module_state.consecutive_errors++;
	uart_module_state.stats.last_error_time = k_uptime_get();
	LOG_ERR("UART error (type %d): %s", error_type, details ? details : "");

	switch (error_type) {
	case UART_SENSOR_ERROR_PARSE_FAILED:  uart_module_state.stats.parse_errors++; break;
	case UART_SENSOR_ERROR_INVALID_DATA:  uart_module_state.stats.validation_errors++; break;
	case UART_SENSOR_ERROR_COMM_TIMEOUT:  uart_module_state.stats.comm_errors++; break;
	default: break;
	}

#ifdef CONFIG_APP_UART_SENSOR_ERROR_RECOVERY
	if (uart_module_state.consecutive_errors >= UART_MAX_CONSECUTIVE_ERRORS) {
		int64_t now = k_uptime_get();
		if (now - uart_module_state.last_recovery_time > UART_RECOVERY_DELAY_MS) {
			uart_device_recovery();
			uart_module_state.last_recovery_time = now;
		}
	}
#endif
}

static void publish_error_message(enum uart_sensor_error_type error_type, const char *details)
{
	struct uart_sensor_msg msg = {
		.type = UART_SENSOR_ERROR_RESPONSE,
		.error_type = error_type,
		.timestamp = k_uptime_get(),
	};
	if (details) {
		strncpy(msg.error_details, details, sizeof(msg.error_details) - 1);
	}
	zbus_chan_pub(&UART_SENSOR_CHAN, &msg, K_NO_WAIT);
}

static void update_statistics(bool parse_ok, bool valid_ok, bool pub_ok)
{
	uart_module_state.stats.messages_received++;
	if (parse_ok) {
		uart_module_state.stats.messages_parsed++;
		uart_module_state.consecutive_errors = 0;
	}
	if (!valid_ok) uart_module_state.stats.validation_errors++;
	if (!pub_ok)   uart_module_state.stats.publish_errors++;
}

bool uart_sensor_validate_data(const struct uart_sensor_msg *msg)
{
	if (!msg) return false;
#ifdef CONFIG_APP_UART_SENSOR_DATA_VALIDATION
	if (msg->temperature < UART_TEMP_MIN || msg->temperature > UART_TEMP_MAX) return false;
	if (msg->humidity < UART_HUMIDITY_MIN || msg->humidity > UART_HUMIDITY_MAX) return false;
	if (msg->probe_battery < 0.0f || msg->probe_battery > 100.0f) return false;
#endif
	return true;
}

int uart_sensor_init(void)
{
	if (uart_module_state.initialized) return 0;

	int err = uart_device_setup();
	if (err) return err;

	uart_module_state.initialized = true;
	uart_module_state.auto_publish_enabled = CONFIG_APP_UART_SENSOR_AUTO_PUBLISH;
	return 0;
}

int uart_sensor_sample_request(void)
{
	if (!uart_module_state.initialized) return -ENODEV;

	/* Trigger an immediate NUS status poll for all known tags */
	if (esl_ap_ready && tag_count > 0) {
		for (int i = 0; i < tag_count; i++) {
			if (tag_db[i].valid && tag_db[i].esl_addr != 0xFFFF) {
				/* Use real ESL hex address \u2014 not the array index */
				uart_sensor_esl_nus_status(tag_db[i].esl_addr);
				k_sleep(K_SECONDS(2));
			}
		}
	}

	/* Also publish whatever we have cached */
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	current_sensor_data.timestamp = k_uptime_get();
	struct uart_sensor_msg to_pub = current_sensor_data;
	k_mutex_unlock(&uart_data_mutex);

	return zbus_chan_pub(&UART_SENSOR_CHAN, &to_pub, K_NO_WAIT);
}

int uart_sensor_get_current_data(struct uart_sensor_msg *data)
{
	if (!data) return -EINVAL;
	k_mutex_lock(&uart_data_mutex, K_FOREVER);
	memcpy(data, &current_sensor_data, sizeof(*data));
	k_mutex_unlock(&uart_data_mutex);
	return 0;
}

int uart_sensor_check_status(void)
{
	LOG_INF("UART status: init=%d, ap_ready=%d, tags=%d, poll=%s, msgs=%u",
		uart_module_state.initialized, esl_ap_ready, tag_count,
		esl_poll_active ? "on" : "off",
		uart_module_state.stats.messages_parsed);
	return 0;
}

int uart_sensor_get_stats(struct uart_sensor_stats *stats)
{
	if (!stats) return -EINVAL;
	memcpy(stats, &uart_module_state.stats, sizeof(*stats));
	stats->uptime_ms = k_uptime_get() - uart_module_state.stats.uptime_ms;
	return 0;
}

int uart_sensor_reset_stats(void)
{
	memset(&uart_module_state.stats, 0, sizeof(uart_module_state.stats));
	uart_module_state.stats.uptime_ms = k_uptime_get();
	uart_module_state.consecutive_errors = 0;
	return 0;
}

int uart_sensor_set_auto_publish(bool enable)
{
	uart_module_state.auto_publish_enabled = enable;
	return 0;
}

/* ===========================================================================
 * Module Init (SYS_INIT)
 * =========================================================================== */

/* Tag name persistence — "app/tag/<hex4>/name" ─────────────────────────────
 * Written on first #SENSOR_NAME receipt; restored at boot via
 * settings_load_subtree("app/tag") called below.
 */
static int app_tag_settings_set(const char *key, size_t len,
				  settings_read_cb read_cb, void *cb_arg)
{
	unsigned int addr_u = 0;

	if (sscanf(key, "%4x/name", &addr_u) == 1) {
		char name[13] = {0};
		ssize_t n = read_cb(cb_arg, name, sizeof(name) - 1);

		if (n > 0) {
			name[n] = '\0';
			int db_idx = esl_tag_db_find_or_add((uint16_t)addr_u);

			if (db_idx >= 0) {
				strncpy(tag_db[db_idx].name, name,
					sizeof(tag_db[db_idx].name) - 1);
				tag_db[db_idx].name[sizeof(tag_db[db_idx].name) - 1] = '\0';
				LOG_INF("Restored name 0x%04X: '%s'",
					(uint16_t)addr_u, name);
			}
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_tag_stg, "app/tag", NULL,
				app_tag_settings_set, NULL, NULL);

static int uart_sensor_module_init(void)
{
	int err;

	LOG_INF("ESL AP sensor module init — build: " __DATE__ " " __TIME__);

	if (!device_is_ready(spi_dev)) {
		LOG_ERR("SPI3 device not ready");
		return -ENODEV;
	}

	err = uart_sensor_init();
	if (err) {
		LOG_ERR("SPI sensor init failed: %d", err);
		return err;
	}

	/* Initialize ESL-specific state */
	memset(tag_db, 0, sizeof(tag_db));
	tag_count = 0;
	/* Restore tag names persisted from previous boot */
	settings_load_subtree("app/tag");
	esl_ap_ready = false;
	esl_poll_active = false;
	esl_startup_step = 0;
	ap_time_sent_this_boot = false;
	lte_time_available = false;

	/* Register for LTE clock sync events so we can push time to AP */
	date_time_register_handler(date_time_event_handler);

	k_work_init_delayable(&esl_poll_work, esl_poll_work_fn);
	k_work_init_delayable(&esl_startup_work, esl_startup_work_fn);
	k_work_init_delayable(&scan_retry_work, scan_retry_work_fn);
	k_work_init_delayable(&nus_settle_work, nus_settle_work_fn);

	memset(&current_sensor_data, 0, sizeof(current_sensor_data));
	current_sensor_data.type = UART_SENSOR_DATA_RESPONSE;
	current_sensor_data.timestamp = k_uptime_get();

	memset(&uart_module_state.stats, 0, sizeof(uart_module_state.stats));
	uart_module_state.stats.uptime_ms = k_uptime_get();

	/* Start processing thread */
	uart_thread_tid = k_thread_create(&uart_thread_data, uart_thread_stack,
					  K_THREAD_STACK_SIZEOF(uart_thread_stack),
					  uart_processing_thread, NULL, NULL, NULL,
					  UART_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(uart_thread_tid, "uart_esl");

	uart_module_state.initialized = true;

	/*
	 * nRF91 is SPI master — start the startup sequence unconditionally.
	 * We do NOT wait for a prompt from nRF5340 (spi1_manager never sends one).
	 * Delay 3 s to give nRF5340 time to finish booting and arm SPIS1.
	 */
	esl_ap_ready = true;
	k_work_reschedule_for_queue(&spi_work_q, &esl_startup_work, K_SECONDS(3));

	LOG_INF("ESL AP sensor module ready — starting nRF5340 config in 3 s");
	return 0;
}

/* ===========================================================================
 * ZBUS listener — forwards MQTT commands to UART
 * =========================================================================== */

static void uart_sensor_zbus_callback(const struct zbus_channel *chan)
{
	const struct custom_mqtt_msg *msg;

	if (&CUSTOM_MQTT_CHAN == chan) {
		msg = zbus_chan_const_msg(chan);

		if (msg->type == CUSTOM_MQTT_EVT_FORWARD_UART) {
			if (msg->forward_uart.data) {
				LOG_INF("MQTT→UART: %s", msg->forward_uart.data);
				uart_sensor_send_command(msg->forward_uart.data);
			}
		}
	}
}

ZBUS_LISTENER_DEFINE(uart_sensor_listener, uart_sensor_zbus_callback);
ZBUS_CHAN_ADD_OBS(CUSTOM_MQTT_CHAN, uart_sensor_listener, 0);

SYS_INIT(uart_sensor_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
