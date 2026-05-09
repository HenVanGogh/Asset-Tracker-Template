/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Unit tests for the FOTA module.
 *
 * fota.c uses:
 *  - fota_download_init(cb)   — register download callback
 *  - fota_download_start(...) — start HTTP download
 *  - fota_download_cancel()   — cancel in-progress download
 *  - boot_is_img_confirmed()  — check MCUboot image confirmation
 *  - boot_write_img_confirmed() — confirm current image
 *
 * The test invokes fota_http_trigger() to set the URL and kick a
 * FOTA_POLL_REQUEST on the channel, then simulates fota_download events
 * by calling the callback that was captured from fota_download_init.
 */

#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <net/fota_download.h>
#include <zephyr/dfu/mcuboot.h>

#include "app_common.h"
#include "fota.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, fota_download_cancel);
FAKE_VALUE_FUNC(int, fota_download_init, fota_download_callback_t);
FAKE_VALUE_FUNC(int, fota_download_start, const char *, const char *, int, uint8_t, size_t);
FAKE_VALUE_FUNC(bool, boot_is_img_confirmed);
FAKE_VALUE_FUNC(int, boot_write_img_confirmed);

ZBUS_MSG_SUBSCRIBER_DEFINE(fota_subscriber);
ZBUS_CHAN_ADD_OBS(FOTA_CHAN, fota_subscriber, 0);

LOG_MODULE_REGISTER(fota_module_test, 4);

/* The callback registered by fota_download_init -- we capture it here. */
static fota_download_callback_t captured_dl_cb;

static int fota_download_init_capture(fota_download_callback_t cb)
{
	captured_dl_cb = cb;
	return 0;
}

/* Forward declarations */
static void event_expect(enum fota_msg_type expected_fota_type);
static void event_send(enum fota_msg_type msg);
static void simulate_dl_evt(enum fota_download_evt_id id, int cause, int progress);
void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(fota_download_cancel);
	RESET_FAKE(fota_download_init);
	RESET_FAKE(fota_download_start);
	RESET_FAKE(boot_is_img_confirmed);
	RESET_FAKE(boot_write_img_confirmed);

	FFF_RESET_HISTORY();

	/* Default: image already confirmed, download init succeeds, captures cb */
	boot_is_img_confirmed_fake.return_val = true;
	fota_download_init_fake.custom_fake = fota_download_init_capture;

	/* Drain any leftover events from previous test */
	const struct zbus_channel *chan;
	enum fota_msg_type fota_msg;

	while (zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_MSEC(10)) == 0) {
		/* discard */
	}
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	enum fota_msg_type drain_msg;

	if (!captured_dl_cb) {
		return;
	}

	/* Send FOTA_DOWNLOAD_CANCEL to transition any active state to STATE_CANCELING.
	 * From STATE_IDLE: state_idle_run handles it with smf_set_handled (stays in IDLE).
	 * From STATE_DOWNLOADING/REBOOT_PENDING: parent state_running_run transitions to CANCELING.
	 */
	drain_msg = FOTA_DOWNLOAD_CANCEL;
	zbus_chan_pub(&FOTA_CHAN, &drain_msg, K_MSEC(100));
	k_sleep(K_MSEC(30));

	/* Simulate FOTA_DOWNLOAD_EVT_CANCELLED to complete the cancel sequence and
	 * return to STATE_IDLE. Also resets fota_progress to -1. */
	struct fota_download_evt evt = { .id = FOTA_DOWNLOAD_EVT_CANCELLED };

	captured_dl_cb(&evt);
	k_sleep(K_MSEC(30));

	/* Drain any leftover subscriber messages */
	while (zbus_sub_wait_msg(&fota_subscriber, &chan, &drain_msg, K_MSEC(10)) == 0) {
		/* discard */
	}
}

static void event_expect(enum fota_msg_type expected_fota_type)
{
	int err;
	const struct zbus_channel *chan;
	enum fota_msg_type fota_msg;

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_MSEC(2000));
	if (err == -ENOMSG) {
		LOG_ERR("Timeout waiting for FOTA event %d", expected_fota_type);
		TEST_FAIL();
		return;
	}
	if (err) {
		LOG_ERR("zbus_sub_wait_msg error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
	if (chan != &FOTA_CHAN) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}
	TEST_ASSERT_EQUAL(expected_fota_type, fota_msg);
}

