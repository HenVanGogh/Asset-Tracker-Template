/*
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SELFTEST_H_
#define SELFTEST_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Self-test result flags (bitmask) */
#define SELFTEST_FLAG_SIM_NOT_DETECTED      BIT(0)
#define SELFTEST_FLAG_SIM_PIN_REQUIRED      BIT(1)
#define SELFTEST_FLAG_SIM_ICCID_FAIL        BIT(2)
#define SELFTEST_FLAG_SIM_IMSI_FAIL         BIT(3)
#define SELFTEST_FLAG_MODEM_FW_READ_FAIL    BIT(4)
#define SELFTEST_FLAG_NO_NETWORK_REG        BIT(5)
#define SELFTEST_FLAG_NO_IP_ADDRESS         BIT(6)
#define SELFTEST_FLAG_PSM_NOT_GRANTED       BIT(7)
#define SELFTEST_FLAG_EDRX_NOT_SUPPORTED    BIT(8)
#define SELFTEST_FLAG_DNS_FAIL              BIT(9)
#define SELFTEST_FLAG_CEREGF_FAIL           BIT(10)
#define SELFTEST_FLAG_WEAK_SIGNAL           BIT(11)
#define SELFTEST_FLAG_POOR_RSRQ             BIT(12)

/* Self-test output message types */
enum selftest_msg_type {
	SELFTEST_STARTED,
	SELFTEST_COMPLETE,
};

/* Self-test result structure published via ZBUS */
struct selftest_msg {
	enum selftest_msg_type type;

	/* Bitmask of SELFTEST_FLAG_* */
	uint32_t flags;

	/* Modem / SIM info gathered during test */
	char iccid[24];
	char imsi[18];
	char modem_fw[32];
	char operator_name[32];

	/* Network params at test time */
	int16_t rsrp;           /* dBm, 0 if unavailable */
	int16_t rsrq_x10_db;   /* dB * 10, 0 if unavailable (e.g. -150 = -15.0 dB) */
	int16_t snr;            /* dB * 10, 0 if unavailable */
	uint16_t cell_id;
	uint16_t area_code;

	/* PSM / eDRX granted status */
	bool psm_granted;
	int  psm_tau;           /* seconds, -1 if not granted */
	int  psm_active_time;   /* seconds, -1 if not granted */

	/* Timing */
	int64_t test_duration_ms;
};

/* ZBUS channel declaration */
ZBUS_CHAN_DECLARE(SELFTEST_CHAN);

/**
 * @brief Check if the selftest LED signaling window is still active.
 *
 * Other modules should defer their own LED operations while this returns true
 * to avoid overwriting diagnostic blink patterns.
 *
 * @return true if selftest LED patterns are being displayed.
 */
bool selftest_led_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* SELFTEST_H_ */
