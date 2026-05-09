/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Fake SPI transport + ext_dfu + flash_area stubs for spi_dfu tests.
 *
 * The fake transport emulates an nRF5340 MCUboot SMP server well enough
 * for the spi_dfu module thread to drive a complete:
 *
 *     UPLOADING -> TESTING -> RESETTING -> DONE
 *
 * sequence. It also instruments per-call timing so the performance test
 * can compute throughput, average chunk latency, retry rate, etc.
 */

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <string.h>

#include "ext_dfu.h"
#include "fake_transport.h"

/* SPI frame layout — must match spi_dfu.c */
#define SPI_FRAME_SIZE   520
#define SPI_PAYLOAD_MAX  512
#define SPI_MAGIC        0xA5
#define SPI_OFF_MAGIC    0
#define SPI_OFF_TYPE     1
#define SPI_OFF_LEN_LO   2
#define SPI_OFF_LEN_HI   3
#define SPI_OFF_PAYLOAD  4
#define SPI_OFF_CRC      516

#define FRAME_SMP        0x06
#define FRAME_READ_REQ   0x04
#define FRAME_NOOP       0x05

#define SMP_HDR_SIZE     8
#define SMP_OP_WRITE     2
#define SMP_OP_READ      0
#define SMP_GROUP_OS     0
#define SMP_GROUP_IMG    1
#define SMP_CMD_IMG_STATE  0
#define SMP_CMD_IMG_UPLOAD 1
#define SMP_CMD_OS_RESET   5

/* ------------------------------------------------------------------ */
/* Public test-side state                                             */
/* ------------------------------------------------------------------ */
struct fake_perf      g_perf;
struct fake_transport g_xport;

static struct ext_dfu_status g_ext_status = {
	.state         = EXT_DFU_STATE_IDLE,
	.bytes_written = 0,
};

void fake_set_image_available(size_t size)
{
	g_ext_status.state         = EXT_DFU_STATE_DONE;
	g_ext_status.bytes_written = size;
}

void spi_dfu_stubs_reset(void)
{
	memset(&g_perf, 0, sizeof(g_perf));
	memset(&g_xport, 0, sizeof(g_xport));
	g_ext_status.state         = EXT_DFU_STATE_IDLE;
	g_ext_status.bytes_written = 0;
}

/* ------------------------------------------------------------------ */
/* ext_dfu stubs                                                      */
/* ------------------------------------------------------------------ */
int ext_dfu_get_status(enum ext_dfu_target target, struct ext_dfu_status *st)
{
	(void)target;
	if (!st) {
		return -EINVAL;
	}
	*st = g_ext_status;
	return 0;
}

int ext_dfu_start(enum ext_dfu_target t, const char *u, int s)
{
	(void)t; (void)u; (void)s; return 0;
}
int ext_dfu_cancel(void)                       { return -EALREADY; }
int ext_dfu_erase(enum ext_dfu_target t)       { (void)t; return 0; }

/* ------------------------------------------------------------------ */
/* flash_area stubs                                                   */
/* ------------------------------------------------------------------ */
static struct flash_area g_fake_fa = {
	.fa_id = 1, .fa_off = 0, .fa_size = 1024 * 1024,
};

int flash_area_open(uint8_t id, const struct flash_area **fa)
{
	(void)id; *fa = &g_fake_fa; return 0;
}
void flash_area_close(const struct flash_area *fa) { (void)fa; }

