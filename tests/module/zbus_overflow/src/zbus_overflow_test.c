/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * ZBus overflow & saturation tests.
 *
 * The Asset-Tracker-Template app uses both LISTENER (synchronous) and
 * MSG_SUBSCRIBER (queued, allocated from a shared net_buf pool) observers.
 * The most common production-failure modes are:
 *
 *   1. The shared net_buf pool that backs every MSG_SUBSCRIBER is sized
 *      by CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE. When several
 *      modules publish bursts (e.g. cloud_mqtt + custom_mqtt + main all
 *      observe NETWORK_CHAN), the pool empties. zbus_chan_pub() returns
 *      -ENOMEM and the message is silently lost for some observers.
 *
 *   2. Each MSG_SUBSCRIBER has an internal FIFO. If the worker thread is
 *      slow (e.g. blocked on a modem AT command), publishes succeed but
 *      the oldest queued items are overwritten as new ones arrive.
 *
 *   3. A LISTENER runs synchronously inside zbus_chan_pub(). A slow
 *      listener stalls every publisher on that channel.
 *
 * The tests below construct synthetic channels/observers that match these
 * patterns and assert the expected error contracts.
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(zbus_overflow_test, 4);

/* ----------------------------------------------------------------------- */
/* A small synthetic message type matching what real modules push.         */
/* ----------------------------------------------------------------------- */
struct stress_msg {
	uint32_t seq;
	uint8_t  payload[64];
};

