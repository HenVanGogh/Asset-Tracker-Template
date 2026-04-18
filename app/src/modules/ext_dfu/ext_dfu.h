/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * External DFU — download firmware images for companion chips (nRF5340,
 * nRF52840) to reserved slots on external QSPI flash.  The images are
 * downloaded via HTTP/HTTPS using the NCS downloader library and stored
 * raw (no MCUboot wrapper).  A later SPI transfer step will push them
 * to the target chip.
 */

#ifndef _EXT_DFU_H_
#define _EXT_DFU_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Target chip whose firmware is being staged. */
enum ext_dfu_target {
	EXT_DFU_TARGET_NRF5340  = 0,
	EXT_DFU_TARGET_NRF52840 = 1,
	EXT_DFU_TARGET_COUNT
};

/** Current state of a download slot. */
enum ext_dfu_state {
	EXT_DFU_STATE_IDLE,
	EXT_DFU_STATE_ERASING,
	EXT_DFU_STATE_DOWNLOADING,
	EXT_DFU_STATE_DONE,
	EXT_DFU_STATE_ERROR,
};

/** Read-only snapshot of a slot's status. */
struct ext_dfu_status {
	enum ext_dfu_state state;
	/** Download progress 0–100, or -1 when idle/error. */
	int progress_pct;
	/** Bytes written so far. */
	size_t bytes_written;
	/** Total file size (0 if unknown). */
	size_t file_size;
	/** Flash slot capacity. */
	size_t slot_size;
	/** Last error code (0 = no error). */
	int error;
};

/**
 * @brief Start downloading firmware for a companion chip.
 *
 * The image is fetched from @p url via HTTP(S) and written to the
 * corresponding external-flash partition (nrf5340_dfu / nrf52840_dfu).
 * Only one download may be active at a time.
 *
 * @param target  Which chip the image is for.
 * @param url     Full HTTP(S) URL of the firmware binary.
 * @param sec_tag Modem TLS security tag (>= 0), or -1 to skip TLS.
 * @return 0 on success, negative errno on failure.
 */
int ext_dfu_start(enum ext_dfu_target target, const char *url, int sec_tag);

/**
 * @brief Cancel an active download.
 *
 * @return 0 on success, -EALREADY if no download is active.
 */
int ext_dfu_cancel(void);

/**
 * @brief Get the status of a download slot.
 *
 * @param target  Which chip slot to query.
 * @param status  Output status structure.
 * @return 0 on success, negative errno on bad target.
 */
int ext_dfu_get_status(enum ext_dfu_target target, struct ext_dfu_status *status);

/**
 * @brief Erase a download slot.
 *
 * @param target  Which chip slot to erase.
 * @return 0 on success, negative errno on failure.
 */
int ext_dfu_erase(enum ext_dfu_target target);

#ifdef __cplusplus
}
#endif

#endif /* _EXT_DFU_H_ */
