/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <modem/lte_lc.h>

#include "app_common.h"
#include "selftest.h"
#include "network.h"

LOG_MODULE_REGISTER(selftest_module_test, 4);

/* selftest.c subscribes to NETWORK_CHAN, so we must define it here. */
ZBUS_CHAN_DEFINE(NETWORK_CHAN,
		 struct network_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/* Subscriber for SELFTEST_CHAN */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);
ZBUS_CHAN_ADD_OBS(SELFTEST_CHAN, test_subscriber, 0);

/* ---- AT command stub ----
 * The real selftest issues AT commands via nrf_modem_at_cmd(); for the test we
 * intercept them and return canned responses controllable from the test body.
 */

enum at_response_mode {
	AT_RESP_HAPPY,        /* All checks pass */
	AT_RESP_NO_SIM,       /* AT+CPIN? returns +CME ERROR */
	AT_RESP_PIN_REQUIRED, /* AT+CPIN? returns "+CPIN: SIM PIN" */
	AT_RESP_NO_NETWORK,   /* CEREG returns stat=0 (not registered) */
};

static enum at_response_mode at_mode = AT_RESP_HAPPY;
static unsigned int at_call_count;

int nrf_modem_at_cmd(void *buf, size_t len, const char *fmt, ...)
{
	char cmd[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);

	at_call_count++;

	char *out = (char *)buf;

	if (strstr(cmd, "AT+CPIN?")) {
		switch (at_mode) {
		case AT_RESP_NO_SIM:
			return -1;
		case AT_RESP_PIN_REQUIRED:
			snprintf(out, len, "+CPIN: SIM PIN\r\nOK\r\n");
			return 0;
		default:
			snprintf(out, len, "+CPIN: READY\r\nOK\r\n");
			return 0;
		}
	}
	if (strstr(cmd, "AT+CCID")) {
		snprintf(out, len, "+CCID: \"8901260000000000000\"\r\nOK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CIMI")) {
		snprintf(out, len, "310170000000000\r\nOK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CGMR")) {
		snprintf(out, len, "mfw_nrf91x1_2.0.1\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CEREG=5")) {
		snprintf(out, len, "OK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CEREG?")) {
		if (at_mode == AT_RESP_NO_NETWORK) {
			snprintf(out, len, "+CEREG: 5,0\r\nOK\r\n");
		} else {
			snprintf(out, len,
				 "+CEREG: 5,1,\"ABCD\",\"12345678\"\r\nOK\r\n");
		}
		return 0;
	}
	if (strstr(cmd, "AT+COPS?")) {
		snprintf(out, len, "+COPS: 0,0,\"TestOp\"\r\nOK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT%CESQ")) {
		/* Strong signal: rsrp_raw=80 -> -60 dBm, rsrq_raw=30 -> -4.5 dB */
		snprintf(out, len, "%%CESQ: 50,2,255,255,30,80,255\r\nOK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CESQ")) {
		snprintf(out, len, "+CESQ: 50,2,255,255,30,80\r\nOK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CPSMS?")) {
		snprintf(out, len, "+CPSMS: 1\r\nOK\r\n");
		return 0;
	}
	if (strstr(cmd, "AT+CGDCONT?")) {
		snprintf(out, len,
			 "+CGDCONT: 0,\"IP\",\"apn\",\"10.0.0.1\"\r\nOK\r\n");
		return 0;
	}

	/* Unknown command — succeed quietly */
	if (out && len) {
		out[0] = '\0';
	}
	return 0;
}

/* ---- lte_lc stubs (subset used by selftest.c) ---- */

static int psm_tau_stub = 3600;
static int psm_active_stub = 16;
static int psm_get_return = 0;

int lte_lc_psm_get(int *tau, int *active_time)
{
	if (psm_get_return) {
		return psm_get_return;
	}
	*tau = psm_tau_stub;
	*active_time = psm_active_stub;
	return 0;
}

/* ---- DNS stub (selftest uses zsock_getaddrinfo for the DNS check) ---- */

static int dns_return = 0;
static struct zsock_addrinfo fake_addr;

int zsock_getaddrinfo(const char *host, const char *service,
		      const struct zsock_addrinfo *hints,
		      struct zsock_addrinfo **res)
{
	ARG_UNUSED(host);
	ARG_UNUSED(service);
	ARG_UNUSED(hints);
	if (dns_return != 0) {
		*res = NULL;
		return dns_return;
	}
	memset(&fake_addr, 0, sizeof(fake_addr));
	*res = &fake_addr;
	return 0;
}

void zsock_freeaddrinfo(struct zsock_addrinfo *res)
{
	ARG_UNUSED(res);
}

/* ---- Test helpers ---- */

static void send_network_connected(void)
{
	struct network_msg msg = { .type = NETWORK_CONNECTED };

	int err = zbus_chan_pub(&NETWORK_CHAN, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

static int wait_for_msg(struct selftest_msg *msg, enum selftest_msg_type expected,
			k_timeout_t timeout)
{
	const struct zbus_channel *chan;
	int64_t end = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

	while (k_uptime_get() < end) {
		int err = zbus_sub_wait_msg(&test_subscriber, &chan, msg,
					    K_MSEC(500));
		if (err == 0 && chan == &SELFTEST_CHAN && msg->type == expected) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

void setUp(void)
{
	at_mode = AT_RESP_HAPPY;
	at_call_count = 0;
	psm_tau_stub = 3600;
	psm_active_stub = 16;
	psm_get_return = 0;
	dns_return = 0;
}

void tearDown(void)
{
	const struct zbus_channel *chan;
	struct selftest_msg drain;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &drain, K_MSEC(50)) == 0) {
		/* drain */
	}
}

/* ----------------------------------------------------------------------- */

/* The selftest thread is started by K_THREAD_DEFINE at boot. The thread waits
 * 3 seconds, then runs the full sequence. The test simulates a quick network
 * connect and verifies that STARTED then COMPLETE messages are emitted.
 */
void test_selftest_happy_path_publishes_started_and_complete(void)
{
	struct selftest_msg msg;
	int err;

	/* Publish a NETWORK_CONNECTED so the test does not have to wait the
	 * full SIM_WAIT_SECONDS for AT polling to "succeed".
	 */
	send_network_connected();

	err = wait_for_msg(&msg, SELFTEST_STARTED, K_SECONDS(10));
	TEST_ASSERT_EQUAL_MESSAGE(0, err, "did not receive SELFTEST_STARTED");

	err = wait_for_msg(&msg, SELFTEST_COMPLETE, K_SECONDS(20));
	TEST_ASSERT_EQUAL_MESSAGE(0, err, "did not receive SELFTEST_COMPLETE");

	TEST_ASSERT_EQUAL(SELFTEST_COMPLETE, msg.type);
	TEST_ASSERT_GREATER_THAN(0, at_call_count);
	TEST_ASSERT_GREATER_THAN(0, msg.test_duration_ms);
	/* On the happy path no error flags should be set. */
	TEST_ASSERT_EQUAL_HEX32(0, msg.flags);
}

/* selftest_led_is_active() must return false before the LED window starts and
 * (since CONFIG_APP_LED is not compiled into the test) should remain false
 * throughout.
 */
void test_selftest_led_active_flag_default_false(void)
{
	TEST_ASSERT_FALSE(selftest_led_is_active());
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
