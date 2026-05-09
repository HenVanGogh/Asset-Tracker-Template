/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * SPI DFU Transport — MCUmgr SMP client sending firmware to nRF5340
 * over the existing SPI inter-chip link (FRAME_SMP = 0x06).
 *
 * Protocol reference: SPI_DFU_TRANSPORT.md
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <pm_config.h>
#include <string.h>

#include "spi_dfu.h"
#include "uart_sensor.h"
#include "ext_dfu.h"

LOG_MODULE_REGISTER(spi_dfu, CONFIG_APP_SPI_DFU_LOG_LEVEL);

/* ── SPI Frame Protocol (matches uart_sensor.c) ──────────────────── */

#define SPI_FRAME_SIZE      520
#define SPI_PAYLOAD_MAX     512
#define SPI_MAGIC           0xA5
#define SPI_OFF_MAGIC       0
#define SPI_OFF_TYPE        1
#define SPI_OFF_LEN_LO      2
#define SPI_OFF_LEN_HI      3
#define SPI_OFF_PAYLOAD     4
#define SPI_OFF_CRC         516

#define FRAME_SMP           0x06
#define FRAME_NOOP          0x05
#define FRAME_NOTIFY        0x03
#define FRAME_READ_REQ      0x04

/* ── SMP Protocol ─────────────────────────────────────────────────── */

#define SMP_HDR_SIZE        8
#define SMP_OP_READ         0
#define SMP_OP_READ_RSP     1
#define SMP_OP_WRITE        2
#define SMP_OP_WRITE_RSP    3

#define SMP_GROUP_OS        0
#define SMP_GROUP_IMG       1
#define SMP_CMD_IMG_STATE   0   /* image list / test / confirm */
#define SMP_CMD_IMG_UPLOAD  1
#define SMP_CMD_OS_RESET    5

#define SMP_VERSION         1

/* ── DFU Tuning ───────────────────────────────────────────────────── */

#define DFU_CHUNK_SIZE      256
#define DFU_SMP_TIMEOUT_MS  30000
#define DFU_RETRY_COUNT     3
#define DFU_POLL_INTERVAL_MS 10

/* ── CBOR Major Types ─────────────────────────────────────────────── */

#define CBOR_UINT   0
#define CBOR_NEGINT 1
#define CBOR_BSTR   2
#define CBOR_TSTR   3
#define CBOR_ARRAY  4
#define CBOR_MAP    5
#define CBOR_SIMPLE 7

/* ── Module State ─────────────────────────────────────────────────── */

static struct {
	enum spi_dfu_state state;
	size_t             bytes_uploaded;
	size_t             image_size;
	int                error_code;
	char               error_msg[64];
	struct k_mutex     lock;
	bool               cancel_requested;
	uint8_t            seq;
} dfu_ctx;

static K_SEM_DEFINE(dfu_start_sem, 0, 1);

/* ── Thread ───────────────────────────────────────────────────────── */

#define DFU_THREAD_STACK_SIZE 4096
#define DFU_THREAD_PRIORITY   7

static K_THREAD_STACK_DEFINE(dfu_thread_stack, DFU_THREAD_STACK_SIZE);
static struct k_thread dfu_thread;

/* ── Working Buffers ──────────────────────────────────────────────── */

static uint8_t tx_frame[SPI_FRAME_SIZE];
static uint8_t rx_frame[SPI_FRAME_SIZE];
static uint8_t smp_buf[SPI_PAYLOAD_MAX];
static uint8_t flash_chunk[DFU_CHUNK_SIZE];

/* Response buffer — static to keep DFU thread stack usage low */
static uint8_t smp_resp[SPI_PAYLOAD_MAX];

/* =====================================================================
 * SPI Frame Helpers
 * ===================================================================== */

static void frame_build(uint8_t *buf, uint8_t type,
			const uint8_t *payload, uint16_t len)
{
	memset(buf, 0, SPI_FRAME_SIZE);
	buf[SPI_OFF_MAGIC]  = SPI_MAGIC;
	buf[SPI_OFF_TYPE]   = type;
	buf[SPI_OFF_LEN_LO] = (uint8_t)(len & 0xFF);
	buf[SPI_OFF_LEN_HI] = (uint8_t)(len >> 8);
	if (payload && len > 0) {
		memcpy(&buf[SPI_OFF_PAYLOAD], payload,
		       MIN(len, (uint16_t)SPI_PAYLOAD_MAX));
	}
	buf[SPI_OFF_CRC] = crc8(buf, SPI_OFF_CRC, 0x07, 0x00, false);
}

static bool frame_valid(const uint8_t *buf)
{
	if (buf[SPI_OFF_MAGIC] != SPI_MAGIC) {
		return false;
	}
	return buf[SPI_OFF_CRC] == crc8(buf, SPI_OFF_CRC, 0x07, 0x00, false);
}

