/*
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>

#include <nrf_modem_at.h>
#include <modem/modem_info.h>
#include <modem/lte_lc.h>

#include "selftest.h"
#include "network.h"
#include "app_common.h"

#if defined(CONFIG_APP_LED)
#include "led.h"
#endif

LOG_MODULE_REGISTER(selftest, CONFIG_APP_SELFTEST_LOG_LEVEL);

/* ---- Selftest LED active flag (shared with other modules) ---- */
static atomic_t selftest_led_active_flag = ATOMIC_INIT(0);

bool selftest_led_is_active(void)
{
	return atomic_get(&selftest_led_active_flag) != 0;
}

/* ---- Configuration ---- */
#define SELFTEST_SIM_WAIT_SEC   CONFIG_APP_SELFTEST_SIM_WAIT_SECONDS
#define SELFTEST_LED_DURATION_SEC CONFIG_APP_SELFTEST_LED_DURATION_SECONDS

/* Polling interval while waiting for network registration */
#define POLL_INTERVAL_MS        5000

/* ---- ZBUS channel ---- */
ZBUS_CHAN_DEFINE(SELFTEST_CHAN,
		 struct selftest_msg,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = SELFTEST_STARTED));

/* ---- ZBUS subscriber (listens to NETWORK_CHAN for connection events) ---- */
ZBUS_MSG_SUBSCRIBER_DEFINE(selftest_sub);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, selftest_sub, 0);

/* ---- LED signaling helpers ---- */

#if defined(CONFIG_APP_LED)

/* LED pattern definitions — each error has a unique blink pattern.
 *
 * Pattern approach:
 *   - Each error blinks a distinct colour a specific number of times.
 *   - After all error patterns are shown, the LED is turned off.
 *   - The total signaling window is limited to SELFTEST_LED_DURATION_SEC.
 */

struct selftest_led_pattern {
	uint32_t flag;
	uint8_t  red;
	uint8_t  green;
	uint8_t  blue;
	uint8_t  blinks;      /* number of on/off cycles */
	uint16_t on_ms;
	uint16_t off_ms;
	uint16_t pause_after_ms; /* pause before next pattern */
};

/* Pattern table — order determines display order */
static const struct selftest_led_pattern led_patterns[] = {
	/* SIM not detected:        RED — 2 fast blinks */
	{ SELFTEST_FLAG_SIM_NOT_DETECTED,   255,   0,   0, 2, 150, 150, 600 },
	/* SIM PIN required:        RED — 3 fast blinks */
	{ SELFTEST_FLAG_SIM_PIN_REQUIRED,   255,   0,   0, 3, 150, 150, 600 },
	/* ICCID read fail:         RED — 4 fast blinks */
	{ SELFTEST_FLAG_SIM_ICCID_FAIL,     255,   0,   0, 4, 150, 150, 600 },
	/* IMSI read fail:          RED — 5 fast blinks */
	{ SELFTEST_FLAG_SIM_IMSI_FAIL,      255,   0,   0, 5, 150, 150, 600 },
	/* Modem FW read fail:      MAGENTA — 2 blinks */
	{ SELFTEST_FLAG_MODEM_FW_READ_FAIL, 255,   0, 200, 2, 200, 200, 600 },
	/* No network registration: ORANGE — 3 slow blinks */
	{ SELFTEST_FLAG_NO_NETWORK_REG,     255, 100,   0, 3, 300, 300, 600 },
	/* No IP address:           ORANGE — 5 slow blinks */
	{ SELFTEST_FLAG_NO_IP_ADDRESS,      255, 100,   0, 5, 300, 300, 600 },
	/* PSM not granted:         CYAN — 2 blinks */
	{ SELFTEST_FLAG_PSM_NOT_GRANTED,      0, 200, 200, 2, 200, 200, 600 },
	/* eDRX not supported:      CYAN — 3 blinks */
	{ SELFTEST_FLAG_EDRX_NOT_SUPPORTED,   0, 200, 200, 3, 200, 200, 600 },
	/* DNS resolution fail:     YELLOW — 3 blinks */
	{ SELFTEST_FLAG_DNS_FAIL,           255, 200,   0, 3, 200, 200, 600 },
	/* CEREG format fail:       MAGENTA — 4 blinks */
	{ SELFTEST_FLAG_CEREGF_FAIL,        255,   0, 200, 4, 200, 200, 600 },
	/* Weak signal (RSRP):      WHITE — 2 slow blinks */
	{ SELFTEST_FLAG_WEAK_SIGNAL,        200, 200, 200, 2, 400, 400, 600 },
	/* Poor signal quality (RSRQ): WHITE — 4 slow blinks */
	{ SELFTEST_FLAG_POOR_RSRQ,          200, 200, 200, 4, 400, 400, 600 },
};

