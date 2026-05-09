/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * spi_dfu module test — argument validation, full end-to-end pipeline
 * walk-through with a fake SMP server, and a performance/diagnostic
 * test that prints a software-timing report at the end of the run.
 *
 * The performance test answers two questions:
 *   1. How many SPI xfer calls and how much wall time does spi_dfu need
 *      to push a typical-sized image (64 KiB)?
 *   2. Where is time spent? (Polling overhead vs. actual SMP exchanges.)
 *
 * The fake transport in stubs.c records every xfer + the cumulative
 * cycles spent inside the transport. spi_dfu's real timing is dominated
 * by k_msleep(DFU_POLL_INTERVAL_MS=10) waits for DRDY, but our fake
 * always reports DRDY active immediately, so the measured numbers
 * isolate spi_dfu's *software* cost from the real SPI bus latency.
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "spi_dfu.h"
#include "fake_transport.h"

#define DFU_CHUNK_SIZE  256

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool wait_for_state(enum spi_dfu_state want, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	struct spi_dfu_status st;

	while (k_uptime_get() < deadline) {
		if (spi_dfu_get_status(&st) == 0 && st.state == want) {
			return true;
		}
		k_msleep(5);
	}
	return false;
}

static bool wait_for_terminal_state(int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	struct spi_dfu_status st;

	while (k_uptime_get() < deadline) {
		if (spi_dfu_get_status(&st) == 0 &&
		    (st.state == SPI_DFU_STATE_DONE ||
		     st.state == SPI_DFU_STATE_ERROR)) {
			return true;
		}
		k_msleep(5);
	}
	return false;
}

void setUp(void)
{
	/* Wait for any previous DFU to settle BEFORE resetting stubs.
	 * Calling spi_dfu_stubs_reset() while an upload is in flight
	 * clears fail_after_upload_chunks, causing the active thread
	 * to resume rather than cancelling — so it never reaches a terminal state. */
	int64_t deadline = k_uptime_get() + 32000;
	struct spi_dfu_status st;

	while (k_uptime_get() < deadline) {
		if (spi_dfu_get_status(&st) == 0 &&
		    (st.state == SPI_DFU_STATE_IDLE ||
		     st.state == SPI_DFU_STATE_DONE ||
		     st.state == SPI_DFU_STATE_ERROR)) {
			break;
		}
		k_msleep(10);
	}
	spi_dfu_stubs_reset();
}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Argument-validation tests                                          */
/* ------------------------------------------------------------------ */

void test_get_status_with_null_returns_error(void)
{
	TEST_ASSERT_LESS_THAN(0, spi_dfu_get_status(NULL));
}

void test_get_status_initial_state_is_idle_or_terminal(void)
{
	struct spi_dfu_status st;

	TEST_ASSERT_EQUAL(0, spi_dfu_get_status(&st));
	/* The DFU thread is global — earlier tests may have left it in
	 * DONE/ERROR. Either is acceptable as long as the call returns. */
	TEST_ASSERT_TRUE(st.state == SPI_DFU_STATE_IDLE ||
			 st.state == SPI_DFU_STATE_DONE ||
			 st.state == SPI_DFU_STATE_ERROR);
}

void test_cancel_when_idle_returns_error(void)
{
	int err = spi_dfu_cancel();

	TEST_ASSERT_LESS_THAN(0, err);
}

void test_start_without_firmware_returns_enoent(void)
{
	int err = spi_dfu_start_nrf5340();

	TEST_ASSERT_EQUAL(-ENOENT, err);
}

/* ------------------------------------------------------------------ */
/* End-to-end happy path: drives all four phases.                     */
/* ------------------------------------------------------------------ */
void test_full_pipeline_completes_done(void)
{
	const size_t img = 4 * DFU_CHUNK_SIZE; /* 1 KiB — 4 chunks */

	g_xport.image_size = img;
	g_xport.chunk_size = DFU_CHUNK_SIZE;
	g_xport.reset_no_response = true; /* match real device behaviour */
	fake_set_image_available(img);

	TEST_ASSERT_EQUAL(0, spi_dfu_start_nrf5340());
	TEST_ASSERT_TRUE(wait_for_terminal_state(10000));

	struct spi_dfu_status st;

	TEST_ASSERT_EQUAL(0, spi_dfu_get_status(&st));
	TEST_ASSERT_EQUAL_MESSAGE(SPI_DFU_STATE_DONE, st.state, st.error_msg);
	TEST_ASSERT_EQUAL(img, st.bytes_uploaded);
	TEST_ASSERT_EQUAL(img, st.image_size);
	TEST_ASSERT_EQUAL(100, st.progress_pct);
	TEST_ASSERT_EQUAL(4, g_xport.upload_chunks_seen);
	TEST_ASSERT_EQUAL(1, g_xport.image_list_requests);
	TEST_ASSERT_EQUAL(1, g_xport.image_test_requests);
	TEST_ASSERT_EQUAL(1, g_xport.reset_requests);
}