/* =====================================================================
 * SMP Header Builder
 * ===================================================================== */

static void smp_header_build(uint8_t *out, uint8_t op, uint16_t group,
			     uint8_t seq, uint8_t cmd_id, uint16_t cbor_len)
{
	out[0] = (op & 0x07) | ((SMP_VERSION & 0x03) << 3);
	out[1] = 0x00;                          /* flags */
	out[2] = (uint8_t)(cbor_len >> 8);      /* length MSB */
	out[3] = (uint8_t)(cbor_len & 0xFF);    /* length LSB */
	out[4] = (uint8_t)(group >> 8);         /* group MSB */
	out[5] = (uint8_t)(group & 0xFF);       /* group LSB */
	out[6] = seq;
	out[7] = cmd_id;
}

/* =====================================================================
 * Manual CBOR Encoding Helpers
 * ===================================================================== */

static int cbor_put_uint(uint8_t *buf, size_t max, uint32_t val)
{
	if (val < 24) {
		if (max < 1) {
			return -1;
		}
		buf[0] = (uint8_t)((CBOR_UINT << 5) | val);
		return 1;
	} else if (val <= 0xFF) {
		if (max < 2) {
			return -1;
		}
		buf[0] = (CBOR_UINT << 5) | 24;
		buf[1] = (uint8_t)val;
		return 2;
	} else if (val <= 0xFFFF) {
		if (max < 3) {
			return -1;
		}
		buf[0] = (CBOR_UINT << 5) | 25;
		buf[1] = (uint8_t)(val >> 8);
		buf[2] = (uint8_t)(val & 0xFF);
		return 3;
	} else {
		if (max < 5) {
			return -1;
		}
		buf[0] = (CBOR_UINT << 5) | 26;
		buf[1] = (uint8_t)(val >> 24);
		buf[2] = (uint8_t)(val >> 16);
		buf[3] = (uint8_t)(val >> 8);
		buf[4] = (uint8_t)(val & 0xFF);
		return 5;
	}
}

static int cbor_put_tstr(uint8_t *buf, size_t max, const char *str, size_t len)
{
	int hdr;

	if (len < 24) {
		if (max < 1 + len) {
			return -1;
		}
		buf[0] = (uint8_t)((CBOR_TSTR << 5) | len);
		hdr = 1;
	} else if (len <= 0xFF) {
		if (max < 2 + len) {
			return -1;
		}
		buf[0] = (CBOR_TSTR << 5) | 24;
		buf[1] = (uint8_t)len;
		hdr = 2;
	} else {
		return -1;
	}
	memcpy(buf + hdr, str, len);
	return hdr + (int)len;
}

static int cbor_put_bstr(uint8_t *buf, size_t max,
			 const uint8_t *data, size_t len)
{
	int hdr;

	if (len < 24) {
		if (max < 1 + len) {
			return -1;
		}
		buf[0] = (uint8_t)((CBOR_BSTR << 5) | len);
		hdr = 1;
	} else if (len <= 0xFF) {
		if (max < 2 + len) {
			return -1;
		}
		buf[0] = (CBOR_BSTR << 5) | 24;
		buf[1] = (uint8_t)len;
		hdr = 2;
	} else if (len <= 0xFFFF) {
		if (max < 3 + len) {
			return -1;
		}
		buf[0] = (CBOR_BSTR << 5) | 25;
		buf[1] = (uint8_t)(len >> 8);
		buf[2] = (uint8_t)(len & 0xFF);
		hdr = 3;
	} else {
		return -1;
	}
	memcpy(buf + hdr, data, len);
	return hdr + (int)len;
}

static int cbor_put_map_hdr(uint8_t *buf, size_t max, uint8_t count)
{
	if (max < 1) {
		return -1;
	}
	buf[0] = (uint8_t)((CBOR_MAP << 5) | count);
	return 1;
}

static int cbor_put_bool_false(uint8_t *buf, size_t max)
{
	if (max < 1) {
		return -1;
	}
	buf[0] = 0xF4;
	return 1;
}

/* =====================================================================
 * CBOR Encoding — SMP Payloads
 * ===================================================================== */

/**
 * Encode an image upload chunk.
 * First chunk: {"image":0, "len":total, "off":0, "data":<bytes>}
 * Subsequent:  {"off":<offset>, "data":<bytes>}
 */