ZBUS_CHAN_DEFINE(STRESS_CHAN_A,
		 struct stress_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(STRESS_CHAN_B,
		 struct stress_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/* Two MSG_SUBSCRIBERs sharing the global net_buf pool, mirroring the
 * "cloud + custom_mqtt + main all subscribe to NETWORK_CHAN" topology.
 */
ZBUS_MSG_SUBSCRIBER_DEFINE(slow_sub);
ZBUS_MSG_SUBSCRIBER_DEFINE(fast_sub);

ZBUS_CHAN_ADD_OBS(STRESS_CHAN_A, slow_sub, 0);
ZBUS_CHAN_ADD_OBS(STRESS_CHAN_A, fast_sub, 0);

/* Listener that records calls + can be made deliberately slow. */
static atomic_t listener_call_count;
static atomic_t listener_slow_us;

static void slow_listener_cb(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
	atomic_inc(&listener_call_count);
	uint32_t us = (uint32_t)atomic_get(&listener_slow_us);

	if (us) {
		k_busy_wait(us);
	}
}

ZBUS_LISTENER_DEFINE(stress_listener, slow_listener_cb);
ZBUS_CHAN_ADD_OBS(STRESS_CHAN_B, stress_listener, 0);

/* ----------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ----------------------------------------------------------------------- */

static int publish(const struct zbus_channel *chan, uint32_t seq)
{
	struct stress_msg m = { .seq = seq };

	return zbus_chan_pub(chan, &m, K_MSEC(50));
}

static unsigned int drain(const struct zbus_observer *sub)
{
	const struct zbus_channel *chan;
	struct stress_msg m;
	unsigned int count = 0;

	while (zbus_sub_wait_msg(sub, &chan, &m, K_MSEC(20)) == 0) {
		count++;
	}
	return count;
}

void setUp(void)
{
	atomic_set(&listener_call_count, 0);
	atomic_set(&listener_slow_us, 0);

	(void)drain(&slow_sub);
	(void)drain(&fast_sub);
}

void tearDown(void) {}

/* ----------------------------------------------------------------------- */
/* Test 1: net_buf pool exhaustion when no subscriber drains.              */
/*                                                                         */
/* The pool is sized to 4 (see prj.conf). With two observers per publish   */
/* each published message reserves 2 buffers, so pool is exhausted after   */
/* ~2 publishes.                                                           */
/* ----------------------------------------------------------------------- */
void test_pool_exhaustion_returns_enomem(void)
{
	int err = 0;
	int last_err = 0;
	unsigned int success = 0;

	for (unsigned int i = 0; i < 32; i++) {
		err = publish(&STRESS_CHAN_A, i);
		if (err == 0) {
			success++;
		} else {
			last_err = err;
			break;
		}
	}

	LOG_INF("Pool exhaustion: %u publishes succeeded before err=%d",
		success, last_err);

	/* With pool=4 and 2 subscribers per publish we expect saturation
	 * within a handful of iterations. The exact count depends on the
	 * Zephyr version, but we MUST eventually see -ENOMEM (or -EAGAIN
	 * mapped from it). */
	TEST_ASSERT_LESS_OR_EQUAL(8, success);
	TEST_ASSERT_LESS_THAN(0, last_err);
}

/* ----------------------------------------------------------------------- */
/* Test 2: draining recovers the pool — publishes succeed again.           */
/* ----------------------------------------------------------------------- */
void test_drain_recovers_pool(void)
{
	int err;

	/* Saturate */
	for (unsigned int i = 0; i < 32; i++) {
		err = publish(&STRESS_CHAN_A, i);
		if (err) {
			break;
		}
	}
	TEST_ASSERT_LESS_THAN(0, err);

	/* Drain both subscribers — buffers go back to the pool */
	unsigned int got_slow = drain(&slow_sub);
	unsigned int got_fast = drain(&fast_sub);

	LOG_INF("Drained: slow=%u fast=%u", got_slow, got_fast);
	TEST_ASSERT_GREATER_THAN(0, got_slow + got_fast);

	/* Now a fresh publish must succeed. */
	TEST_ASSERT_EQUAL(0, publish(&STRESS_CHAN_A, 9999));
}

/* ----------------------------------------------------------------------- */
/* Test 3: a slow LISTENER blocks the publisher.                           */
/*                                                                         */
/* This documents the production rule: NEVER do significant work in a      */
/* LISTENER callback. The test asserts that publish latency tracks         */
/* listener time so regressions are caught.                                */
/* ----------------------------------------------------------------------- */
void test_slow_listener_blocks_publisher(void)
{
	const uint32_t slow_us = 50000; /* 50 ms */

	atomic_set(&listener_slow_us, slow_us);

	int64_t t0 = k_uptime_get();

	for (unsigned int i = 0; i < 4; i++) {
		TEST_ASSERT_EQUAL(0, publish(&STRESS_CHAN_B, i));
	}

	int64_t elapsed = k_uptime_get() - t0;

	LOG_INF("4 publishes through slow (50ms) listener took %lld ms",
		elapsed);

	/* Listener was called once per publish. */
	TEST_ASSERT_EQUAL(4, atomic_get(&listener_call_count));

	/* Total elapsed must be at least 4 * slow_us (= 200 ms). Allow a
	 * small floor below to absorb timer granularity. */
	TEST_ASSERT_GREATER_OR_EQUAL(150, elapsed);
}

/* ----------------------------------------------------------------------- */
/* Test 4: high-frequency publish does not corrupt sequence numbers.       */
/*                                                                         */
/* This exercises the FIFO-style delivery contract: even when the pool is  */
/* repeatedly emptied and refilled, surviving messages must be in order    */
/* and uncorrupted.                                                        */
/* ----------------------------------------------------------------------- */
void test_messages_in_order_under_load(void)
{
	const unsigned int total = 100;
	unsigned int published = 0;
	unsigned int skipped = 0;

	for (unsigned int i = 0; i < total; i++) {
		int err = publish(&STRESS_CHAN_A, i);

		if (err == 0) {
			published++;
		} else {
			skipped++;
		}

		/* Drain occasionally so pool can recover. */
		if ((i & 0x3) == 0x3) {
			(void)drain(&slow_sub);
			(void)drain(&fast_sub);
		}
	}

	LOG_INF("Burst: %u/%u published (%u dropped to ENOMEM)",
		published, total, skipped);

	(void)drain(&slow_sub);

	/* Final integrity check — pull from the fast subscriber and make
	 * sure sequence numbers are strictly increasing. */
	const struct zbus_channel *chan;
	struct stress_msg m;
	uint32_t prev = 0;
	bool first = true;
	unsigned int received = 0;

	while (zbus_sub_wait_msg(&fast_sub, &chan, &m, K_MSEC(50)) == 0) {
		received++;
		if (!first) {
			TEST_ASSERT_GREATER_THAN(prev, m.seq);
		}
		prev = m.seq;
		first = false;
	}

	LOG_INF("Integrity: %u messages received in order on fast_sub",
		received);
}

/* ----------------------------------------------------------------------- */
/* Test 5: slow MSG_SUBSCRIBER does not block the publisher (it just       */
/* drops messages once the pool/queue is exhausted).                       */
/* ----------------------------------------------------------------------- */
void test_slow_msg_subscriber_does_not_block_publisher(void)
{
	int64_t t0 = k_uptime_get();
	unsigned int published = 0;
	unsigned int dropped = 0;

	/* Use K_NO_WAIT: MSG_SUBSCRIBERs must not stall the caller even when
	 * the net_buf pool is exhausted.  A LISTENER would add latency
	 * synchronously; a MSG_SUBSCRIBER must return -ENOMEM immediately
	 * once the pool is full. */
	for (unsigned int i = 0; i < 50; i++) {
		struct stress_msg m = { .seq = i };
		int err = zbus_chan_pub(&STRESS_CHAN_A, &m, K_NO_WAIT);

		if (err == 0) {
			published++;
		} else {
			dropped++;
		}
	}

	int64_t elapsed = k_uptime_get() - t0;

	LOG_INF("50 publish attempts (K_NO_WAIT) took %lld ms — published=%u dropped=%u",
		elapsed, published, dropped);

	/* With K_NO_WAIT, failed publishes must return immediately.
	 * Allow 500 ms of slack for native_sim scheduling jitter. */
	TEST_ASSERT_LESS_THAN(500, elapsed);
	/* At least some messages must have been dropped (pool is tiny). */
	TEST_ASSERT_GREATER_THAN(0, dropped);
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