static void selftest_show_led_errors(uint32_t flags)
{
	int64_t deadline = k_uptime_get() + (SELFTEST_LED_DURATION_SEC * 1000LL);

	atomic_set(&selftest_led_active_flag, 1);

	for (size_t i = 0; i < ARRAY_SIZE(led_patterns); i++) {
		if (k_uptime_get() >= deadline) {
			break;
		}
		if (!(flags & led_patterns[i].flag)) {
			continue;
		}

		struct led_msg msg = {
			.type = LED_RGB_SET,
			.red = led_patterns[i].red,
			.green = led_patterns[i].green,
			.blue = led_patterns[i].blue,
			.duration_on_msec = led_patterns[i].on_ms,
			.duration_off_msec = led_patterns[i].off_ms,
			.repetitions = led_patterns[i].blinks,
		};

		int err = zbus_chan_pub(&LED_CHAN, &msg, K_SECONDS(1));
		if (err) {
			LOG_ERR("LED publish failed: %d", err);
		}

		/* Wait for the pattern to play + pause */
		uint32_t pattern_time_ms =
			(uint32_t)led_patterns[i].blinks *
			(led_patterns[i].on_ms + led_patterns[i].off_ms) +
			led_patterns[i].pause_after_ms;
		k_sleep(K_MSEC(pattern_time_ms));
	}

	/* Turn LED off after signaling */
	struct led_msg off_msg = {
		.type = LED_RGB_SET,
		.red = 0,
		.green = 0,
		.blue = 0,
		.duration_on_msec = 0,
		.duration_off_msec = 0,
		.repetitions = 0,
	};
	(void)zbus_chan_pub(&LED_CHAN, &off_msg, K_SECONDS(1));

	atomic_set(&selftest_led_active_flag, 0);
}

static void selftest_show_success_led(void)
{
	atomic_set(&selftest_led_active_flag, 1);

	struct led_msg msg = {
		.type = LED_RGB_SET,
		.red = 0,
		.green = 255,
		.blue = 0,
		.duration_on_msec = 200,
		.duration_off_msec = 200,
		.repetitions = 3,
	};
	(void)zbus_chan_pub(&LED_CHAN, &msg, K_SECONDS(1));
	k_sleep(K_MSEC(3 * (200 + 200) + 300));

	/* Turn off */
	struct led_msg off_msg = {
		.type = LED_RGB_SET,
		.red = 0, .green = 0, .blue = 0,
		.duration_on_msec = 0, .duration_off_msec = 0,
		.repetitions = 0,
	};
	(void)zbus_chan_pub(&LED_CHAN, &off_msg, K_SECONDS(1));

	atomic_set(&selftest_led_active_flag, 0);
}

#endif /* CONFIG_APP_LED */

/* ---- AT command helpers ---- */

static int at_cmd_read(const char *cmd, char *buf, size_t buf_len)
{
	int err = nrf_modem_at_cmd(buf, buf_len, "%s", cmd);
	if (err) {
		LOG_WRN("AT cmd '%s' failed: %d", cmd, err);
	}
	return err;
}

