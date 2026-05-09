/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "spi_fwd.h"

extern struct stub_state {
	unsigned int packet_alloc_count;
	unsigned int packet_free_count;
	unsigned int rx_req_count;
	unsigned int submit_work_count;
	unsigned int send_smp_fwd_count;
	uint8_t      last_rx_data[1024];
	uint16_t     last_rx_len;
	uint8_t      last_send_data[1024];
	uint16_t     last_send_len;
	int          send_smp_fwd_return;
	bool         alloc_returns_null;
} spi_fwd_stub;

extern void spi_fwd_stub_reset(void);

void setUp(void)
{
	spi_fwd_stub_reset();
}

void tearDown(void) {}

/* Happy path: an SMP request from the PC should be forwarded into MCUmgr
 * via smp_rx_req(), and the buffer contents must match the input.
 */
void test_spi_fwd_rx_forwards_into_mcumgr(void)
{
	uint8_t pkt[64];

	for (size_t i = 0; i < sizeof(pkt); i++) {
		pkt[i] = (uint8_t)i;
	}

	spi_fwd_rx(pkt, sizeof(pkt));

	TEST_ASSERT_EQUAL(1, spi_fwd_stub.packet_alloc_count);
	TEST_ASSERT_EQUAL(1, spi_fwd_stub.rx_req_count);
	TEST_ASSERT_EQUAL(sizeof(pkt), spi_fwd_stub.last_rx_len);
	TEST_ASSERT_EQUAL_MEMORY(pkt, spi_fwd_stub.last_rx_data, sizeof(pkt));
}

/* If smp_packet_alloc() fails, spi_fwd_rx() must drop the request without
 * calling smp_rx_req() and without leaking a packet.
 */
void test_spi_fwd_rx_drops_when_alloc_fails(void)
{
	uint8_t pkt[16] = {0};

	spi_fwd_stub.alloc_returns_null = true;
	spi_fwd_rx(pkt, sizeof(pkt));

	TEST_ASSERT_EQUAL(1, spi_fwd_stub.packet_alloc_count);
	TEST_ASSERT_EQUAL(0, spi_fwd_stub.rx_req_count);
	TEST_ASSERT_EQUAL(0, spi_fwd_stub.packet_free_count);
}

/* A request larger than the net_buf tailroom must be rejected and the
 * allocated packet freed.
 */
void test_spi_fwd_rx_rejects_oversize(void)
{
	static uint8_t big[2048];

	memset(big, 0xAA, sizeof(big));
	spi_fwd_rx(big, sizeof(big));

	TEST_ASSERT_EQUAL(1, spi_fwd_stub.packet_alloc_count);
	TEST_ASSERT_EQUAL(1, spi_fwd_stub.packet_free_count);
	TEST_ASSERT_EQUAL(0, spi_fwd_stub.rx_req_count);
}

extern int unity_main(void);

int main(void)
{
	(void)unity_main();
	return 0;
}
