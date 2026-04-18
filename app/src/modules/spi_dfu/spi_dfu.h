/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * SPI DFU Transport — push firmware from external flash to the nRF5340
 * over SPI using the MCUmgr SMP protocol (FRAME_SMP 0x06).
 *
 * The nRF5340 runs an MCUmgr SMP server on its SPI slave interface.
 * This module acts as the SMP client: it reads the firmware binary from
 * the nrf5340_dfu partition (previously downloaded by ext_dfu), fragments
 * it into 256-byte SMP upload chunks, and drives the full DFU sequence:
 * upload → image list → image test → reset.
 */

#ifndef SPI_DFU_H_
#define SPI_DFU_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** DFU progress state. */
enum spi_dfu_state {
	SPI_DFU_STATE_IDLE,
	SPI_DFU_STATE_UPLOADING,
	SPI_DFU_STATE_TESTING,
	SPI_DFU_STATE_RESETTING,
	SPI_DFU_STATE_DONE,
	SPI_DFU_STATE_ERROR,
};

/** Read-only snapshot of the DFU progress. */
struct spi_dfu_status {
	enum spi_dfu_state state;
	/** Upload progress 0–100, or -1 when idle/error. */
	int progress_pct;
	/** Bytes uploaded over SPI so far. */
	size_t bytes_uploaded;
	/** Total firmware image size (from ext_dfu slot). */
	size_t image_size;
	/** Error code (0 = no error). */
	int error_code;
	/** Human-readable error description. */
	char error_msg[64];
};

/**
 * @brief Start nRF5340 firmware update over SPI.
 *
 * Reads the firmware binary from the nrf5340_dfu external flash partition
 * (must have been previously downloaded via ext_dfu) and uploads it to
 * the nRF5340 using MCUmgr SMP over SPI frames.
 *
 * The process runs asynchronously in a dedicated thread.  Use
 * spi_dfu_get_status() to monitor progress.
 *
 * @return 0 on success (DFU started), -EBUSY if already running,
 *         -ENOENT if no firmware is available.
 */
int spi_dfu_start_nrf5340(void);

/**
 * @brief Cancel an active nRF5340 SPI DFU.
 *
 * The upload is aborted after the current chunk completes.
 *
 * @return 0 on success, -EALREADY if no DFU is active.
 */
int spi_dfu_cancel(void);

/**
 * @brief Get the current DFU status.
 *
 * @param status  Output status structure.
 * @return 0 on success, negative errno on bad parameter.
 */
int spi_dfu_get_status(struct spi_dfu_status *status);

#ifdef __cplusplus
}
#endif

#endif /* SPI_DFU_H_ */