static int encode_upload(uint8_t *buf, size_t buf_size,
			 uint32_t offset, uint32_t total_len,
			 const uint8_t *data, uint16_t data_len,
			 bool first_chunk)
{
	int pos = 0;
	int n;
	uint8_t map_count = first_chunk ? 4 : 2;

	n = cbor_put_map_hdr(buf + pos, buf_size - pos, map_count);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;

	if (first_chunk) {
		/* "image": 0 */
		n = cbor_put_tstr(buf + pos, buf_size - pos, "image", 5);
		if (n < 0) {
			return -EINVAL;
		}
		pos += n;
		n = cbor_put_uint(buf + pos, buf_size - pos, 0);
		if (n < 0) {
			return -EINVAL;
		}
		pos += n;

		/* "len": total_len */
		n = cbor_put_tstr(buf + pos, buf_size - pos, "len", 3);
		if (n < 0) {
			return -EINVAL;
		}
		pos += n;
		n = cbor_put_uint(buf + pos, buf_size - pos, total_len);
		if (n < 0) {
			return -EINVAL;
		}
		pos += n;
	}

	/* "off": offset */
	n = cbor_put_tstr(buf + pos, buf_size - pos, "off", 3);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;
	n = cbor_put_uint(buf + pos, buf_size - pos, offset);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;

	/* "data": bstr(data) */
	n = cbor_put_tstr(buf + pos, buf_size - pos, "data", 4);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;
	n = cbor_put_bstr(buf + pos, buf_size - pos, data, data_len);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;

	return pos;
}

/**
 * Encode image test request: {"hash":<32bytes>, "confirm":false}
 */
static int encode_image_test(uint8_t *buf, size_t buf_size,
			     const uint8_t *hash, size_t hash_len)
{
	int pos = 0;
	int n;

	n = cbor_put_map_hdr(buf + pos, buf_size - pos, 2);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;

	n = cbor_put_tstr(buf + pos, buf_size - pos, "hash", 4);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;
	n = cbor_put_bstr(buf + pos, buf_size - pos, hash, hash_len);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;

	n = cbor_put_tstr(buf + pos, buf_size - pos, "confirm", 7);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;
	n = cbor_put_bool_false(buf + pos, buf_size - pos);
	if (n < 0) {
		return -EINVAL;
	}
	pos += n;

	return pos;
}

/**
 * Encode empty map: 0xA0
 */
static int encode_empty_map(uint8_t *buf, size_t buf_size)
{
	if (buf_size < 1) {
		return -EINVAL;
	}
	buf[0] = 0xA0;
	return 1;
}

/* =====================================================================
 * Manual CBOR Decoding
 * ===================================================================== */

struct cbor_reader {
	const uint8_t *buf;
	size_t         len;
	size_t         pos;
};

static int cbor_read_head(struct cbor_reader *r, uint8_t *major, uint32_t *val)
{
	if (r->pos >= r->len) {
		return -1;
	}

	uint8_t b = r->buf[r->pos++];

	*major = b >> 5;
	uint8_t ai = b & 0x1F;

	if (ai < 24) {
		*val = ai;
	} else if (ai == 24) {
		if (r->pos >= r->len) {
			return -1;
		}
		*val = r->buf[r->pos++];
	} else if (ai == 25) {
		if (r->pos + 2 > r->len) {
			return -1;
		}
		*val = ((uint32_t)r->buf[r->pos] << 8) | r->buf[r->pos + 1];
		r->pos += 2;
	} else if (ai == 26) {
		if (r->pos + 4 > r->len) {
			return -1;
		}
		*val = ((uint32_t)r->buf[r->pos] << 24) |
		       ((uint32_t)r->buf[r->pos + 1] << 16) |
		       ((uint32_t)r->buf[r->pos + 2] << 8) |
		       r->buf[r->pos + 3];
		r->pos += 4;
	} else {
		return -1; /* 8-byte or indefinite — not expected */
	}
	return 0;
}

/**
 * Skip one CBOR element (recursively for containers).
 */
static int cbor_skip(struct cbor_reader *r)
{
	uint8_t major;
	uint32_t val;

	if (cbor_read_head(r, &major, &val)) {
		return -1;
	}

	switch (major) {
	case CBOR_UINT:
	case CBOR_NEGINT:
	case CBOR_SIMPLE:
		return 0;
	case CBOR_BSTR:
	case CBOR_TSTR:
		if (r->pos + val > r->len) {
			return -1;
		}
		r->pos += val;
		return 0;
	case CBOR_ARRAY:
		for (uint32_t i = 0; i < val; i++) {
			if (cbor_skip(r)) {
				return -1;
			}
		}
		return 0;
	case CBOR_MAP:
		for (uint32_t i = 0; i < val; i++) {
			if (cbor_skip(r)) {
				return -1;  /* key */
			}
			if (cbor_skip(r)) {
				return -1;  /* value */
			}
		}
		return 0;
	default:
		return -1;
	}
}

/**
 * Decode upload response: {"rc": <int>, "off": <uint>}
 */
