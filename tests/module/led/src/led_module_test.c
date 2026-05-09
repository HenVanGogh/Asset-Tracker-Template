/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "app_common.h"
#include "led.h"

LOG_MODULE_REGISTER(led_module_test, 4);

/* Test hooks defined in redef.c */
extern void fake_pwm_reset(void);
extern unsigned int fake_pwm_call_count(void);
extern int fake_pwm_set_return_value;

struct fake_pwm_call {
	const void *spec;
	uint32_t period;
	uint32_t pulse;
};
extern const struct fake_pwm_call *fake_pwm_last_three(void);

static int publish_led_msg(uint8_t r, uint8_t g, uint8_t b,
			   uint32_t on_ms, uint32_t off_ms, int reps)
{
	struct led_msg msg = {
		.type = LED_RGB_SET,
		.red = r,
		.green = g,
		.blue = b,
		.duration_on_msec = on_ms,
		.duration_off_msec = off_ms,
		.repetitions = reps,
	};
	return zbus_chan_pub(&LED_CHAN, &msg, K_SECONDS(1));
}

void setUp(void)
{
	fake_pwm_reset();
	/* Wait for SYS_INIT-driven module setup */
	k_sleep(K_MSEC(50));
}

void tearDown(void)
{
	/* Quiet the LED so leftover work items don't bleed into the next test */
	(void)publish_led_msg(0, 0, 0, 1000, 1000, 0);
	k_sleep(K_MSEC(50));
}

/* Steady-on (repetitions = -1, treated as forever) should program PWM with the
 * requested colour and not cycle off. We only require that the colour was
 * written exactly once.
 */
void test_steady_on_writes_colour_once(void)
{
	int err = publish_led_msg(255, 128, 64, 100, 100, -1);

	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(50));

	const struct fake_pwm_call *last = fake_pwm_last_three();

	TEST_ASSERT_NOT_NULL(last);
	TEST_ASSERT_EQUAL_UINT32(255, last[0].pulse);
	TEST_ASSERT_EQUAL_UINT32(128, last[1].pulse);
	TEST_ASSERT_EQUAL_UINT32(64,  last[2].pulse);
}

/* repetitions = 0 should immediately turn the LED off (force_off path). */
void test_off_request_drives_zero_pulse(void)
{
	int err = publish_led_msg(255, 255, 255, 100, 100, 0);

	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(50));

	const struct fake_pwm_call *last = fake_pwm_last_three();

	TEST_ASSERT_NOT_NULL(last);
	TEST_ASSERT_EQUAL_UINT32(0, last[0].pulse);
	TEST_ASSERT_EQUAL_UINT32(0, last[1].pulse);
	TEST_ASSERT_EQUAL_UINT32(0, last[2].pulse);
}

/* A finite-blink request should flip the LED multiple times until reps reach 0
 * and then stop scheduling further toggles. We verify by counting PWM writes.
 */
void test_finite_blink_completes(void)
{
	const int reps = 3;
	const uint32_t on_ms = 30;
	const uint32_t off_ms = 30;

	int err = publish_led_msg(10, 20, 30, on_ms, off_ms, reps);

	TEST_ASSERT_EQUAL(0, err);

	/* Run for long enough to complete all cycles plus margin. */
	k_sleep(K_MSEC((on_ms + off_ms) * (reps + 2)));

	unsigned int writes_during_blink = fake_pwm_call_count();

	/* Each toggle writes 3 PWM channels; expect at least one write per
	 * toggle. With reps=3 we expect ~7 transitions (initial on + 3 off + 3 on).
	 */
	TEST_ASSERT_GREATER_OR_EQUAL(3 * 4, writes_during_blink);

	/* Wait again — no further writes should happen. */
	unsigned int after_done = fake_pwm_call_count();

	k_sleep(K_MSEC((on_ms + off_ms) * 3));

	TEST_ASSERT_EQUAL(after_done, fake_pwm_call_count());
}

/* A new LED message should cancel the in-flight blink and replace it. */
void test_new_message_cancels_previous_blink(void)
{
	int err = publish_led_msg(255, 0, 0, 50, 50, -1);

	TEST_ASSERT_EQUAL(0, err);
	k_sleep(K_MSEC(120));

	unsigned int after_first = fake_pwm_call_count();

	/* Replace with a steady-off command */
	err = publish_led_msg(0, 0, 0, 1000, 1000, 0);
	TEST_ASSERT_EQUAL(0, err);

	k_sleep(K_MSEC(200));

	unsigned int after_replace = fake_pwm_call_count();

	/* The off command writes 3 PWM channels; nothing else should follow. */
	const struct fake_pwm_call *last = fake_pwm_last_three();

	TEST_ASSERT_NOT_NULL(last);
	TEST_ASSERT_EQUAL_UINT32(0, last[0].pulse);
	TEST_ASSERT_EQUAL_UINT32(0, last[1].pulse);
	TEST_ASSERT_EQUAL_UINT32(0, last[2].pulse);

	/* Sleep more — make sure no new toggles are scheduled */
	k_sleep(K_MSEC(300));
	TEST_ASSERT_EQUAL(after_replace, fake_pwm_call_count());

	(void)after_first;
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