/* ------------------------------------------------------------------ */
/* Failure injection: at least one chunk succeeds before the stall.   */
/* ------------------------------------------------------------------ */
void test_upload_makes_partial_progress_then_stalls(void)
{
	const size_t img = 8 * DFU_CHUNK_SIZE;

	g_xport.image_size = img;
	g_xport.chunk_size = DFU_CHUNK_SIZE;
	g_xport.fail_after_upload_chunks = 2; /* 2 ok then stall */
	fake_set_image_available(img);

	(void)spi_dfu_start_nrf5340();

	(void)wait_for_state(SPI_DFU_STATE_UPLOADING, 1000);
	k_msleep(500);

	TEST_ASSERT_EQUAL(2, g_xport.upload_chunks_seen);
	TEST_ASSERT_GREATER_OR_EQUAL(1, g_xport.timeouts_injected);

	/* Don't wait the full 30s SMP timeout; cancellation lets the
	 * thread unwind quickly. */
	(void)spi_dfu_cancel();
	/* The cancel flag is only checked between chunks. The active SMP
	 * exchange has DFU_RETRY_COUNT(3) retries × DFU_SMP_TIMEOUT_MS(30s) =
	 * up to 90 simulated seconds before the thread reaches ERROR state.
	 * With CONFIG_NATIVE_SIM_SLOWDOWN_TO_REAL_TIME=n this is nearly free
	 * in wall-clock time, so we wait here to prevent EBUSY in the next test. */
	(void)wait_for_terminal_state(95000);
}

/* ------------------------------------------------------------------ */
/* Performance / diagnostic test.                                     */
/*                                                                    */
/* Drives a 64 KiB upload and prints a structured report:             */
/*   spi_dfu_perf <metric>=<value>                                    */
/* The user can grep the build log for "spi_dfu_perf" to extract a    */
/* baseline and watch for regressions.                                */
/* ------------------------------------------------------------------ */
void test_performance_report_64k_upload(void)
{
	const size_t img = 64 * 1024; /* 256 chunks */

	g_xport.image_size = img;
	g_xport.chunk_size = DFU_CHUNK_SIZE;
	g_xport.reset_no_response = true;
	fake_set_image_available(img);

	int64_t  t0   = k_uptime_get();
	uint64_t cyc0 = k_cycle_get_64();

	TEST_ASSERT_EQUAL(0, spi_dfu_start_nrf5340());
	TEST_ASSERT_TRUE_MESSAGE(wait_for_terminal_state(60000),
				 "DFU did not reach terminal state in 60s");

	int64_t  elapsed_ms = k_uptime_get() - t0;
	uint64_t cyc_total  = k_cycle_get_64() - cyc0;

	struct spi_dfu_status st;

	(void)spi_dfu_get_status(&st);
	TEST_ASSERT_EQUAL_MESSAGE(SPI_DFU_STATE_DONE, st.state, st.error_msg);

	uint32_t expected_chunks = (uint32_t)(img / DFU_CHUNK_SIZE);
	uint32_t throughput_kbps = elapsed_ms > 0
		? (uint32_t)((img * 8ULL) / (uint32_t)elapsed_ms)
		: 0;
	uint32_t avg_chunk_us = expected_chunks
		? (uint32_t)((elapsed_ms * 1000ULL) / expected_chunks)
		: 0;
	uint64_t xfer_us = k_cyc_to_us_floor64(g_perf.xfer_cycles);
	uint32_t avg_xfer_us = g_perf.xfer_calls
		? (uint32_t)(xfer_us / g_perf.xfer_calls) : 0;

	printk("\n");
	printk("================== spi_dfu performance report ==================\n");
	printk("spi_dfu_perf image_bytes=%u\n",          (unsigned int)img);
	printk("spi_dfu_perf chunks_expected=%u\n",      expected_chunks);
	printk("spi_dfu_perf chunks_acked=%u\n",         g_xport.upload_chunks_seen);
	printk("spi_dfu_perf duration_ms=%lld\n",        elapsed_ms);
	printk("spi_dfu_perf software_throughput_kbps=%u\n", throughput_kbps);
	printk("spi_dfu_perf avg_chunk_latency_us=%u\n", avg_chunk_us);
	printk("spi_dfu_perf spi_xfer_calls=%u\n",       g_perf.xfer_calls);
	printk("spi_dfu_perf smp_frames_sent=%u\n",      g_perf.smp_frames_in);
	printk("spi_dfu_perf bus_lock_count=%u\n",       g_perf.lock_calls);
	printk("spi_dfu_perf bus_unlock_count=%u\n",     g_perf.unlock_calls);
	printk("spi_dfu_perf transport_total_us=%u\n",   (unsigned int)xfer_us);
	printk("spi_dfu_perf transport_avg_us=%u\n",     avg_xfer_us);
	printk("spi_dfu_perf total_cpu_cycles=%llu\n",   cyc_total);
	printk("spi_dfu_perf retries_observed=%u\n",     g_xport.timeouts_injected);
	printk("=================================================================\n");

	/* Sanity assertions so the report can't silently become meaningless. */
	TEST_ASSERT_EQUAL(expected_chunks, g_xport.upload_chunks_seen);
	TEST_ASSERT_EQUAL(img, st.bytes_uploaded);
	TEST_ASSERT_EQUAL(0,   g_xport.timeouts_injected);
	TEST_ASSERT_GREATER_THAN(expected_chunks, g_perf.xfer_calls);

	/* Lock/unlock must be perfectly balanced — leaks would deadlock
	 * the bus in production. */
	TEST_ASSERT_EQUAL(g_perf.lock_calls, g_perf.unlock_calls);
}

extern int unity_main(void);

int main(void) { (void)unity_main(); return 0; }
