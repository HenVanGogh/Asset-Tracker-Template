/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * SPI SMP Forward — MCUmgr SMP server transport for nRF9151 DFU via USB.
 *
 * FLOW
 * ----
 * PC (mcumgr) → USB CDC → nRF5340 (spi_smp_fwd.c) → SPI FRAME_SMP_FWD(0x07)
 *   → nRF9151 uart_sensor.c (process_rx_frame) → spi_fwd_rx() [here]
 *   → MCUmgr SMP server → output() callback
 *   → uart_sensor_spi_send_smp_fwd() → SPI FRAME_SMP_FWD(0x07)
 *   → nRF5340 (spi_smp_fwd_rx) → USB CDC → PC
 *
 * The nRF9151 is the SPI MASTER so it initiates every transaction.
 * After MCUmgr calls output(), a work item triggers a new SPI exchange
 * carrying the FRAME_SMP_FWD response frame in the TX slot.
 */

#include "spi_fwd.h"
#include "uart_sensor.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>

LOG_MODULE_REGISTER(spi_fwd, CONFIG_APP_SPI_FWD_LOG_LEVEL);

/* smp_rx_req is defined in zephyr/subsys/mgmt/mcumgr/transport/src/smp.c.
 * No public header exposes it in this NCS version, so declare it here. */
struct smp_transport;
struct net_buf;
extern void smp_rx_req(struct smp_transport *smpt, struct net_buf *nb);

/* ── Constants ─────────────────────────────────────────────────────────── */

/* SMP MTU — must match CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE and the SPI
 * frame payload size (512 bytes). */
#define SPI_FWD_MTU 512

/* ── Response buffer ───────────────────────────────────────────────────── */
/*
 * MCUmgr's workqueue calls output() when a response is ready.
 * A work item then sends it back over SPI via uart_sensor_spi_send_smp_fwd().
 *
 * Only one SMP request/response is in flight at a time — the PC must
 * wait for the response before issuing the next request (standard MCUmgr
 * request-response protocol). The atomic_t flag ensures the work item
 * sees consistent data written by the MCUmgr workqueue thread.
 */
static uint8_t resp_data[SPI_FWD_MTU];
static uint16_t resp_len;
static atomic_t resp_ready;

/* ── Forward declarations ──────────────────────────────────────────────── */

static void spi_fwd_send_work_fn(struct k_work *work);
static K_WORK_DEFINE(spi_fwd_send_work, spi_fwd_send_work_fn);

/* ── MCUmgr transport callbacks ────────────────────────────────────────── */

/**
 * Called by MCUmgr when an SMP response for the PC is ready.
 * Store it and schedule sending it back to the nRF5340 over SPI.
 */
static int spi_fwd_output(struct net_buf *nb)
{
	if (nb->len > SPI_FWD_MTU) {
		LOG_ERR("SMP FWD response too large (%u > %u), dropped",
			nb->len, SPI_FWD_MTU);
		smp_packet_free(nb);
		return -EMSGSIZE;
	}

	/* Copy before freeing — fill resp_data before setting the atomic flag
	 * so the work handler always sees a consistent buffer. */
	resp_len = nb->len;
	memcpy(resp_data, nb->data, nb->len);
	smp_packet_free(nb);

	atomic_set(&resp_ready, 1);
	uart_sensor_spi_submit_work(&spi_fwd_send_work);

	LOG_INF("SPI FWD: SMP response queued (%u bytes)", resp_len);
	return 0;
}

static uint16_t spi_fwd_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	return SPI_FWD_MTU;
}

/* ── Transport instance ────────────────────────────────────────────────── */

static struct smp_transport spi_fwd_transport;

/* ── Work handler: send response back to nRF5340 ──────────────────────── */

static void spi_fwd_send_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_get(&resp_ready)) {
		return;
	}

	/* Safe to use resp_data directly — the atomic flag serializes
	 * access and MCUmgr is strictly request-response. */
	uint16_t len = resp_len;

	atomic_set(&resp_ready, 0);

	int ret = uart_sensor_spi_send_smp_fwd(resp_data, len);

	if (ret != 0) {
		LOG_ERR("SPI FWD: sending response to nRF5340 failed: %d", ret);
	} else {
		LOG_INF("SPI FWD: response sent to nRF5340 (%u bytes)", len);
	}
}

/* ── Public API ────────────────────────────────────────────────────────── */

void spi_fwd_rx(const uint8_t *data, uint16_t len)
{
	struct net_buf *nb = smp_packet_alloc();

	if (!nb) {
		LOG_ERR("SPI FWD: smp_packet_alloc failed — request dropped");
		return;
	}

	if (len > net_buf_tailroom(nb)) {
		LOG_ERR("SPI FWD: SMP packet too large (%u > %zu), dropped",
			len, net_buf_tailroom(nb));
		smp_packet_free(nb);
		return;
	}

	net_buf_add_mem(nb, data, len);
	smp_rx_req(&spi_fwd_transport, nb);
	LOG_INF("SPI FWD: SMP request from PC (%u bytes) → MCUmgr", len);
}

/* ── Initialisation ────────────────────────────────────────────────────── */

static int spi_fwd_init(void)
{
	spi_fwd_transport.functions.output  = spi_fwd_output;
	spi_fwd_transport.functions.get_mtu = spi_fwd_get_mtu;

	int rc = smp_transport_init(&spi_fwd_transport);

	if (rc) {
		LOG_ERR("SPI FWD: SMP transport init failed: %d", rc);
		return rc;
	}

	LOG_INF("SPI FWD: SMP server transport ready (nRF9151 DFU via USB enabled)");
	return 0;
}

/* Run after the SPI bus (uart_sensor, prio 90) is initialised. */
SYS_INIT(spi_fwd_init, APPLICATION, 93);
