/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * FOTA module — HTTP/HTTPS firmware update triggered via MQTT command.
 * Does NOT use nRF Cloud. Uses fota_download library + MCUboot DFU target.
 *
 * Usage:
 *   Send MQTT: {"command":"fota_start","url":"https://t4as.org/thingyupdate"}
 *   Optional:  add "sec_tag":<n> to specify a TLS credential tag.
 *
 * TLS notes:
 *   For HTTPS, provision the CA cert of the firmware server into modem
 *   credential storage at the chosen sec_tag using AT%CMNG.
 *   Default sec_tag is CONFIG_APP_FOTA_SEC_TAG (default -1 = no TLS cert check).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>
#include <zephyr/sys/reboot.h>
#include <net/fota_download.h>
#include <string.h>

#include "app_common.h"
#include "fota.h"

LOG_MODULE_REGISTER(fota, CONFIG_APP_FOTA_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_FOTA_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_FOTA_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

ZBUS_MSG_SUBSCRIBER_DEFINE(fota);

ZBUS_CHAN_DEFINE(FOTA_CHAN,
		 enum fota_msg_type,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_ADD_OBS(FOTA_CHAN, fota, 0);

#define MAX_MSG_SIZE sizeof(enum fota_msg_type)

/* URL and sec_tag set by fota_http_trigger() */
static char fota_pending_url[CONFIG_APP_FOTA_URL_MAX_LEN];
static int  fota_pending_sec_tag = CONFIG_APP_FOTA_SEC_TAG;

/* Download progress (0–100), updated from fota_download callback */
static atomic_t fota_progress = ATOMIC_INIT(-1);

/* ─── State machine ─────────────────────────────────────────────────────── */

enum fota_module_state {
	STATE_RUNNING,
		STATE_IDLE,
		STATE_DOWNLOADING,
		STATE_REBOOT_PENDING,
		STATE_CANCELING,
};

struct fota_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
};

static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_idle_entry(void *obj);
static void state_idle_run(void *obj);
static void state_downloading_entry(void *obj);
static void state_downloading_run(void *obj);
static void state_reboot_pending_entry(void *obj);
static void state_canceling_entry(void *obj);
static void state_canceling_run(void *obj);

static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, &states[STATE_IDLE]),
	[STATE_IDLE] =
		SMF_CREATE_STATE(state_idle_entry, state_idle_run, NULL,
				 &states[STATE_RUNNING], NULL),
	[STATE_DOWNLOADING] =
		SMF_CREATE_STATE(state_downloading_entry, state_downloading_run, NULL,
				 &states[STATE_RUNNING], NULL),
	[STATE_REBOOT_PENDING] =
		SMF_CREATE_STATE(state_reboot_pending_entry, NULL, NULL,
				 &states[STATE_RUNNING], NULL),
	[STATE_CANCELING] =
		SMF_CREATE_STATE(state_canceling_entry, state_canceling_run, NULL,
				 &states[STATE_RUNNING], NULL),
};

/* ─── URL parser ─────────────────────────────────────────────────────────── */

static void parse_url(const char *url,
		      char *host_buf, size_t host_sz,
		      char *file_buf, size_t file_sz)
{
	/* Find start of host portion (after "://") */
	const char *scheme_end = strstr(url, "://");
	const char *host_start = scheme_end ? scheme_end + 3 : url;

	/* Find first '/' after the host */
	const char *slash = strchr(host_start, '/');

	if (slash) {
		/*
		 * host_buf = scheme + host, e.g. "https://t4as.org"
		 * The downloader constructs the final URL as snprintf("%s/%s", host, file),
		 * so host must include the scheme to avoid the "Protocol not specified" warning
		 * and to select the right port automatically.
		 */
		size_t hlen = MIN((size_t)(slash - url), host_sz - 1);

		strncpy(host_buf, url, hlen);
		host_buf[hlen] = '\0';
		/* file = path without leading '/': "thingyupdate" (not "/thingyupdate")
		 * because the downloader adds its own '/' separator.
		 */
		strncpy(file_buf, slash + 1, file_sz - 1);
		file_buf[file_sz - 1] = '\0';
	} else {
		/* No path — use whole URL as host */
		strncpy(host_buf, url, host_sz - 1);
		host_buf[host_sz - 1] = '\0';
		file_buf[0] = '\0';
	}
}