/* Parse a quoted string from AT response: +CMD: "value"\r\n */
static int parse_quoted_string(const char *resp, char *out, size_t out_len)
{
	const char *start = strchr(resp, '"');
	if (!start) {
		return -EINVAL;
	}
	start++;
	const char *end = strchr(start, '"');
	if (!end) {
		return -EINVAL;
	}
	size_t len = (size_t)(end - start);
	if (len >= out_len) {
		len = out_len - 1;
	}
	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

/* ---- Self-test checks ---- */

static uint32_t check_sim_card(struct selftest_msg *result)
{
	char buf[128];
	uint32_t flags = 0;

	/* Check SIM status via AT+CPIN? */
	if (at_cmd_read("AT+CPIN?", buf, sizeof(buf)) != 0) {
		LOG_ERR("SIM card not detected or not responding");
		flags |= SELFTEST_FLAG_SIM_NOT_DETECTED;
		return flags;
	}

	if (strstr(buf, "SIM PIN") != NULL && strstr(buf, "READY") == NULL) {
		LOG_WRN("SIM card requires PIN");
		flags |= SELFTEST_FLAG_SIM_PIN_REQUIRED;
	} else if (strstr(buf, "READY") == NULL) {
		LOG_ERR("SIM card not ready: %s", buf);
		flags |= SELFTEST_FLAG_SIM_NOT_DETECTED;
		return flags;
	}

	LOG_INF("SIM card detected and ready");

	/* Read ICCID */
	if (at_cmd_read("AT+CCID", buf, sizeof(buf)) == 0) {
		if (parse_quoted_string(buf, result->iccid, sizeof(result->iccid)) != 0) {
			/* Some modems return ICCID without quotes, try direct parse */
			char *p = strstr(buf, "+CCID: ");
			if (p) {
				p += 7;
			} else {
				/* Response may be just the ICCID */
				p = buf;
			}
			/* Copy digits only */
			size_t i = 0;
			while (*p >= '0' && *p <= '9' && i < sizeof(result->iccid) - 1) {
				result->iccid[i++] = *p++;
			}
			result->iccid[i] = '\0';
		}
		if (strlen(result->iccid) < 10) {
			LOG_WRN("ICCID read returned short/invalid value: '%s'", result->iccid);
			flags |= SELFTEST_FLAG_SIM_ICCID_FAIL;
		} else {
			LOG_INF("ICCID: %s", result->iccid);
		}
	} else {
		flags |= SELFTEST_FLAG_SIM_ICCID_FAIL;
	}

	/* Read IMSI */
	if (at_cmd_read("AT+CIMI", buf, sizeof(buf)) == 0) {
		/* IMSI is returned as plain digits, no prefix */
		char *p = buf;
		/* Skip any non-digit prefix */
		while (*p && (*p < '0' || *p > '9')) {
			p++;
		}
		size_t i = 0;
		while (*p >= '0' && *p <= '9' && i < sizeof(result->imsi) - 1) {
			result->imsi[i++] = *p++;
		}
		result->imsi[i] = '\0';

		if (strlen(result->imsi) < 10) {
			LOG_WRN("IMSI read returned short/invalid value: '%s'", result->imsi);
			flags |= SELFTEST_FLAG_SIM_IMSI_FAIL;
		} else {
			LOG_INF("IMSI: %s", result->imsi);
		}
	} else {
		flags |= SELFTEST_FLAG_SIM_IMSI_FAIL;
	}

	return flags;
}

static uint32_t check_modem_firmware(struct selftest_msg *result)
{
	char buf[128];
	uint32_t flags = 0;

	/* AT+CGMR returns modem firmware version */
	if (at_cmd_read("AT+CGMR", buf, sizeof(buf)) == 0) {
		/* Response is typically just the version string */
		char *p = buf;
		while (*p && (*p == '\r' || *p == '\n' || *p == ' ')) {
			p++;
		}
		size_t i = 0;
		while (p[i] && p[i] != '\r' && p[i] != '\n' && i < sizeof(result->modem_fw) - 1) {
			result->modem_fw[i] = p[i];
			i++;
		}
		result->modem_fw[i] = '\0';
		LOG_INF("Modem FW: %s", result->modem_fw);
	} else {
		flags |= SELFTEST_FLAG_MODEM_FW_READ_FAIL;
	}

	return flags;
}

static uint32_t check_network_registration(struct selftest_msg *result)
{
	char buf[256];
	uint32_t flags = 0;

	/* Enable extended CEREG reporting first */
	if (at_cmd_read("AT+CEREG=5", buf, sizeof(buf)) != 0) {
		LOG_WRN("Could not set CEREG format");
		flags |= SELFTEST_FLAG_CEREGF_FAIL;
	}

	/* Read current registration status: AT+CEREG? */
	if (at_cmd_read("AT+CEREG?", buf, sizeof(buf)) == 0) {
		/* Parse: +CEREG: <n>,<stat>[,<tac>,<ci>,...] */
		int n_val = 0, stat = 0;
		unsigned int tac = 0, ci = 0;
		int parsed = sscanf(buf, "+CEREG: %d,%d,\"%x\",\"%x\"",
				    &n_val, &stat, &tac, &ci);
		if (parsed >= 2) {
			result->area_code = (uint16_t)tac;
			result->cell_id = (uint16_t)(ci & 0xFFFF);

			/* stat: 1 = home, 5 = roaming */
			if (stat != 1 && stat != 5) {
				LOG_WRN("Not registered to network (CEREG stat=%d)", stat);
				flags |= SELFTEST_FLAG_NO_NETWORK_REG;
			} else {
				LOG_INF("Registered to network (stat=%d, TAC=0x%04X, CI=0x%04X)",
					stat, tac, ci);
			}
		} else {
			LOG_WRN("Could not parse CEREG response: %s", buf);
			flags |= SELFTEST_FLAG_NO_NETWORK_REG;
		}
	} else {
		flags |= SELFTEST_FLAG_NO_NETWORK_REG;
	}

	/* Read operator name */
	if (at_cmd_read("AT+COPS?", buf, sizeof(buf)) == 0) {
		if (parse_quoted_string(buf, result->operator_name,
					sizeof(result->operator_name)) != 0) {
			snprintf(result->operator_name, sizeof(result->operator_name), "unknown");
		}
		LOG_INF("Operator: %s", result->operator_name);
	}

	return flags;
}

static uint32_t check_signal_quality(struct selftest_msg *result)
{
	char buf[128];
	uint32_t flags = 0;

	/* AT%CESQ — extended signal quality for nRF modems */
	if (at_cmd_read("AT%CESQ", buf, sizeof(buf)) == 0) {
		/* Response: %CESQ: <rxlev>,<ber>,<rscp>,<ecn0>,<rsrq>,<rsrp>[,<snr>] */
		int rxlev, ber, rscp, ecn0, rsrq_raw, rsrp_raw, snr_raw;
		int parsed = sscanf(buf, "%%CESQ: %d,%d,%d,%d,%d,%d,%d",
				    &rxlev, &ber, &rscp, &ecn0, &rsrq_raw, &rsrp_raw, &snr_raw);
		if (parsed >= 6 && rsrp_raw != 255) {
			/* RSRP: value 0..97 maps to -140..-44 dBm (3GPP TS 27.007) */
			result->rsrp = (int16_t)(rsrp_raw - 140);
			LOG_INF("RSRP: %d dBm", result->rsrp);

			if (result->rsrp < -120) {
				LOG_WRN("Very weak signal (RSRP=%d dBm, threshold -120 dBm)",
					result->rsrp);
				flags |= SELFTEST_FLAG_WEAK_SIGNAL;
			}
		}
		if (parsed >= 6 && rsrq_raw != 255) {
			/* RSRQ: value 0..34 maps to -19.5..-2.5 dB in 0.5 dB steps.
			 * Stored as dB * 10: rsrq_x10 = rsrq_raw * 5 - 195 */
			result->rsrq_x10_db = (int16_t)(rsrq_raw * 5 - 195);
			LOG_INF("RSRQ: %d.%d dB",
				result->rsrq_x10_db / 10,
				abs(result->rsrq_x10_db) % 10);

			/* Threshold: RSRQ < -15.0 dB (rsrq_x10 < -150) is poor quality */
			if (result->rsrq_x10_db < -150) {
				LOG_WRN("Poor signal quality (RSRQ=%d.%d dB, threshold -15.0 dB)",
					result->rsrq_x10_db / 10,
					abs(result->rsrq_x10_db) % 10);
				flags |= SELFTEST_FLAG_POOR_RSRQ;
			}
		}
		if (parsed >= 7 && snr_raw != 255) {
			/* SNR: value maps to -24..76 dB range, stored *10 */
			result->snr = (int16_t)((snr_raw - 240) * 10 / 8);
			LOG_INF("SNR: %d.%d dB", result->snr / 10, abs(result->snr) % 10);
		}
	} else {
		/* Fallback: AT+CESQ (standard) */
		if (at_cmd_read("AT+CESQ", buf, sizeof(buf)) == 0) {
			int rxlev, ber, rscp, ecn0, rsrq_raw, rsrp_raw;

			if (sscanf(buf, "+CESQ: %d,%d,%d,%d,%d,%d",
				   &rxlev, &ber, &rscp, &ecn0, &rsrq_raw, &rsrp_raw) >= 6) {
				if (rsrp_raw != 255) {
					result->rsrp = (int16_t)(rsrp_raw - 140);
					if (result->rsrp < -120) {
						flags |= SELFTEST_FLAG_WEAK_SIGNAL;
					}
				}
				if (rsrq_raw != 255) {
					result->rsrq_x10_db = (int16_t)(rsrq_raw * 5 - 195);
					if (result->rsrq_x10_db < -150) {
						flags |= SELFTEST_FLAG_POOR_RSRQ;
					}
				}
			}
		}
	}

	return flags;
}

static uint32_t check_psm_status(struct selftest_msg *result)
{
	char buf[128];
	uint32_t flags = 0;

	/* AT+CPSMS? — query PSM mode (0=disabled, 1=enabled/requested) */
	if (at_cmd_read("AT+CPSMS?", buf, sizeof(buf)) == 0) {
		int mode = 0;

		if (sscanf(buf, "+CPSMS: %d", &mode) >= 1 && mode == 1) {
			result->psm_granted = true;
			LOG_INF("PSM mode requested by device");
		} else {
			LOG_WRN("PSM not requested by device (mode=%d)", mode);
			result->psm_granted = false;
			flags |= SELFTEST_FLAG_PSM_NOT_GRANTED;
		}
	} else {
		result->psm_granted = false;
		flags |= SELFTEST_FLAG_PSM_NOT_GRANTED;
	}

	/* Read granted PSM timer values from the modem (what the network actually granted) */
	int tau_sec = -1;
	int active_time_sec = -1;

	if (lte_lc_psm_get(&tau_sec, &active_time_sec) == 0) {
		result->psm_tau = tau_sec;
		result->psm_active_time = active_time_sec;

		if (tau_sec > 0 && active_time_sec >= 0) {
			LOG_INF("PSM granted — TAU: %d s, Active time: %d s",
				tau_sec, active_time_sec);

			/* Warn if TAU is very short (< 5 min) — bad for battery */
			if (tau_sec < 300) {
				LOG_WRN("PSM TAU granted is very short (%d s, expect >300 s)."
					" Network may not support the requested PSM interval.",
					tau_sec);
			}
		} else if (result->psm_granted) {
			/* PSM requested but network didn't grant it */
			LOG_WRN("PSM requested but network did not grant PSM timers"
				" (tau=%d, active=%d)", tau_sec, active_time_sec);
			result->psm_granted = false;
			flags |= SELFTEST_FLAG_PSM_NOT_GRANTED;
		}
	} else {
		result->psm_tau = -1;
		result->psm_active_time = -1;
	}

	return flags;
}

static uint32_t check_ip_and_dns(struct selftest_msg *result)
{
	char buf[256];
	uint32_t flags = 0;

	ARG_UNUSED(result);

	/* AT+CGDCONT? — check PDP context / IP address */
	if (at_cmd_read("AT+CGDCONT?", buf, sizeof(buf)) == 0) {
		/* Look for an IP address in the response */
		if (strstr(buf, "0.0.0.0") != NULL || strlen(buf) < 20) {
			LOG_WRN("No valid IP address assigned");
			flags |= SELFTEST_FLAG_NO_IP_ADDRESS;
		} else {
			LOG_INF("PDP context active");
		}
	} else {
		flags |= SELFTEST_FLAG_NO_IP_ADDRESS;
	}

	/* DNS resolution test — try to resolve a well-known host */
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *addr_result = NULL;
	int ret = zsock_getaddrinfo("dns.google", "443", &hints, &addr_result);
	if (ret != 0 || addr_result == NULL) {
		LOG_WRN("DNS resolution failed (err=%d)", ret);
		flags |= SELFTEST_FLAG_DNS_FAIL;
	} else {
		LOG_INF("DNS resolution OK");
		zsock_freeaddrinfo(addr_result);
	}

	return flags;
}

/* ---- Main self-test orchestrator ---- */

static void selftest_run(void)
{
	struct selftest_msg result;
	int64_t start_time = k_uptime_get();
	int err;

	memset(&result, 0, sizeof(result));
	result.type = SELFTEST_STARTED;
	result.psm_tau = -1;
	result.psm_active_time = -1;

	/* Publish STARTED event */
	err = zbus_chan_pub(&SELFTEST_CHAN, &result, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish selftest start: %d", err);
	}

	LOG_INF("=== STARTUP SELF-TEST BEGIN ===");

	/* Phase 1: SIM card checks (do not need network) */
	LOG_INF("--- Phase 1: SIM card ---");
	result.flags |= check_sim_card(&result);

	/* Phase 2: Modem firmware */
	LOG_INF("--- Phase 2: Modem firmware ---");
	result.flags |= check_modem_firmware(&result);

	if (result.flags & (SELFTEST_FLAG_SIM_NOT_DETECTED | SELFTEST_FLAG_SIM_PIN_REQUIRED)) {
		/* SIM is not usable — skip network-dependent tests but still wait the
		 * configured time in case the SIM is just slow to initialise. */
		LOG_WRN("SIM not usable, waiting %d seconds before final verdict...",
			SELFTEST_SIM_WAIT_SEC);

		int64_t wait_deadline = k_uptime_get() + (SELFTEST_SIM_WAIT_SEC * 1000LL);
		bool recovered = false;

		while (k_uptime_get() < wait_deadline) {
			k_sleep(K_MSEC(POLL_INTERVAL_MS));

			char buf[64];
			if (at_cmd_read("AT+CPIN?", buf, sizeof(buf)) == 0 &&
			    strstr(buf, "READY") != NULL) {
				LOG_INF("SIM became ready after waiting");
				result.flags &= ~(SELFTEST_FLAG_SIM_NOT_DETECTED |
						  SELFTEST_FLAG_SIM_PIN_REQUIRED);
				/* Re-run SIM checks */
				result.flags |= check_sim_card(&result);
				recovered = true;
				break;
			}
		}

		if (!recovered) {
			LOG_ERR("SIM did not become ready after %d seconds", SELFTEST_SIM_WAIT_SEC);
			goto finish;
		}
	}

	/* Phase 3: Wait for network registration.
	 * The network module is already trying to connect — we just observe. */
	LOG_INF("--- Phase 3: Network registration (waiting up to %d seconds) ---",
		SELFTEST_SIM_WAIT_SEC);
	{
		int64_t net_deadline = k_uptime_get() + (SELFTEST_SIM_WAIT_SEC * 1000LL);
		bool registered = false;
		const struct zbus_channel *chan;
		uint8_t zbus_buf[sizeof(struct network_msg)];

		while (k_uptime_get() < net_deadline) {
			/* Check ZBUS for network events */
			err = zbus_sub_wait_msg(&selftest_sub, &chan, zbus_buf,
						K_MSEC(POLL_INTERVAL_MS));
			if (err == 0 && chan == &NETWORK_CHAN) {
				struct network_msg net_msg =
					*(const struct network_msg *)zbus_buf;

				if (net_msg.type == NETWORK_CONNECTED) {
					LOG_INF("Network connected during self-test");
					registered = true;
					break;
				} else if (net_msg.type == NETWORK_UICC_FAILURE) {
					LOG_ERR("UICC failure reported during self-test");
					result.flags |= SELFTEST_FLAG_SIM_NOT_DETECTED;
					goto finish;
				}
			}

			/* Also poll CEREG directly */
			char buf[256];
			if (at_cmd_read("AT+CEREG?", buf, sizeof(buf)) == 0) {
				int n_val = 0, stat = 0;
				if (sscanf(buf, "+CEREG: %d,%d", &n_val, &stat) >= 2) {
					if (stat == 1 || stat == 5) {
						LOG_INF("Network registered (CEREG stat=%d)", stat);
						registered = true;
						break;
					}
				}
			}
		}

		if (!registered) {
			LOG_WRN("Network registration timed out after %d seconds",
				SELFTEST_SIM_WAIT_SEC);
			result.flags |= SELFTEST_FLAG_NO_NETWORK_REG;
		}
	}

	/* Phase 4: Network-dependent checks (only if registered) */
	if (!(result.flags & SELFTEST_FLAG_NO_NETWORK_REG)) {
		LOG_INF("--- Phase 4: Network details ---");
		result.flags |= check_network_registration(&result);
		result.flags |= check_signal_quality(&result);
		result.flags |= check_psm_status(&result);

		/* Wait a little for IP assignment */
		k_sleep(K_SECONDS(5));
		result.flags |= check_ip_and_dns(&result);
	}

finish:
	result.type = SELFTEST_COMPLETE;
	result.test_duration_ms = k_uptime_get() - start_time;

	LOG_INF("=== STARTUP SELF-TEST COMPLETE ===");
	LOG_INF("Duration: %lld ms", result.test_duration_ms);
	LOG_INF("Flags: 0x%08X (%s)", result.flags,
		result.flags == 0 ? "ALL PASS" : "ISSUES FOUND");

	if (result.flags != 0) {
		LOG_WRN("Self-test issues detected:");
		if (result.flags & SELFTEST_FLAG_SIM_NOT_DETECTED) {
			LOG_WRN("  - SIM card not detected");
		}
		if (result.flags & SELFTEST_FLAG_SIM_PIN_REQUIRED) {
			LOG_WRN("  - SIM PIN required");
		}
		if (result.flags & SELFTEST_FLAG_SIM_ICCID_FAIL) {
			LOG_WRN("  - Could not read ICCID");
		}
		if (result.flags & SELFTEST_FLAG_SIM_IMSI_FAIL) {
			LOG_WRN("  - Could not read IMSI");
		}
		if (result.flags & SELFTEST_FLAG_MODEM_FW_READ_FAIL) {
			LOG_WRN("  - Could not read modem firmware version");
		}
		if (result.flags & SELFTEST_FLAG_NO_NETWORK_REG) {
			LOG_WRN("  - No network registration after %d seconds", SELFTEST_SIM_WAIT_SEC);
		}
		if (result.flags & SELFTEST_FLAG_NO_IP_ADDRESS) {
			LOG_WRN("  - No IP address assigned");
		}
		if (result.flags & SELFTEST_FLAG_PSM_NOT_GRANTED) {
			LOG_WRN("  - PSM not granted by network");
		}
		if (result.flags & SELFTEST_FLAG_EDRX_NOT_SUPPORTED) {
			LOG_WRN("  - eDRX not supported/granted");
		}
		if (result.flags & SELFTEST_FLAG_DNS_FAIL) {
			LOG_WRN("  - DNS resolution failed");
		}
		if (result.flags & SELFTEST_FLAG_WEAK_SIGNAL) {
			LOG_WRN("  - Weak cellular signal (RSRP=%d dBm, threshold -120 dBm)",
				result.rsrp);
		}
		if (result.flags & SELFTEST_FLAG_POOR_RSRQ) {
			LOG_WRN("  - Poor signal quality (RSRQ=%d.%d dB, threshold -15.0 dB)",
				result.rsrq_x10_db / 10, abs(result.rsrq_x10_db) % 10);
		}
	}

	/* Publish result to ZBUS for MQTT module to pick up */
	err = zbus_chan_pub(&SELFTEST_CHAN, &result, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish selftest result: %d", err);
	}

	/* LED signaling — limited to SELFTEST_LED_DURATION_SEC */
#if defined(CONFIG_APP_LED)
	if (result.flags != 0) {
		selftest_show_led_errors(result.flags);
	} else {
		selftest_show_success_led();
	}
#endif
}

/* ---- Thread ---- */

static void selftest_thread_fn(void)
{
	/* Small delay to let modem and network module initialise */
	k_sleep(K_SECONDS(3));

	selftest_run();

	LOG_INF("Self-test thread finished");
}

K_THREAD_DEFINE(selftest_thread, CONFIG_APP_SELFTEST_THREAD_STACK_SIZE,
		selftest_thread_fn, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
