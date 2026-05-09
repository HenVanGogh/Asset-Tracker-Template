/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Stubs for flash_map, settings and downloader APIs used by ext_dfu.c.
 *
 * The stubs implement an in-memory "flash" so that the test can exercise
 * the slot lifecycle (open/erase/close, write progress, persisted size)
 * without a real device or QSPI controller.
 *
 * TODO (extend coverage):
 *   - simulate downloader event callbacks (DOWNLOADER_EVT_FRAGMENT,
 *     DOWNLOADER_EVT_DONE, DOWNLOADER_EVT_ERROR) by capturing the
 *     callback registered via downloader_init() and invoking it from
 *     test bodies to drive ext_dfu through ERASING -> DOWNLOADING -> DONE.
 *   - inject flash_area_write() failures and confirm STATE_ERROR.
 */

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/settings/settings.h>
#include <net/downloader.h>
#include <string.h>

/* ---- Fake flash --------------------------------------------------------- */

#define STUB_FLASH_SIZE (256 * 1024)

static uint8_t fake_flash[STUB_FLASH_SIZE];
static struct flash_area fake_fa = {
	.fa_id = 1,
	.fa_off = 0,
	.fa_size = STUB_FLASH_SIZE,
};

unsigned int flash_open_count;
unsigned int flash_erase_count;
unsigned int flash_close_count;
unsigned int flash_write_count;
size_t       flash_total_written;

int flash_area_open(uint8_t id, const struct flash_area **fa)
{
	(void)id;
	flash_open_count++;
	*fa = &fake_fa;
	return 0;
}

void flash_area_close(const struct flash_area *fa)
{
	(void)fa;
	flash_close_count++;
}

int flash_area_erase(const struct flash_area *fa, off_t off, size_t len)
{
	(void)fa;
	flash_erase_count++;
	if (off + len <= STUB_FLASH_SIZE) {
		memset(fake_flash + off, 0xFF, len);
	}
	return 0;
}

int flash_area_write(const struct flash_area *fa, off_t off,
		     const void *src, size_t len)
{
	(void)fa;
	flash_write_count++;
	flash_total_written += len;
	if (off + len <= STUB_FLASH_SIZE) {
		memcpy(fake_flash + off, src, len);
	}
	return 0;
}

int flash_area_read(const struct flash_area *fa, off_t off,
		    void *dst, size_t len)
{
	(void)fa;
	if (off + len <= STUB_FLASH_SIZE) {
		memcpy(dst, fake_flash + off, len);
	}
	return 0;
}

/* ---- Settings ---------------------------------------------------------- */

int settings_subsys_init(void) { return 0; }
int settings_load(void) { return 0; }
int settings_load_subtree(const char *subtree) { (void)subtree; return 0; }
int settings_register(struct settings_handler *h) { (void)h; return 0; }
int settings_save_one(const char *name, const void *value, size_t val_len)
{
	(void)name; (void)value; (void)val_len;
	return 0;
}
int settings_delete(const char *name)
{
	(void)name;
	return 0;
}
int settings_name_steq(const char *name, const char *key, const char **next)
{
	(void)name; (void)key; (void)next;
	return 0;
}

/* ---- Downloader -------------------------------------------------------- */

unsigned int downloader_start_count;
unsigned int downloader_cancel_count;
int          downloader_start_return;

int downloader_init(struct downloader *dl, struct downloader_cfg *cfg)
{
	(void)dl; (void)cfg;
	return 0;
}

int downloader_get(struct downloader *dl, const struct downloader_host_cfg *host_cfg,
		   const char *url, size_t from)
{
	(void)dl; (void)host_cfg; (void)url; (void)from;
	downloader_start_count++;
	return downloader_start_return;
}

int downloader_cancel(struct downloader *dl)
{
	(void)dl;
	downloader_cancel_count++;
	return 0;
}

int downloader_deinit(struct downloader *dl)
{
	(void)dl;
	return 0;
}

int downloader_file_size_get(struct downloader *dl, size_t *size)
{
	(void)dl;
	*size = 0;
	return -EAGAIN;
}

void ext_dfu_stubs_reset(void)
{
	memset(fake_flash, 0, sizeof(fake_flash));
	flash_open_count = 0;
	flash_erase_count = 0;
	flash_close_count = 0;
	flash_write_count = 0;
	flash_total_written = 0;
	downloader_start_count = 0;
	downloader_cancel_count = 0;
	downloader_start_return = 0;
}
