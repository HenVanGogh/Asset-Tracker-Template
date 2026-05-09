/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef LED_REDEF_H_
#define LED_REDEF_H_

/* Prevent <zephyr/drivers/pwm.h> from being expanded so that we can supply
 * our own minimal stand-ins for the PWM API used by led.c. The real header
 * pulls in devicetree macros and inline functions that require board PWM
 * aliases (pwm_led0/1/2) which native_sim does not have.
 */
#define ZEPHYR_INCLUDE_DRIVERS_PWM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

struct pwm_dt_spec {
	const struct device *dev;
	uint32_t channel;
	uint32_t period;
	uint32_t flags;
};

#define PWM_USEC(x) (x)

/* Devicetree macros referenced in led.c — make them safe no-ops. */
#undef DT_ALIAS
#define DT_ALIAS(name) DT_INVALID_NODE
#undef DT_NODE_HAS_STATUS
#define DT_NODE_HAS_STATUS(node, status) 1
#undef PWM_DT_SPEC_GET
#define PWM_DT_SPEC_GET(node) ((struct pwm_dt_spec){ .dev = NULL })

bool pwm_is_ready_dt(const struct pwm_dt_spec *spec);
int  pwm_set_dt(const struct pwm_dt_spec *spec, uint32_t period, uint32_t pulse);

#endif /* LED_REDEF_H_ */
