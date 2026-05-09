/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Local stub for pm_config.h. ext_dfu.c uses two partition IDs that
 * are normally provided by NCS's partition manager.
 */
#ifndef PM_CONFIG_H_
#define PM_CONFIG_H_

#ifndef PM_NRF5340_DFU_ID
#define PM_NRF5340_DFU_ID 1
#endif
#ifndef PM_NRF52840_DFU_ID
#define PM_NRF52840_DFU_ID 2
#endif

#endif /* PM_CONFIG_H_ */