/* ─── fota_download callback (downloader thread context) ─────────────────── */

static void fota_dl_callback(const struct fota_download_evt *evt)
{
	enum fota_msg_type msg;

	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_PROGRESS:
		atomic_set(&fota_progress, evt->progress);
		LOG_INF("FOTA download: %d%%", evt->progress);
		return;
	case FOTA_DOWNLOAD_EVT_ERASE_PENDING:
		LOG_INF("FOTA: erasing secondary flash slot...");
		return;
	case FOTA_DOWNLOAD_EVT_ERASE_DONE:
		LOG_INF("FOTA: flash erase done");
		return;
	case FOTA_DOWNLOAD_EVT_ERASE_TIMEOUT:
		LOG_WRN("FOTA: flash erase timed out, continuing");
		return;
	case FOTA_DOWNLOAD_EVT_FINISHED:
		LOG_INF("FOTA: download complete — reboot to apply");
		atomic_set(&fota_progress, 100);
		msg = FOTA_SUCCESS_REBOOT_NEEDED;
		break;
	case FOTA_DOWNLOAD_EVT_ERROR:
		LOG_ERR("FOTA: download error, cause=%d", (int)evt->cause);
		atomic_set(&fota_progress, -1);
		msg = FOTA_DOWNLOAD_FAILED;
		break;
	case FOTA_DOWNLOAD_EVT_CANCELLED:
		LOG_INF("FOTA: download cancelled");
		atomic_set(&fota_progress, -1);
		msg = FOTA_DOWNLOAD_CANCELED;
		break;
	default:
		return;
	}

	int err = zbus_chan_pub(&FOTA_CHAN, &msg, K_MSEC(500));

	if (err) {
		LOG_ERR("zbus_chan_pub FOTA_CHAN: %d", err);
	}
}

/* ─── State handlers ──────────────────────────────────────────────────────── */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_INF("FOTA module started (HTTP/HTTPS mode, no nRF Cloud)");

	/* Confirm the running image so MCUboot won't roll it back */
	if (!boot_is_img_confirmed()) {
		int err = boot_write_img_confirmed();

		if (err) {
			LOG_WRN("boot_write_img_confirmed: %d", err);
		} else {
			LOG_INF("MCUboot image confirmed");
		}
	}

	int err = fota_download_init(fota_dl_callback);

	if (err) {
		LOG_ERR("fota_download_init: %d", err);
	}
}

static void state_running_run(void *obj)
{
	struct fota_state_object const *s = obj;

	if (s->chan == &FOTA_CHAN &&
	    MSG_TO_FOTA_TYPE(s->msg_buf) == FOTA_DOWNLOAD_CANCEL) {
		smf_set_state(SMF_CTX(s), &states[STATE_CANCELING]);
	}
}

static void state_idle_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_DBG("FOTA: idle — send {\"command\":\"fota_start\",\"url\":\"...\"}");
}

static void state_idle_run(void *obj)
{
	struct fota_state_object const *s = obj;

	if (s->chan != &FOTA_CHAN) {
		return;
	}

	enum fota_msg_type msg = MSG_TO_FOTA_TYPE(s->msg_buf);

	if (msg == FOTA_POLL_REQUEST) {
		if (strlen(fota_pending_url) == 0) {
			LOG_WRN("FOTA: ignoring FOTA_POLL_REQUEST with empty URL");
			smf_set_handled(SMF_CTX(s));
			return;
		}
		smf_set_state(SMF_CTX(s), &states[STATE_DOWNLOADING]);
	} else if (msg == FOTA_DOWNLOAD_CANCEL) {
		LOG_DBG("FOTA: no active download to cancel");
		smf_set_handled(SMF_CTX(s));
	}
}