static int decode_upload_response(const uint8_t *cbor, size_t len,
				  int *rc_out, uint32_t *off_out)
{
	struct cbor_reader r = { .buf = cbor, .len = len, .pos = 0 };
	uint8_t major;
	uint32_t val;

	*rc_out = -1;
	*off_out = 0;

	/* Map header */
	if (cbor_read_head(&r, &major, &val) || major != CBOR_MAP) {
		return -EINVAL;
	}
	uint32_t map_count = val;

	for (uint32_t i = 0; i < map_count; i++) {
		/* Read key (text string) */
		uint8_t km;
		uint32_t klen;

		if (cbor_read_head(&r, &km, &klen)) {
			return -EINVAL;
		}
		if (km != CBOR_TSTR) {
			/* Non-string key: head already consumed the encoding,
			 * only advance pos for bstr/tstr data bytes. */
			if (km == CBOR_BSTR || km == CBOR_TSTR) {
				r.pos += klen;
			}
			if (cbor_skip(&r)) {
				return -EINVAL;
			}
			continue;
		}
		if (r.pos + klen > r.len) {
			return -EINVAL;
		}

		const uint8_t *key = r.buf + r.pos;

		r.pos += klen;

		if (klen == 2 && memcmp(key, "rc", 2) == 0) {
			uint8_t vm;
			uint32_t vv;

			if (cbor_read_head(&r, &vm, &vv)) {
				return -EINVAL;
			}
			if (vm == CBOR_UINT) {
				*rc_out = (int)vv;
			} else if (vm == CBOR_NEGINT) {
				*rc_out = -1 - (int)vv;
			} else {
				return -EINVAL;
			}
		} else if (klen == 3 && memcmp(key, "off", 3) == 0) {
			uint8_t vm;
			uint32_t vv;

			if (cbor_read_head(&r, &vm, &vv) || vm != CBOR_UINT) {
				return -EINVAL;
			}
			*off_out = vv;
		} else {
			if (cbor_skip(&r)) {
				return -EINVAL;
			}
		}
	}

	return 0;
}

/**
 * Extract the SHA-256 hash of the image in slot 1 from an image list response.
 *
 * Response format:
 * {"images":[{"slot":0,...},{"slot":1,"hash":<32 bytes>,...}]}
 */
static int decode_image_list_hash(const uint8_t *cbor, size_t len,
				  uint8_t *hash_out, size_t hash_size)
{
	struct cbor_reader r = { .buf = cbor, .len = len, .pos = 0 };
	uint8_t major;
	uint32_t val;

	/* Top-level map */
	if (cbor_read_head(&r, &major, &val) || major != CBOR_MAP) {
		return -EINVAL;
	}
	uint32_t top_count = val;

	for (uint32_t t = 0; t < top_count; t++) {
		/* Key */
		uint8_t km;
		uint32_t klen;

		if (cbor_read_head(&r, &km, &klen) || km != CBOR_TSTR) {
			return -EINVAL;
		}
		if (r.pos + klen > r.len) {
			return -EINVAL;
		}

		const uint8_t *key = r.buf + r.pos;

		r.pos += klen;

		if (klen == 6 && memcmp(key, "images", 6) == 0) {
			/* Array of image entries */
			if (cbor_read_head(&r, &major, &val) || major != CBOR_ARRAY) {
				return -EINVAL;
			}
			uint32_t arr_count = val;

			for (uint32_t a = 0; a < arr_count; a++) {
				/* Each element is a map */
				if (cbor_read_head(&r, &major, &val) ||
				    major != CBOR_MAP) {
					return -EINVAL;
				}
				uint32_t img_count = val;
				int32_t slot = -1;
				const uint8_t *hash_ptr = NULL;
				size_t hash_len = 0;

				for (uint32_t im = 0; im < img_count; im++) {
					uint8_t ikm;
					uint32_t iklen;

					if (cbor_read_head(&r, &ikm, &iklen) ||
					    ikm != CBOR_TSTR) {
						/* Non-string key — skip key data + value */
						if (ikm == CBOR_TSTR || ikm == CBOR_BSTR) {
							r.pos += iklen;
						}
						cbor_skip(&r);
						continue;
					}
					if (r.pos + iklen > r.len) {
						return -EINVAL;
					}

					const uint8_t *ikey = r.buf + r.pos;

					r.pos += iklen;

					if (iklen == 4 &&
					    memcmp(ikey, "slot", 4) == 0) {
						uint8_t vm;
						uint32_t vv;

						if (cbor_read_head(&r, &vm, &vv) ||
						    vm != CBOR_UINT) {
							return -EINVAL;
						}
						slot = (int32_t)vv;
					} else if (iklen == 4 &&
						   memcmp(ikey, "hash", 4) == 0) {
						uint8_t vm;
						uint32_t vv;

						if (cbor_read_head(&r, &vm, &vv) ||
						    vm != CBOR_BSTR) {
							return -EINVAL;
						}
						if (r.pos + vv > r.len) {
							return -EINVAL;
						}
						hash_ptr = r.buf + r.pos;
						hash_len = vv;
						r.pos += vv;
					} else {
						if (cbor_skip(&r)) {
							return -EINVAL;
						}
					}
				}

				/* Slot 1 = secondary (newly uploaded) */
				if (slot == 1 && hash_ptr &&
				    hash_len >= hash_size) {
					memcpy(hash_out, hash_ptr, hash_size);
					return 0;
				}
			}
			/* If we get here, no slot 1 with hash found */
			return -ENOENT;
		} else {
			if (cbor_skip(&r)) {
				return -EINVAL;
			}
		}
	}

	return -ENOENT;
}

