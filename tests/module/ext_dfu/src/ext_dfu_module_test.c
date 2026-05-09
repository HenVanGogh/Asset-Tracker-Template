/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Public-API tests for ext_dfu. Coverage focuses on argument validation and
 * lifecycle state observable via ext_dfu_get_status().
 *
 * TODO (extend):
 *   - test ext_dfu_start() success path: capture downloader callback,
 *     drive DOWNLOADER_EVT_FRAGMENT / DONE, observe progress -> 100 -> DONE.
 *   - test concurrent start: second call returns -EBUSY.
 *   - test cancel during download: state -> IDLE.
 *   - test downloader error event: state -> ERROR with non-zero error code.
 */

#include <unity.h>
#include <zephyr/kernel.h>

#include "ext_dfu.h"

extern void ext_dfu_stubs_reset(void);

void setUp(void)
{
	ext_dfu_stubs_reset();
}

void tearDown(void) {}

void test_get_status_rejects_invalid_target(void)
{
	struct ext_dfu_status status;
	int err = ext_dfu_get_status((enum ext_dfu_target)EXT_DFU_TARGET_COUNT, &status);

	TEST_ASSERT_LESS_THAN(0, err);
}

void test_get_status_initial_idle(void)
{
	struct ext_dfu_status status;
	int err = ext_dfu_get_status(EXT_DFU_TARGET_NRF5340, &status);

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(EXT_DFU_STATE_IDLE, status.state);
}

void test_cancel_when_idle_returns_error(void)
{
	int err = ext_dfu_cancel();

	TEST_ASSERT_LESS_THAN(0, err);
}

void test_start_with_null_url_returns_error(void)
{
	int err = ext_dfu_start(EXT_DFU_TARGET_NRF5340, NULL, -1);

	TEST_ASSERT_LESS_THAN(0, err);
}

void test_start_with_invalid_target_returns_error(void)
{
	int err = ext_dfu_start((enum ext_dfu_target)EXT_DFU_TARGET_COUNT,
				"http://example.com/fw.bin", -1);

	TEST_ASSERT_LESS_THAN(0, err);
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
