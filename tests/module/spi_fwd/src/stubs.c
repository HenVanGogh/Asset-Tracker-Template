/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Stubs for MCUmgr SMP transport and net_buf functions used by spi_fwd.c.
 * Tracks calls so the test body can verify spi_fwd_rx() actually delivers
 * data into MCUmgr.
 */

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <string.h>

/* Test-visible state */
struct stub_state {
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
};

struct stub_state spi_fwd_stub;

void spi_fwd_stub_reset(void)
{
	memset(&spi_fwd_stub, 0, sizeof(spi_fwd_stub));
}

/* ---- net_buf stub ----
 * spi_fwd.c uses smp_packet_alloc/free, which return a struct net_buf.
 * Provide a single static buf with a backing data area large enough
 * for a 512-byte SMP MTU.
 */
#define STUB_NB_DATA_SIZE 1024

static uint8_t  stub_nb_storage[STUB_NB_DATA_SIZE];
static struct net_buf stub_nb;

struct net_buf *smp_packet_alloc(void)
{
	spi_fwd_stub.packet_alloc_count++;
	if (spi_fwd_stub.alloc_returns_null) {
		return NULL;
	}
	memset(&stub_nb, 0, sizeof(stub_nb));
	stub_nb.data = stub_nb_storage;
	stub_nb.__buf = stub_nb_storage;
	stub_nb.size = STUB_NB_DATA_SIZE;
	stub_nb.len = 0;
	return &stub_nb;
}

void smp_packet_free(struct net_buf *nb)
{
	(void)nb;
	spi_fwd_stub.packet_free_count++;
}

/* spi_fwd.c declares smp_rx_req extern. Provide it. */
struct smp_transport;
void smp_rx_req(struct smp_transport *t, struct net_buf *nb)
{
	(void)t;
	spi_fwd_stub.rx_req_count++;
	if (nb && nb->len <= sizeof(spi_fwd_stub.last_rx_data)) {
		memcpy(spi_fwd_stub.last_rx_data, nb->data, nb->len);
		spi_fwd_stub.last_rx_len = nb->len;
	}
}

/* smp_transport_init — referenced by SYS_INIT. Return 0. */
int smp_transport_init(struct smp_transport *t)
{
	(void)t;
	return 0;
}

/* ---- uart_sensor stubs ---- */

void uart_sensor_spi_submit_work(struct k_work *work)
{
	spi_fwd_stub.submit_work_count++;
	if (work) {
		k_work_submit(work);
	}
}

int uart_sensor_spi_send_smp_fwd(const uint8_t *data, uint16_t len)
{
	spi_fwd_stub.send_smp_fwd_count++;
	if (len <= sizeof(spi_fwd_stub.last_send_data)) {
		memcpy(spi_fwd_stub.last_send_data, data, len);
		spi_fwd_stub.last_send_len = len;
	}
	return spi_fwd_stub.send_smp_fwd_return;
}

/* ---- net_buf_simple non-inline functions -------------------------------- */

void *net_buf_simple_add_mem(struct net_buf_simple *buf, const void *mem, size_t len)
{
	uint8_t *tail = buf->data + buf->len;

	buf->len += len;
	return memcpy(tail, mem, len);
}

size_t net_buf_simple_tailroom(const struct net_buf_simple *buf)
{
	return buf->size - (size_t)(buf->data - buf->__buf) - buf->len;
}