/**
 * Decode a simple SMP response: {"rc": <value>}
 * Returns the rc value, or -1 on parse error.
 */
static int decode_rc_response(const uint8_t *cbor, size_t len)
{
	int rc;
	uint32_t off;

	if (decode_upload_response(cbor, len, &rc, &off) == 0) {
		return rc;
	}
	return -1;
}

/* =====================================================================
 * SPI SMP Transaction
 * ===================================================================== */

/**
 * Send an SMP frame and wait for the SMP response.
 *
 * Holds the SPI bus lock for the entire exchange (send + poll).
 *
 * @param smp_pkt     SMP packet (header + CBOR)
 * @param smp_len     Length of SMP packet
 * @param resp        Output buffer for SMP response
 * @param resp_len    Output: response length
 * @param timeout_ms  Max time to wait
 * @return 0 on success, negative errno on failure
 */
static int spi_smp_exchange(const uint8_t *smp_pkt, uint16_t smp_len,
			    uint8_t *resp, uint16_t *resp_len,
			    int timeout_ms)
{
	int ret;
	int64_t deadline = k_uptime_get() + timeout_ms;

	ret = uart_sensor_spi_lock(K_MSEC(5000));
	if (ret) {
		LOG_ERR("SPI DFU: bus lock timeout");
		return -EBUSY;
	}

	/* Build and send FRAME_SMP */
	frame_build(tx_frame, FRAME_SMP, smp_pkt, smp_len);
	ret = uart_sensor_spi_xfer_locked(tx_frame, rx_frame);
	if (ret) {
		uart_sensor_spi_unlock();
		LOG_ERR("SPI SMP send failed: %d", ret);
		return ret;
	}

	/* Check simultaneous RX — might already be an SMP response */
	if (frame_valid(rx_frame) && rx_frame[SPI_OFF_TYPE] == FRAME_SMP) {
		uint16_t len = (uint16_t)rx_frame[SPI_OFF_LEN_LO] |
			       ((uint16_t)rx_frame[SPI_OFF_LEN_HI] << 8);
		if (len > 0 && len <= SPI_PAYLOAD_MAX) {
			memcpy(resp, &rx_frame[SPI_OFF_PAYLOAD], len);
			*resp_len = len;
			uart_sensor_spi_unlock();
			return 0;
		}
	}

	/* Poll for response */
	while (k_uptime_get() < deadline) {
		/* Wait for DATA_READY */
		while (!uart_sensor_spi_drdy_active() &&
		       k_uptime_get() < deadline) {
			k_msleep(DFU_POLL_INTERVAL_MS);
		}

		if (k_uptime_get() >= deadline) {
			break;
		}

		/* Send READ_REQ to fetch response */
		frame_build(tx_frame, FRAME_READ_REQ, NULL, 0);
		ret = uart_sensor_spi_xfer_locked(tx_frame, rx_frame);
		if (ret) {
			uart_sensor_spi_unlock();
			LOG_ERR("SPI READ_REQ failed: %d", ret);
			return ret;
		}

		if (!frame_valid(rx_frame)) {
			k_msleep(DFU_POLL_INTERVAL_MS);
			continue;
		}

		uint8_t type = rx_frame[SPI_OFF_TYPE];
		uint16_t len = (uint16_t)rx_frame[SPI_OFF_LEN_LO] |
			       ((uint16_t)rx_frame[SPI_OFF_LEN_HI] << 8);

		if (type == FRAME_SMP && len > 0 && len <= SPI_PAYLOAD_MAX) {
			memcpy(resp, &rx_frame[SPI_OFF_PAYLOAD], len);
			*resp_len = len;
			uart_sensor_spi_unlock();
			return 0;
		}

		if (type == FRAME_NOTIFY && len > 0) {
			char nb[64];
			size_t copy = MIN(len, sizeof(nb) - 1);

			memcpy(nb, &rx_frame[SPI_OFF_PAYLOAD], copy);
			nb[copy] = '\0';
			LOG_INF("SPI DFU: NOTIFY during poll: %s", nb);
			continue;
		}

		/* NOOP or other — keep polling */
		k_msleep(DFU_POLL_INTERVAL_MS);
	}

	uart_sensor_spi_unlock();
	return -ETIMEDOUT;
}

/* =====================================================================
 * DFU Steps
 * ===================================================================== */

