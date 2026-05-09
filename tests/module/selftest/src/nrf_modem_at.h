/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Minimal native_sim stub for the modem AT command interface.
 * Only the subset of symbols used by selftest.c is declared here.
 */

#ifndef NRF_MODEM_AT_H__
#define NRF_MODEM_AT_H__

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int nrf_modem_at_cmd(void *buf, size_t len, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* NRF_MODEM_AT_H__ */
