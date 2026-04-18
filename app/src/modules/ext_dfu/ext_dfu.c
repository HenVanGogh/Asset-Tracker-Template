/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * External DFU — download companion-chip firmware to external flash.
 * Uses the NCS downloader library for HTTP(S) and the Zephyr flash_area
 * API for storage.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <net/downloader.h>
#include <pm_config.h>
#include <string.h>

#include "ext_dfu.h"

LOG_MODULE_REGISTER(ext_dfu, CONFIG_APP_EXT_DFU_LOG_LEVEL);

/* ─── Partition IDs from Partition Manager ──────────────────────────────── */

static const uint8_t slot_pm_id[EXT_DFU_TARGET_COUNT] = {
	[EXT_DFU_TARGET_NRF5340]  = PM_NRF5340_DFU_ID,
	[EXT_DFU_TARGET_NRF52840] = PM_NRF52840_DFU_ID,
};

static const char *const target_name[EXT_DFU_TARGET_COUNT] = {
	[EXT_DFU_TARGET_NRF5340]  = "nRF5340",
	[EXT_DFU_TARGET_NRF52840] = "nRF52840",
};

/* ─── Download context ──────────────────────────────────────────────────── */

static struct {
	struct downloader         dl;
	const struct flash_area  *fa;
	enum ext_dfu_target       target;
	enum ext_dfu_state        state;
	size_t                    bytes_written;
	size_t                    file_size;
	size_t                    slot_size;
	int                       error;
	struct k_mutex            lock;
	int                       sec_tag;
	bool                      busy;
} ctx;

static char dl_buf[2048];

/* ─── Flash helpers ─────────────────────────────────────────────────────── */

static int slot_open(enum ext_dfu_target target)
{
	int err = flash_area_open(slot_pm_id[target], &ctx.fa);

	if (err) {
		LOG_ERR("flash_area_open(%s): %d", target_name[target], err);
		return err;
	}
	ctx.slot_size = ctx.fa->fa_size;
	return 0;
}

static void slot_close(void)
{
	if (ctx.fa) {
		flash_area_close(ctx.fa);
		ctx.fa = NULL;
	}
}

static int slot_erase(void)
{
	if (!ctx.fa) {
		return -EINVAL;
	}
	LOG_INF("Erasing %s slot (%u bytes)...", target_name[ctx.target],
		(unsigned int)ctx.fa->fa_size);
	int err = flash_area_erase(ctx.fa, 0, ctx.fa->fa_size);

	if (err) {
		LOG_ERR("flash_area_erase: %d", err);
	} else {
		LOG_INF("Erase complete");
	}
	return err;
}

static int slot_write(const void *data, size_t len)
{
	if (!ctx.fa) {
		return -EINVAL;
	}
	if (ctx.bytes_written + len > ctx.fa->fa_size) {
		LOG_ERR("Image too large for slot (%u + %u > %u)",
			(unsigned int)ctx.bytes_written, (unsigned int)len,
			(unsigned int)ctx.fa->fa_size);
		return -ENOSPC;
	}
	int err = flash_area_write(ctx.fa, ctx.bytes_written, data, len);

	if (err) {
		LOG_ERR("flash_area_write at 0x%x: %d",
			(unsigned int)ctx.bytes_written, err);
		return err;
	}
	ctx.bytes_written += len;
	return 0;
}

/* ─── Downloader callback (downloader thread context) ───────────────────── */