static void state_downloading_entry(void *obj)
{
	char host[128];
	char file[256];

	LOG_INF("Starting FOTA: url=%s sec_tag=%d",
		fota_pending_url, fota_pending_sec_tag);

	parse_url(fota_pending_url, host, sizeof(host), file, sizeof(file));
	LOG_DBG("FOTA parsed: host=%s file=%s", host, file);

	atomic_set(&fota_progress, 0);

	int err = fota_download_start(host, file, fota_pending_sec_tag, 0, 0);

	if (err) {
		LOG_ERR("fota_download_start: %d", err);
		enum fota_msg_type fail = FOTA_DOWNLOAD_FAILED;

		zbus_chan_pub(&FOTA_CHAN, &fail, K_MSEC(500));
		return;
	}

	/* Notify main.c so it can show the LED and cancel the polling timer */
	enum fota_msg_type notify = FOTA_DOWNLOADING_UPDATE;

	err = zbus_chan_pub(&FOTA_CHAN, &notify, K_MSEC(500));
	if (err) {
		LOG_WRN("Failed to notify main of download start: %d", err);
	}
}

static void state_downloading_run(void *obj)
{
	struct fota_state_object const *s = obj;

	if (s->chan != &FOTA_CHAN) {
		return;
	}

	switch (MSG_TO_FOTA_TYPE(s->msg_buf)) {
	case FOTA_SUCCESS_REBOOT_NEEDED:
		smf_set_state(SMF_CTX(s), &states[STATE_REBOOT_PENDING]);
		break;
	case FOTA_DOWNLOAD_FAILED:
	case FOTA_DOWNLOAD_TIMED_OUT:
	case FOTA_DOWNLOAD_CANCELED:
		LOG_WRN("FOTA download did not complete — back to idle");
		smf_set_state(SMF_CTX(s), &states[STATE_IDLE]);
		break;
	case FOTA_DOWNLOADING_UPDATE:
		/* Our own start notification — already in downloading, ignore */
		smf_set_handled(SMF_CTX(s));
		break;
	default:
		break;
	}
}

static void state_reboot_pending_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_INF("FOTA image ready — system will reboot to apply update");
	/* main.c handles the reboot sequence on FOTA_SUCCESS_REBOOT_NEEDED */
}

static void state_canceling_entry(void *obj)
{
	ARG_UNUSED(obj);
	LOG_INF("Canceling FOTA download");

	int err = fota_download_cancel();

	if (err) {
		LOG_WRN("fota_download_cancel: %d", err);
	}
}

static void state_canceling_run(void *obj)
{
	struct fota_state_object const *s = obj;

	if (s->chan == &FOTA_CHAN &&
	    MSG_TO_FOTA_TYPE(s->msg_buf) == FOTA_DOWNLOAD_CANCELED) {
		smf_set_state(SMF_CTX(s), &states[STATE_IDLE]);
	}
}

/* ─── WDT callback ────────────────────────────────────────────────────────── */

static void fota_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

int fota_http_trigger(const char *url, int sec_tag)
{
	if (!url || strlen(url) == 0) {
		return -EINVAL;
	}
	if (strlen(url) >= CONFIG_APP_FOTA_URL_MAX_LEN) {
		LOG_ERR("FOTA URL too long (%zu >= %d)",
			strlen(url), CONFIG_APP_FOTA_URL_MAX_LEN);
		return -ENOMEM;
	}

	strncpy(fota_pending_url, url, sizeof(fota_pending_url) - 1);
	fota_pending_url[sizeof(fota_pending_url) - 1] = '\0';
	fota_pending_sec_tag = sec_tag;

	enum fota_msg_type msg = FOTA_POLL_REQUEST;

	return zbus_chan_pub(&FOTA_CHAN, &msg, K_MSEC(500));
}

int fota_get_progress(void)
{
	return (int)atomic_get(&fota_progress);
}

/* ─── Module thread ───────────────────────────────────────────────────────── */

static void fota_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(uint32_t)(CONFIG_APP_FOTA_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(uint32_t)(CONFIG_APP_FOTA_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct fota_state_object fota_state = { 0 };

	LOG_DBG("FOTA module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, fota_wdt_callback,
				   (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&fota_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&fota, &fota_state.chan,
					fota_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&fota_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(fota_module_thread_id,
		CONFIG_APP_FOTA_THREAD_STACK_SIZE,
		fota_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