static int dfu_upload_image(const struct flash_area *fa, size_t image_size)
{
	uint32_t offset = 0;
	uint16_t resp_len;
	int rc;

	LOG_INF("SPI DFU: Uploading %u bytes to nRF5340",
		(unsigned int)image_size);

	while (offset < image_size) {
		if (dfu_ctx.cancel_requested) {
			return -ECANCELED;
		}

		uint16_t chunk_len = (uint16_t)MIN(DFU_CHUNK_SIZE,
						    image_size - offset);
		bool first = (offset == 0);

		/* Read chunk from external flash */
		rc = flash_area_read(fa, offset, flash_chunk, chunk_len);
		if (rc) {
			LOG_ERR("flash_area_read at 0x%x: %d", offset, rc);
			return rc;
		}

		/* Encode CBOR payload */
		int cbor_len = encode_upload(smp_buf + SMP_HDR_SIZE,
					     sizeof(smp_buf) - SMP_HDR_SIZE,
					     offset, (uint32_t)image_size,
					     flash_chunk, chunk_len, first);
		if (cbor_len < 0) {
			LOG_ERR("CBOR encode failed: %d", cbor_len);
			return cbor_len;
		}

		/* Build SMP header */
		smp_header_build(smp_buf, SMP_OP_WRITE, SMP_GROUP_IMG,
				 dfu_ctx.seq++, SMP_CMD_IMG_UPLOAD,
				 (uint16_t)cbor_len);

		/* Send and wait for response (with retries) */
		int retries = DFU_RETRY_COUNT;
		int smp_rc = -1;
		uint32_t next_off = 0;

		while (retries-- > 0) {
			rc = spi_smp_exchange(smp_buf,
					      SMP_HDR_SIZE + cbor_len,
					      smp_resp, &resp_len,
					      DFU_SMP_TIMEOUT_MS);
			if (rc) {
				LOG_WRN("SMP exchange failed at %u: %d (retry=%d)",
					offset, rc, retries);
				continue;
			}

			if (resp_len < SMP_HDR_SIZE) {
				LOG_WRN("Short SMP response: %u bytes", resp_len);
				continue;
			}

			/* Decode CBOR (skip 8-byte SMP header) */
			rc = decode_upload_response(smp_resp + SMP_HDR_SIZE,
						    resp_len - SMP_HDR_SIZE,
						    &smp_rc, &next_off);
			if (rc) {
				LOG_WRN("CBOR decode failed: %d", rc);
				continue;
			}

			if (smp_rc == 0) {
				break;
			}

			LOG_WRN("Upload rc=%d at %u (retry=%d)",
				smp_rc, offset, retries);
		}

		if (smp_rc != 0) {
			LOG_ERR("Upload failed at offset %u: rc=%d",
				offset, smp_rc);
			return -EIO;
		}

		/* Guard against stall: server must advance offset */
		if (next_off <= offset) {
			LOG_ERR("SPI DFU: no forward progress "
				"(off=%u, expected >%u)",
				(unsigned int)next_off,
				(unsigned int)offset);
			return -EIO;
		}
		offset = next_off;

		/* Update progress */
		k_mutex_lock(&dfu_ctx.lock, K_FOREVER);
		dfu_ctx.bytes_uploaded = offset;
		k_mutex_unlock(&dfu_ctx.lock);

		if (offset % (64 * DFU_CHUNK_SIZE) == 0 ||
		    offset >= image_size) {
			LOG_INF("SPI DFU: %u / %u (%u%%)",
				(unsigned int)offset,
				(unsigned int)image_size,
				(unsigned int)((offset * 100) / image_size));
		}
	}

	LOG_INF("SPI DFU: Upload complete (%u bytes)",
		(unsigned int)image_size);
	return 0;
}

static int dfu_image_list(uint8_t *hash_out)
{
	uint16_t resp_len;
	int rc;

	LOG_INF("SPI DFU: Requesting image list");

	int cbor_len = encode_empty_map(smp_buf + SMP_HDR_SIZE,
					sizeof(smp_buf) - SMP_HDR_SIZE);
	smp_header_build(smp_buf, SMP_OP_READ, SMP_GROUP_IMG,
			 dfu_ctx.seq++, SMP_CMD_IMG_STATE,
			 (uint16_t)cbor_len);

	rc = spi_smp_exchange(smp_buf, SMP_HDR_SIZE + cbor_len,
			      smp_resp, &resp_len, DFU_SMP_TIMEOUT_MS);
	if (rc) {
		LOG_ERR("Image list request failed: %d", rc);
		return rc;
	}

	if (resp_len < SMP_HDR_SIZE) {
		LOG_ERR("Short image list response: %u", resp_len);
		return -EINVAL;
	}

	rc = decode_image_list_hash(smp_resp + SMP_HDR_SIZE,
				    resp_len - SMP_HDR_SIZE,
				    hash_out, 32);
	if (rc) {
		LOG_ERR("Failed to extract hash from image list: %d", rc);
		return rc;
	}

	LOG_INF("SPI DFU: Got image hash from slot 1");
	return 0;
}