static int dl_callback(const struct downloader_evt *event)
{
	k_mutex_lock(&ctx.lock, K_FOREVER);

	switch (event->id) {
	case DOWNLOADER_EVT_FRAGMENT: {
		int err = slot_write(event->fragment.buf, event->fragment.len);

		if (err) {
			ctx.state = EXT_DFU_STATE_ERROR;
			ctx.error = err;
			k_mutex_unlock(&ctx.lock);
			return -1; /* stop download */
		}

		/* Update progress if file size is known */
		if (ctx.file_size > 0) {
			int pct = (int)((ctx.bytes_written * 100) / ctx.file_size);

			if (pct > 100) {
				pct = 100;
			}
			LOG_INF("%s DFU: %d%% (%u / %u)", target_name[ctx.target],
				pct, (unsigned int)ctx.bytes_written,
				(unsigned int)ctx.file_size);
		} else {
			LOG_INF("%s DFU: %u bytes", target_name[ctx.target],
				(unsigned int)ctx.bytes_written);
		}
		break;
	}

	case DOWNLOADER_EVT_DONE:
		LOG_INF("%s DFU: download complete — %u bytes stored",
			target_name[ctx.target], (unsigned int)ctx.bytes_written);
		ctx.state = EXT_DFU_STATE_DONE;
		ctx.file_size = ctx.bytes_written;
		slot_close();
		ctx.busy = false;
		break;

	case DOWNLOADER_EVT_ERROR:
		LOG_ERR("%s DFU: download error %d", target_name[ctx.target],
			event->error);
		ctx.state = EXT_DFU_STATE_ERROR;
		ctx.error = event->error;
		slot_close();
		ctx.busy = false;
		break;

	case DOWNLOADER_EVT_STOPPED:
		LOG_INF("%s DFU: download stopped", target_name[ctx.target]);
		if (ctx.state != EXT_DFU_STATE_DONE && ctx.state != EXT_DFU_STATE_ERROR) {
			ctx.state = EXT_DFU_STATE_IDLE;
		}
		slot_close();
		ctx.busy = false;
		break;

	default:
		break;
	}

	k_mutex_unlock(&ctx.lock);
	return 0;
}

/* ─── Public API ────────────────────────────────────────────────────────── */

int ext_dfu_start(enum ext_dfu_target target, const char *url, int sec_tag)
{
	int err;

	if (target >= EXT_DFU_TARGET_COUNT || !url || strlen(url) == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);

	if (ctx.busy) {
		LOG_WRN("ext_dfu: download already in progress");
		k_mutex_unlock(&ctx.lock);
		return -EBUSY;
	}

	/* Open flash slot */
	err = slot_open(target);
	if (err) {
		k_mutex_unlock(&ctx.lock);
		return err;
	}

	ctx.target = target;
	ctx.bytes_written = 0;
	ctx.file_size = 0;
	ctx.error = 0;
	ctx.sec_tag = sec_tag;
	ctx.busy = true;
	ctx.state = EXT_DFU_STATE_ERASING;

	/* Erase the slot */
	err = slot_erase();
	if (err) {
		ctx.state = EXT_DFU_STATE_ERROR;
		ctx.error = err;
		ctx.busy = false;
		slot_close();
		k_mutex_unlock(&ctx.lock);
		return err;
	}

	ctx.state = EXT_DFU_STATE_DOWNLOADING;

	/* Initialize downloader */
	struct downloader_cfg dl_cfg = {
		.callback = dl_callback,
		.buf = dl_buf,
		.buf_size = sizeof(dl_buf),
	};

	err = downloader_init(&ctx.dl, &dl_cfg);
	if (err) {
		LOG_ERR("downloader_init: %d", err);
		ctx.state = EXT_DFU_STATE_ERROR;
		ctx.error = err;
		ctx.busy = false;
		slot_close();
		k_mutex_unlock(&ctx.lock);
		return err;
	}

	/* Configure host (TLS if sec_tag >= 0) */
	struct downloader_host_cfg host_cfg = { 0 };

	if (sec_tag >= 0) {
		host_cfg.sec_tag_list = &ctx.sec_tag;
		host_cfg.sec_tag_count = 1;
	}

	LOG_INF("Starting %s DFU download: %s (sec_tag=%d, slot=%u bytes)",
		target_name[target], url, sec_tag,
		(unsigned int)ctx.slot_size);

	err = downloader_get(&ctx.dl, &host_cfg, url, 0);
	if (err) {
		LOG_ERR("downloader_get: %d", err);
		ctx.state = EXT_DFU_STATE_ERROR;
		ctx.error = err;
		ctx.busy = false;
		slot_close();
		downloader_deinit(&ctx.dl);
		k_mutex_unlock(&ctx.lock);
		return err;
	}

	k_mutex_unlock(&ctx.lock);
	return 0;
}

