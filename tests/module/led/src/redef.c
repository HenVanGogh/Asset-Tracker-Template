/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct pwm_dt_spec; /* opaque to the test */

/* Capture last PWM call so the test can assert on the requested values. */
struct fake_pwm_call {
	const struct pwm_dt_spec *spec;
	uint32_t period;
	uint32_t pulse;
};

#define FAKE_PWM_HISTORY_LEN 32

static struct fake_pwm_call fake_pwm_history[FAKE_PWM_HISTORY_LEN];
static unsigned int fake_pwm_history_count;
int fake_pwm_set_return_value;

bool pwm_is_ready_dt(const struct pwm_dt_spec *spec)
{
	(void)spec;
	return true;
}

int pwm_set_dt(const struct pwm_dt_spec *spec, uint32_t period, uint32_t pulse)
{
	if (fake_pwm_history_count < FAKE_PWM_HISTORY_LEN) {
		fake_pwm_history[fake_pwm_history_count++] =
			(struct fake_pwm_call){ spec, period, pulse };
	}
	return fake_pwm_set_return_value;
}

void fake_pwm_reset(void)
{
	fake_pwm_history_count = 0;
	fake_pwm_set_return_value = 0;
}

unsigned int fake_pwm_call_count(void)
{
	return fake_pwm_history_count;
}

const struct fake_pwm_call *fake_pwm_last_three(void)
{
	if (fake_pwm_history_count < 3) {
		return NULL;
	}
	return &fake_pwm_history[fake_pwm_history_count - 3];
}