static int dfu_image_test(const uint8_t *hash)
{
	uint16_t resp_len;
	int rc;

	LOG_INF("SPI DFU: Marking image as test-pending");

	int cbor_len = encode_image_test(smp_buf + SMP_HDR_SIZE,
					 sizeof(smp_buf) - SMP_HDR_SIZE,
					 hash, 32);
	if (cbor_len < 0) {
		return cbor_len;
	}

	smp_header_build(smp_buf, SMP_OP_WRITE, SMP_GROUP_IMG,
			 dfu_ctx.seq++, SMP_CMD_IMG_STATE,
			 (uint16_t)cbor_len);

	rc = spi_smp_exchange(smp_buf, SMP_HDR_SIZE + cbor_len,
			      smp_resp, &resp_len, DFU_SMP_TIMEOUT_MS);
	if (rc) {
		LOG_ERR("Image test request failed: %d", rc);
		return rc;
	}

	if (resp_len >= SMP_HDR_SIZE) {
		int smp_rc = decode_rc_response(smp_resp + SMP_HDR_SIZE,
						resp_len - SMP_HDR_SIZE);
		if (smp_rc != 0) {
			LOG_ERR("Image test failed: rc=%d", smp_rc);
			return -EIO;
		}
	}

	LOG_INF("SPI DFU: Image marked as test-pending");
	return 0;
}

static int dfu_reset(void)
{
	uint16_t resp_len;

	LOG_INF("SPI DFU: Sending reset command");

	int cbor_len = encode_empty_map(smp_buf + SMP_HDR_SIZE,
					sizeof(smp_buf) - SMP_HDR_SIZE);
	smp_header_build(smp_buf, SMP_OP_WRITE, SMP_GROUP_OS,
			 dfu_ctx.seq++, SMP_CMD_OS_RESET,
			 (uint16_t)cbor_len);

	/* The nRF5340 may reboot before sending a response — treat
	 * timeout as success. */
	int rc = spi_smp_exchange(smp_buf, SMP_HDR_SIZE + cbor_len,
				  smp_resp, &resp_len, 5000);
	if (rc == -ETIMEDOUT) {
		LOG_INF("SPI DFU: Reset sent (no response — device rebooting)");
		return 0;
	}
	if (rc) {
		LOG_ERR("Reset send failed: %d", rc);
		return rc;
	}

	LOG_INF("SPI DFU: Reset command acknowledged");
	return 0;
}

/* =====================================================================
 * DFU Thread
 * ===================================================================== */

static void set_error(int code, const char *fmt, ...)
{
	va_list ap;

	k_mutex_lock(&dfu_ctx.lock, K_FOREVER);
	dfu_ctx.state = SPI_DFU_STATE_ERROR;
	dfu_ctx.error_code = code;

	va_start(ap, fmt);
	vsnprintf(dfu_ctx.error_msg, sizeof(dfu_ctx.error_msg), fmt, ap);
	va_end(ap);

	k_mutex_unlock(&dfu_ctx.lock);
}

static void dfu_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sem_take(&dfu_start_sem, K_FOREVER);

		int rc;
		uint8_t image_hash[32];
		const struct flash_area *fa;

		/* Verify firmware is ready */
		struct ext_dfu_status st;

		rc = ext_dfu_get_status(EXT_DFU_TARGET_NRF5340, &st);
		if (rc || st.state != EXT_DFU_STATE_DONE ||
		    st.bytes_written == 0) {
			LOG_ERR("SPI DFU: No firmware available (state=%d "
				"bytes=%u rc=%d)",
				st.state, (unsigned int)st.bytes_written, rc);
			set_error(-ENOENT,
				  "No firmware downloaded — download first");
			continue;
		}

		size_t image_size = st.bytes_written;

		k_mutex_lock(&dfu_ctx.lock, K_FOREVER);
		dfu_ctx.image_size = image_size;
		dfu_ctx.bytes_uploaded = 0;
		dfu_ctx.seq = 0;
		dfu_ctx.cancel_requested = false;
		dfu_ctx.state = SPI_DFU_STATE_UPLOADING;
		dfu_ctx.error_code = 0;
		dfu_ctx.error_msg[0] = '\0';
		k_mutex_unlock(&dfu_ctx.lock);

		/* Open flash partition for reading */
		rc = flash_area_open(PM_NRF5340_DFU_ID, &fa);
		if (rc) {
			LOG_ERR("SPI DFU: flash_area_open: %d", rc);
			set_error(rc, "flash_area_open: %d", rc);
			continue;
		}

		/* Phase 1: Upload firmware image */
		rc = dfu_upload_image(fa, image_size);
		flash_area_close(fa);

		if (rc) {
			set_error(rc, "Upload failed: %d", rc);
			continue;
		}

		/* Phase 2: Image List → get hash for slot 1 */
		k_mutex_lock(&dfu_ctx.lock, K_FOREVER);
		dfu_ctx.state = SPI_DFU_STATE_TESTING;
		k_mutex_unlock(&dfu_ctx.lock);

		rc = dfu_image_list(image_hash);
		if (rc) {
			set_error(rc, "Image list failed: %d", rc);
			continue;
		}

		/* Phase 3: Image Test → mark pending */
		rc = dfu_image_test(image_hash);
		if (rc) {
			set_error(rc, "Image test failed: %d", rc);
			continue;
		}

		/* Phase 4: Reset nRF5340 */
		k_mutex_lock(&dfu_ctx.lock, K_FOREVER);
		dfu_ctx.state = SPI_DFU_STATE_RESETTING;
		k_mutex_unlock(&dfu_ctx.lock);

		rc = dfu_reset();
		if (rc) {
			set_error(rc, "Reset failed: %d", rc);
			continue;
		}

		/* Done */
		k_mutex_lock(&dfu_ctx.lock, K_FOREVER);
		dfu_ctx.state = SPI_DFU_STATE_DONE;
		k_mutex_unlock(&dfu_ctx.lock);

		LOG_INF("SPI DFU: nRF5340 update complete — rebooting");
	}
}