int ext_dfu_cancel(void)
{
	k_mutex_lock(&ctx.lock, K_FOREVER);

	if (!ctx.busy) {
		k_mutex_unlock(&ctx.lock);
		return -EALREADY;
	}

	LOG_INF("Canceling %s DFU download", target_name[ctx.target]);
	int err = downloader_cancel(&ctx.dl);

	k_mutex_unlock(&ctx.lock);
	return err;
}

int ext_dfu_get_status(enum ext_dfu_target target, struct ext_dfu_status *status)
{
	if (target >= EXT_DFU_TARGET_COUNT || !status) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);

	/* If this target is the active one, return live state */
	if (ctx.busy && ctx.target == target) {
		status->state = ctx.state;
		status->bytes_written = ctx.bytes_written;
		status->file_size = ctx.file_size;
		status->slot_size = ctx.slot_size;
		status->error = ctx.error;
		if (ctx.file_size > 0) {
			status->progress_pct = (int)((ctx.bytes_written * 100) / ctx.file_size);
		} else if (ctx.bytes_written > 0) {
			status->progress_pct = 0; /* unknown total */
		} else {
			status->progress_pct = -1;
		}
	} else if (!ctx.busy && ctx.target == target && ctx.state != EXT_DFU_STATE_IDLE) {
		/* Last completed/errored download was for this target */
		status->state = ctx.state;
		status->bytes_written = ctx.bytes_written;
		status->file_size = ctx.file_size;
		status->error = ctx.error;
		status->progress_pct = (ctx.state == EXT_DFU_STATE_DONE) ? 100 : -1;
		/* Get slot size from PM */
		const struct flash_area *fa;
		if (flash_area_open(slot_pm_id[target], &fa) == 0) {
			status->slot_size = fa->fa_size;
			flash_area_close(fa);
		} else {
			status->slot_size = 0;
		}
	} else {
		/* Idle — just report slot size */
		status->state = EXT_DFU_STATE_IDLE;
		status->progress_pct = -1;
		status->bytes_written = 0;
		status->file_size = 0;
		status->error = 0;
		const struct flash_area *fa;
		if (flash_area_open(slot_pm_id[target], &fa) == 0) {
			status->slot_size = fa->fa_size;
			flash_area_close(fa);
		} else {
			status->slot_size = 0;
		}
	}

	k_mutex_unlock(&ctx.lock);
	return 0;
}

int ext_dfu_erase(enum ext_dfu_target target)
{
	if (target >= EXT_DFU_TARGET_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);

	if (ctx.busy) {
		k_mutex_unlock(&ctx.lock);
		return -EBUSY;
	}

	int err = slot_open(target);

	if (err) {
		k_mutex_unlock(&ctx.lock);
		return err;
	}

	ctx.target = target;
	err = slot_erase();
	slot_close();

	if (err == 0) {
		ctx.state = EXT_DFU_STATE_IDLE;
		ctx.bytes_written = 0;
		ctx.file_size = 0;
		ctx.error = 0;
	}

	k_mutex_unlock(&ctx.lock);
	return err;
}

/* Module init — just initialize the mutex */
static int ext_dfu_init(void)
{
	k_mutex_init(&ctx.lock);
	LOG_INF("ext_dfu: ready (nRF5340 slot %u B @ PM %u, nRF52840 slot %u B @ PM %u)",
		PM_NRF5340_DFU_SIZE, PM_NRF5340_DFU_ID,
		PM_NRF52840_DFU_SIZE, PM_NRF52840_DFU_ID);
	return 0;
}

SYS_INIT(ext_dfu_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