int flash_area_read(const struct flash_area *fa, off_t off,
		    void *dst, size_t len)
{
	(void)fa;
	uint8_t *d = dst;
	for (size_t i = 0; i < len; i++) {
		d[i] = (uint8_t)((off + i) & 0xFF);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Fake SPI transport                                                 */
/* ------------------------------------------------------------------ */
static uint8_t g_resp[SPI_FRAME_SIZE];
static bool    g_resp_pending;

static uint8_t crc8_frame(const uint8_t *buf, size_t len)
{
	return crc8(buf, len, 0x07, 0x00, false);
}

static void make_resp_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
	memset(g_resp, 0, SPI_FRAME_SIZE);
	g_resp[SPI_OFF_MAGIC]  = SPI_MAGIC;
	g_resp[SPI_OFF_TYPE]   = type;
	g_resp[SPI_OFF_LEN_LO] = (uint8_t)(len & 0xFF);
	g_resp[SPI_OFF_LEN_HI] = (uint8_t)(len >> 8);
	if (payload && len) {
		memcpy(&g_resp[SPI_OFF_PAYLOAD], payload, len);
	}
	g_resp[SPI_OFF_CRC] = crc8_frame(g_resp, SPI_OFF_CRC);
	g_resp_pending = true;
}

/* ---- minimal CBOR encoders (returns full head byte including major type) -- */
static int cbor_put_uint_full(uint8_t *buf, size_t max, uint8_t major,
			      uint32_t val)
{
	if (val < 24) {
		if (max < 1) return -1;
		buf[0] = (uint8_t)((major << 5) | val);
		return 1;
	}
	if (val <= 0xFF) {
		if (max < 2) return -1;
		buf[0] = (uint8_t)((major << 5) | 24);
		buf[1] = (uint8_t)val;
		return 2;
	}
	if (val <= 0xFFFF) {
		if (max < 3) return -1;
		buf[0] = (uint8_t)((major << 5) | 25);
		buf[1] = (uint8_t)(val >> 8);
		buf[2] = (uint8_t)(val & 0xFF);
		return 3;
	}
	if (max < 5) return -1;
	buf[0] = (uint8_t)((major << 5) | 26);
	buf[1] = (uint8_t)(val >> 24);
	buf[2] = (uint8_t)(val >> 16);
	buf[3] = (uint8_t)(val >> 8);
	buf[4] = (uint8_t)(val & 0xFF);
	return 5;
}

static int build_upload_rsp(uint8_t *buf, size_t max, uint32_t off)
{
	int pos = 0;

	if (max < 1) return -1;
	buf[pos++] = (5 << 5) | 2;            /* map(2) */
	if (max - pos < 4) return -1;
	buf[pos++] = (3 << 5) | 2;            /* tstr "rc" */
	buf[pos++] = 'r'; buf[pos++] = 'c';
	int n = cbor_put_uint_full(buf + pos, max - pos, 0, 0);
	if (n < 0) return -1;
	pos += n;
	if (max - pos < 5) return -1;
	buf[pos++] = (3 << 5) | 3;            /* tstr "off" */
	buf[pos++] = 'o'; buf[pos++] = 'f'; buf[pos++] = 'f';
	n = cbor_put_uint_full(buf + pos, max - pos, 0, off);
	if (n < 0) return -1;
	pos += n;
	return pos;
}

static int build_image_list_rsp(uint8_t *buf, size_t max)
{
	int pos = 0;
	const uint8_t fake_hash[32] = {
		0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
		0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,
		0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
		0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,
	};

	if (max < 64) return -1;
	buf[pos++] = (5 << 5) | 1;            /* map(1) */
	buf[pos++] = (3 << 5) | 6;            /* tstr "images" */
	memcpy(&buf[pos], "images", 6); pos += 6;
	buf[pos++] = (4 << 5) | 1;            /* array(1) */
	buf[pos++] = (5 << 5) | 2;            /* map(2) */
	buf[pos++] = (3 << 5) | 4;
	memcpy(&buf[pos], "slot", 4); pos += 4;
	buf[pos++] = (0 << 5) | 1;            /* uint 1 */
	buf[pos++] = (3 << 5) | 4;
	memcpy(&buf[pos], "hash", 4); pos += 4;
	buf[pos++] = (2 << 5) | 24;           /* bstr len byte */
	buf[pos++] = 32;
	memcpy(&buf[pos], fake_hash, 32); pos += 32;
	return pos;
}

static int build_simple_rc_rsp(uint8_t *buf, size_t max, int rc)
{
	int pos = 0;

	if (max < 6) return -1;
	buf[pos++] = (5 << 5) | 1;            /* map(1) */
	buf[pos++] = (3 << 5) | 2;
	buf[pos++] = 'r'; buf[pos++] = 'c';
	uint8_t major = (rc >= 0) ? 0 : 1;
	uint32_t val  = (rc >= 0) ? (uint32_t)rc : (uint32_t)(-1 - rc);
	int n = cbor_put_uint_full(buf + pos, max - pos, major, val);
	if (n < 0) return -1;
	pos += n;
	return pos;
}

static void handle_smp_request(const uint8_t *frame)
{
	uint16_t len = (uint16_t)frame[SPI_OFF_LEN_LO] |
		       ((uint16_t)frame[SPI_OFF_LEN_HI] << 8);

	if (len < SMP_HDR_SIZE || len > SPI_PAYLOAD_MAX) {
		return;
	}

	const uint8_t *smp = &frame[SPI_OFF_PAYLOAD];
	uint8_t  op    = smp[0] & 0x07;
	uint16_t group = ((uint16_t)smp[4] << 8) | smp[5];
	uint8_t  cmd   = smp[7];

	uint8_t  smp_resp[SPI_PAYLOAD_MAX];
	uint8_t  cbor_buf[SPI_PAYLOAD_MAX - SMP_HDR_SIZE];
	int      cbor_len = -1;

	if (group == SMP_GROUP_IMG && cmd == SMP_CMD_IMG_UPLOAD &&
	    op == SMP_OP_WRITE) {

		if (g_xport.fail_after_upload_chunks &&
		    g_xport.upload_chunks_seen >= g_xport.fail_after_upload_chunks) {
			g_xport.timeouts_injected++;
			g_resp_pending = false;
			return;
		}

		size_t remaining = (g_xport.image_size > g_xport.upload_offset)
			? g_xport.image_size - g_xport.upload_offset : 0;
		size_t advance = remaining > g_xport.chunk_size
				 ? g_xport.chunk_size : remaining;

		g_xport.upload_offset      += advance;
		g_xport.upload_chunks_seen += 1;
		g_xport.bytes_acknowledged += advance;

		cbor_len = build_upload_rsp(cbor_buf, sizeof(cbor_buf),
					    (uint32_t)g_xport.upload_offset);
	} else if (group == SMP_GROUP_IMG && cmd == SMP_CMD_IMG_STATE &&
		   op == SMP_OP_READ) {
		cbor_len = build_image_list_rsp(cbor_buf, sizeof(cbor_buf));
		g_xport.image_list_requests++;
	} else if (group == SMP_GROUP_IMG && cmd == SMP_CMD_IMG_STATE &&
		   op == SMP_OP_WRITE) {
		cbor_len = build_simple_rc_rsp(cbor_buf, sizeof(cbor_buf), 0);
		g_xport.image_test_requests++;
	} else if (group == SMP_GROUP_OS && cmd == SMP_CMD_OS_RESET) {
		g_xport.reset_requests++;
		if (g_xport.reset_no_response) {
			g_resp_pending = false;
			return;
		}
		cbor_len = build_simple_rc_rsp(cbor_buf, sizeof(cbor_buf), 0);
	} else {
		cbor_len = build_simple_rc_rsp(cbor_buf, sizeof(cbor_buf), -1);
	}

	if (cbor_len < 0) {
		g_resp_pending = false;
		return;
	}

	smp_resp[0] = ((op + 1) & 0x07) | (1 << 3);
	smp_resp[1] = 0x00;
	smp_resp[2] = (uint8_t)(cbor_len >> 8);
	smp_resp[3] = (uint8_t)(cbor_len & 0xFF);
	smp_resp[4] = (uint8_t)(group >> 8);
	smp_resp[5] = (uint8_t)(group & 0xFF);
	smp_resp[6] = smp[6];
	smp_resp[7] = cmd;
	memcpy(&smp_resp[SMP_HDR_SIZE], cbor_buf, cbor_len);

	make_resp_frame(FRAME_SMP, smp_resp,
			(uint16_t)(SMP_HDR_SIZE + cbor_len));
}

/* ---- transport API consumed by spi_dfu.c ---- */
int uart_sensor_spi_lock(k_timeout_t timeout)
{
	(void)timeout; g_perf.lock_calls++; return 0;
}
void uart_sensor_spi_unlock(void) { g_perf.unlock_calls++; }

int uart_sensor_spi_xfer_locked(const uint8_t *tx, uint8_t *rx)
{
	uint64_t t0 = k_cycle_get_64();

	g_perf.xfer_calls++;
	uint8_t type = tx[SPI_OFF_TYPE];

	if (type == FRAME_SMP) {
		g_perf.smp_frames_in++;
		handle_smp_request(tx);
	}

	if (g_resp_pending) {
		memcpy(rx, g_resp, SPI_FRAME_SIZE);
		if (type == FRAME_READ_REQ) {
			g_resp_pending = false;
		}
	} else {
		memset(rx, 0, SPI_FRAME_SIZE);
		rx[SPI_OFF_MAGIC] = SPI_MAGIC;
		rx[SPI_OFF_TYPE]  = FRAME_NOOP;
		rx[SPI_OFF_CRC]   = crc8_frame(rx, SPI_OFF_CRC);
	}

	g_perf.xfer_cycles += k_cycle_get_64() - t0;
	return 0;
}

bool uart_sensor_spi_drdy_active(void) { return g_resp_pending; }

int uart_sensor_spi_send_smp_fwd(const uint8_t *p, uint16_t len)
{
	(void)p; (void)len; return 0;
}

void uart_sensor_spi_submit_work(struct k_work *w) { (void)w; }