/* =====================================================================
 * Public API
 * ===================================================================== */

int spi_dfu_start_nrf5340(void)
{
	LOG_INF("SPI DFU: start_nrf5340 requested");

	k_mutex_lock(&dfu_ctx.lock, K_FOREVER);

	if (dfu_ctx.state == SPI_DFU_STATE_UPLOADING ||
	    dfu_ctx.state == SPI_DFU_STATE_TESTING ||
	    dfu_ctx.state == SPI_DFU_STATE_RESETTING) {
		LOG_WRN("SPI DFU: already in progress (state=%d)", dfu_ctx.state);
		k_mutex_unlock(&dfu_ctx.lock);
		return -EBUSY;
	}

	/* Quick pre-check: is firmware available? */
	struct ext_dfu_status st;
	int rc = ext_dfu_get_status(EXT_DFU_TARGET_NRF5340, &st);

	if (rc || st.state != EXT_DFU_STATE_DONE || st.bytes_written == 0) {
		LOG_ERR("SPI DFU: no firmware available — download first "
			"(ext_dfu state=%d bytes=%u rc=%d)",
			st.state, (unsigned int)st.bytes_written, rc);
		k_mutex_unlock(&dfu_ctx.lock);
		return -ENOENT;
	}

	LOG_INF("SPI DFU: firmware ready (%u bytes) — starting upload to nRF5340",
		(unsigned int)st.bytes_written);

	dfu_ctx.state = SPI_DFU_STATE_UPLOADING;
	k_mutex_unlock(&dfu_ctx.lock);

	k_sem_give(&dfu_start_sem);
	return 0;
}

int spi_dfu_cancel(void)
{
	k_mutex_lock(&dfu_ctx.lock, K_FOREVER);

	if (dfu_ctx.state != SPI_DFU_STATE_UPLOADING) {
		k_mutex_unlock(&dfu_ctx.lock);
		return -EALREADY;
	}

	dfu_ctx.cancel_requested = true;
	k_mutex_unlock(&dfu_ctx.lock);
	return 0;
}

int spi_dfu_get_status(struct spi_dfu_status *status)
{
	if (!status) {
		return -EINVAL;
	}

	k_mutex_lock(&dfu_ctx.lock, K_FOREVER);

	status->state = dfu_ctx.state;
	status->bytes_uploaded = dfu_ctx.bytes_uploaded;
	status->image_size = dfu_ctx.image_size;
	status->error_code = dfu_ctx.error_code;
	memcpy(status->error_msg, dfu_ctx.error_msg,
	       sizeof(status->error_msg));

	if (dfu_ctx.image_size > 0 && dfu_ctx.bytes_uploaded > 0) {
		int pct = (int)((dfu_ctx.bytes_uploaded * 100) /
				 dfu_ctx.image_size);
		status->progress_pct = MIN(pct, 100);
	} else {
		status->progress_pct = -1;
	}

	k_mutex_unlock(&dfu_ctx.lock);
	return 0;
}

/* =====================================================================
 * Module Init
 * ===================================================================== */

static int spi_dfu_init(void)
{
	k_mutex_init(&dfu_ctx.lock);

	k_thread_create(&dfu_thread, dfu_thread_stack,
			DFU_THREAD_STACK_SIZE,
			dfu_thread_fn, NULL, NULL, NULL,
			DFU_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&dfu_thread, "spi_dfu");

	LOG_INF("spi_dfu: ready");
	return 0;
}

SYS_INIT(spi_dfu_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
