/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _FOTA_H_
#define _FOTA_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(FOTA_CHAN);

enum fota_msg_type {
	/* Output message types */

	/* Event notified when downloading the FOTA update failed. */
	FOTA_DOWNLOAD_FAILED = 0x1,

	/* Event notified when downloading the FOTA update timed out. */
	FOTA_DOWNLOAD_TIMED_OUT,

	/* Event notified when a FOTA update is being downloaded. */
	FOTA_DOWNLOADING_UPDATE,

	/* Event notified if there is no available update. */
	FOTA_NO_AVAILABLE_UPDATE,

	/* Event notified when a FOTA update has succeeded, reboot is needed to apply the image. */
	FOTA_SUCCESS_REBOOT_NEEDED,

	/* Event notified when the module needs the network to disconnect in order to apply
	 * an update. When disconnected from the network, send the event FOTA_IMAGE_APPLY.
	 * This is needed for Full Modem FOTA updates.
	 */
	FOTA_IMAGE_APPLY_NEEDED,

	/* Event notified when the FOTA download has been canceled. */
	FOTA_DOWNLOAD_CANCELED,

	/* Input message types */

	/* Request to poll cloud for any available firmware updates. */
	FOTA_POLL_REQUEST,

	/* Request to apply the downloaded firmware image. */
	FOTA_IMAGE_APPLY,

	/* Cancel the FOTA download. */
	FOTA_DOWNLOAD_CANCEL,
};

#define MSG_TO_FOTA_TYPE(_msg) (*(const enum fota_msg_type *)_msg)

/**
 * @brief Trigger an HTTP(S) firmware download.
 *
 * Stores the URL and security tag, then publishes FOTA_POLL_REQUEST on
 * FOTA_CHAN to kick off the download state machine.
 *
 * @param url      Full URL of firmware binary (e.g. "https://t4as.org/thingyupdate").
 * @param sec_tag  Modem TLS security tag with provisioned CA cert (>= 0),
 *                 or -1 to skip TLS certificate verification (plain HTTP).
 * @return 0 on success, negative errno on failure.
 */
int fota_http_trigger(const char *url, int sec_tag);

/**
 * @brief Get the current FOTA download progress.
 *
 * @return Download progress in percent (0-100), or -1 if no download is in
 *         progress.
 */
int fota_get_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* _FOTA_H_ */