static void event_send(enum fota_msg_type msg)
{
	int err = zbus_chan_pub(&FOTA_CHAN, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static void simulate_dl_evt(enum fota_download_evt_id id, int cause, int progress)
{
	if (!captured_dl_cb) {
		LOG_ERR("No callback captured -- fota_download_init not called yet");
		TEST_FAIL();
		return;
	}

	struct fota_download_evt evt = {
		.id = id,
	};

	if (id == FOTA_DOWNLOAD_EVT_ERROR) {
		evt.cause = cause;
	} else if (id == FOTA_DOWNLOAD_EVT_PROGRESS) {
		evt.progress = progress;
	}

	captured_dl_cb(&evt);

	/* Give the FOTA module thread a chance to process the event */
	k_sleep(K_MSEC(50));
}

/* Test: module calls fota_download_init at startup */
void test_fota_module_calls_download_init_at_startup(void)
{
	k_sleep(K_MSEC(200));
	TEST_ASSERT_GREATER_OR_EQUAL(1, fota_download_init_fake.call_count);
}

/* Test: trigger download -> FOTA_DOWNLOADING_UPDATE published */
void test_fota_trigger_starts_download(void)
{
	fota_download_start_fake.return_val = 0;
	k_sleep(K_MSEC(100));

	int err = fota_http_trigger("http://example.com/firmware.bin", -1);

	TEST_ASSERT_EQUAL(0, err);
	event_expect(FOTA_POLL_REQUEST);
	event_expect(FOTA_DOWNLOADING_UPDATE);

	TEST_ASSERT_EQUAL(1, fota_download_start_fake.call_count);
}

/* Test: download finishes -> FOTA_SUCCESS_REBOOT_NEEDED */
void test_fota_download_success(void)
{
	fota_download_start_fake.return_val = 0;
	k_sleep(K_MSEC(100));

	fota_http_trigger("http://example.com/firmware.bin", -1);
	event_expect(FOTA_POLL_REQUEST);
	event_expect(FOTA_DOWNLOADING_UPDATE);

	simulate_dl_evt(FOTA_DOWNLOAD_EVT_FINISHED, 0, 0);
	event_expect(FOTA_SUCCESS_REBOOT_NEEDED);
}

/* Test: download error -> FOTA_DOWNLOAD_FAILED */
void test_fota_download_error(void)
{
	fota_download_start_fake.return_val = 0;
	k_sleep(K_MSEC(100));

	fota_http_trigger("http://example.com/firmware.bin", -1);
	event_expect(FOTA_POLL_REQUEST);
	event_expect(FOTA_DOWNLOADING_UPDATE);

	simulate_dl_evt(FOTA_DOWNLOAD_EVT_ERROR, 1, 0);
	event_expect(FOTA_DOWNLOAD_FAILED);
}

/* Test: cancel in-progress download */
void test_fota_cancel_download(void)
{
	fota_download_start_fake.return_val = 0;
	k_sleep(K_MSEC(100));

	fota_http_trigger("http://example.com/firmware.bin", -1);
	event_expect(FOTA_POLL_REQUEST);
	event_expect(FOTA_DOWNLOADING_UPDATE);

	/* event_send publishes on FOTA_CHAN, fota_subscriber sees it too —
	 * drain that copy before expecting the module's FOTA_DOWNLOAD_CANCELED. */
	event_send(FOTA_DOWNLOAD_CANCEL);
	event_expect(FOTA_DOWNLOAD_CANCEL); /* drain our own sent message */

	simulate_dl_evt(FOTA_DOWNLOAD_EVT_CANCELLED, 0, 0);
	event_expect(FOTA_DOWNLOAD_CANCELED);

	TEST_ASSERT_EQUAL(1, fota_download_cancel_fake.call_count);
}

/* Test: fota_download_start failure -> FOTA_DOWNLOAD_FAILED */
void test_fota_start_failure(void)
{
	fota_download_start_fake.return_val = -EIO;
	k_sleep(K_MSEC(100));

	fota_http_trigger("http://example.com/firmware.bin", -1);
	event_expect(FOTA_POLL_REQUEST);
	event_expect(FOTA_DOWNLOAD_FAILED);
}

/* Test: fota_get_progress returns -1 before download */
void test_fota_progress_initial(void)
{
	int progress = fota_get_progress();

	TEST_ASSERT_EQUAL(-1, progress);
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
