/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Shared state between stubs.c and spi_dfu_module_test.c so the
 * performance test can assert against the fake transport's bookkeeping.
 */
#ifndef FAKE_TRANSPORT_H_
#define FAKE_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct fake_perf {
	uint64_t xfer_cycles;
	uint32_t xfer_calls;
	uint32_t smp_frames_in;
	uint32_t lock_calls;
	uint32_t unlock_calls;
};

struct fake_transport {
	/* Configuration — set before starting DFU. */
	size_t   image_size;
	size_t   chunk_size;            /* must match DFU_CHUNK_SIZE */
	uint32_t fail_after_upload_chunks; /* 0 = never */
	bool     reset_no_response;

	/* Bookkeeping — populated by the fake server. */
	size_t   upload_offset;
	uint32_t upload_chunks_seen;
	size_t   bytes_acknowledged;
	uint32_t image_list_requests;
	uint32_t image_test_requests;
	uint32_t reset_requests;
	uint32_t timeouts_injected;
};

extern struct fake_perf      g_perf;
extern struct fake_transport g_xport;

void spi_dfu_stubs_reset(void);
void fake_set_image_available(size_t size);

#endif /* FAKE_TRANSPORT_H_ */
