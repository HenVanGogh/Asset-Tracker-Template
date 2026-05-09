# Startup Self-Test Module

## Overview

The startup self-test module runs automatically at boot and performs a comprehensive diagnostic check of the device's cellular connectivity stack. Results are:

1. **Reported to MQTT** — a JSON `selftest_report` message is published as soon as the broker connection is established.
2. **Indicated via RGB LED** — a sequence of colour-coded blink patterns signals any detected issues. LED signaling is strictly limited to **60 seconds** after the self-test completes to conserve power.

The self-test is **read-only** — it does not change LTE/modem configuration. It observes the network module's existing connection attempt and queries AT commands to gather diagnostics.

---

## Self-Test Phases

| Phase | Description | Timeout |
|-------|-------------|---------|
| 1 — SIM Card | Checks SIM presence (`AT+CPIN?`), reads ICCID (`AT+CCID`) and IMSI (`AT+CIMI`) | Immediate |
| 2 — Modem FW | Reads modem firmware version (`AT+CGMR`) | Immediate |
| 3 — Network Registration | Waits for CEREG stat=1 (home) or 5 (roaming), also listens for ZBUS `NETWORK_CONNECTED` | Up to **4 minutes** (configurable) |
| 4 — Network Details | Signal quality (`AT%CESQ`), operator name (`AT+COPS?`), PSM status (`AT+CPSMS?`), IP/PDP context (`AT+CGDCONT?`), DNS resolution | ~5 seconds |

If the SIM card is not detected in Phase 1, the module waits the full 4-minute window before declaring it bad — some SIM cards are slow to initialise.

---

## Error Flags

Each issue is represented by a bit flag. Multiple issues can be active simultaneously.

| Flag | Hex | Description |
|------|-----|-------------|
| `SIM_NOT_DETECTED` | `0x001` | SIM card not responding to `AT+CPIN?` or not in READY state |
| `SIM_PIN_REQUIRED` | `0x002` | SIM requires PIN entry (not supported in field) |
| `SIM_ICCID_FAIL` | `0x004` | Could not read ICCID or value too short |
| `SIM_IMSI_FAIL` | `0x008` | Could not read IMSI or value too short |
| `MODEM_FW_READ_FAIL` | `0x010` | Could not read modem firmware version |
| `NO_NETWORK_REG` | `0x020` | No network registration after timeout |
| `NO_IP_ADDRESS` | `0x040` | No IP address assigned (PDP context failure) |
| `PSM_NOT_GRANTED` | `0x080` | PSM requested but not granted by network |
| `EDRX_NOT_SUPPORTED` | `0x100` | eDRX not granted by network |
| `DNS_FAIL` | `0x200` | DNS resolution of `dns.google` failed |
| `CEREG_FORMAT_FAIL` | `0x400` | Could not configure CEREG extended format |
| `WEAK_SIGNAL` | `0x800` | RSRP below −120 dBm |
| `POOR_RSRQ` | `0x1000` | RSRQ below −15.0 dB (poor link quality even if RSRP is OK) |

---

## RGB LED Error Patterns

LED patterns are displayed **only once** after the self-test completes. The signaling window is limited to **60 seconds** (configurable via `CONFIG_APP_SELFTEST_LED_DURATION_SECONDS`). After the window expires, the LED is turned off.

If **no issues** are found, a brief green success pattern is shown instead.

### Pattern Table

| Issue | LED Colour | Blinks | On/Off (ms) | Total Pattern Time | Visual Description |
|-------|-----------|--------|-------------|-------------------|-------------------|
| SIM not detected | **RED** | 2 | 150/150 | ~0.9 s | Two quick red flashes |
| SIM PIN required | **RED** | 3 | 150/150 | ~1.5 s | Three quick red flashes |
| ICCID read fail | **RED** | 4 | 150/150 | ~1.8 s | Four quick red flashes |
| IMSI read fail | **RED** | 5 | 150/150 | ~2.1 s | Five quick red flashes |
| Modem FW read fail | **MAGENTA** | 2 | 200/200 | ~1.4 s | Two magenta flashes |
| No network registration | **ORANGE** | 3 | 300/300 | ~2.4 s | Three slow orange flashes |
| No IP address | **ORANGE** | 5 | 300/300 | ~3.6 s | Five slow orange flashes |
| PSM not granted | **CYAN** | 2 | 200/200 | ~1.4 s | Two cyan flashes |
| eDRX not supported | **CYAN** | 3 | 200/200 | ~1.8 s | Three cyan flashes |
| DNS resolution fail | **YELLOW** | 3 | 200/200 | ~1.8 s | Three yellow flashes |
| CEREG format fail | **MAGENTA** | 4 | 200/200 | ~2.2 s | Four magenta flashes |
| Weak signal (RSRP) | **WHITE** | 2 | 400/400 | ~2.2 s | Two slow white flashes |
| Poor quality (RSRQ) | **WHITE** | 4 | 400/400 | ~4.0 s | Four slow white flashes |
| **All pass** | **GREEN** | 3 | 200/200 | ~1.5 s | Three green flashes |

### LED Colour Reference (RGB values)

| Colour | R | G | B | Meaning |
|--------|---|---|---|---------|
| Red | 255 | 0 | 0 | SIM card issues |
| Magenta | 255 | 0 | 200 | Modem/CEREG issues |
| Orange | 255 | 100 | 0 | Network connectivity issues |
| Cyan | 0 | 200 | 200 | Power saving feature issues (PSM/eDRX) |
| Yellow | 255 | 200 | 0 | DNS/internet connectivity issues |
| White | 200 | 200 | 200 | Signal quality warning |
| Green | 0 | 255 | 0 | All checks passed |

### Reading the LED Patterns

When multiple issues are detected, the patterns are shown **sequentially** in the order listed above with a 600 ms pause between each. For example, if both "SIM ICCID fail" and "No network registration" are flagged, you will see:

1. Four quick RED flashes (ICCID fail)
2. 600 ms pause
3. Three slow ORANGE flashes (no network registration)
4. 600 ms pause
5. LED turns off

**Count the blinks and note the colour** to identify the specific issue(s).

---

## MQTT Report Format

When the MQTT connection is established, a single `selftest_report` message is published to the configured publish topic. Example:

```json
{
  "type": "selftest_report",
  "device_id": "gateway_A1B2",
  "timestamp": 45230,
  "pass": false,
  "flags": 2208,
  "test_duration_ms": 248500,
  "modem_info": {
    "iccid": "8944501234567890123",
    "imsi": "234501234567890",
    "modem_fw": "mfw_nrf91x1_2.0.2",
    "operator": "Vodafone"
  },
  "signal": {
    "rsrp_dbm": -105,
    "rsrq_x10_db": -142,
    "snr_x10_db": 42,
    "cell_id": 12345,
    "area_code": 5678
  },
  "psm": {
    "granted": false,
    "tau_sec": -1,
    "active_time_sec": -1
  },
  "issues": [
    "psm_not_granted",
    "weak_signal_rsrp"
  ]
}
```

### Key Fields

| Field | Description |
|-------|-------------|
| `pass` | `true` if all checks passed (`flags == 0`) |
| `flags` | Bitmask of all detected issues (see Error Flags table) |
| `test_duration_ms` | How long the self-test took in milliseconds |
| `modem_info.iccid` | SIM card ICCID (unique card identifier) |
| `modem_info.imsi` | SIM subscriber identity |
| `modem_info.modem_fw` | Modem firmware version string |
| `modem_info.operator` | Registered network operator name |
| `signal.rsrp_dbm` | Reference Signal Received Power in dBm |
| `signal.rsrq_x10_db` | Reference Signal Received Quality × 10 in dB (e.g. -150 = -15.0 dB) |
| `signal.snr_x10_db` | Signal-to-Noise Ratio × 10 in dB |
| `psm.granted` | Whether PSM was granted by the network |
| `psm.tau_sec` | Granted TAU (Tracking Area Update) interval in seconds, -1 if not granted |
| `psm.active_time_sec` | Granted active time in seconds, -1 if not granted |
| `issues` | Array of human-readable issue names |

---

## Configuration (Kconfig)

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_APP_SELFTEST` | `y` | Enable/disable the self-test module |
| `CONFIG_APP_SELFTEST_SIM_WAIT_SECONDS` | `240` | Max wait time for SIM/network (seconds) |
| `CONFIG_APP_SELFTEST_LED_DURATION_SECONDS` | `60` | LED signaling window after test (seconds) |
| `CONFIG_APP_SELFTEST_THREAD_STACK_SIZE` | `4096` | Thread stack size |
| `CONFIG_APP_SELFTEST_LOG_LEVEL_*` | `DBG` | Log verbosity |

---

## Power Considerations

- The self-test runs **once at startup** and the thread exits after completion.
- LED signaling is strictly limited to 60 seconds (configurable). After the window, all LEDs are turned off.
- No ongoing background work or periodic checks are created.
- The module does not interfere with PSM entry — once the self-test thread finishes, it has zero power impact.

---

## Troubleshooting by LED Pattern

### Device shows RED blinks at startup
**SIM card issue.** Count the blinks:
- 2 blinks → SIM not detected. Check SIM is inserted correctly and contacts are clean.
- 3 blinks → SIM requires PIN. Use a SIM without PIN lock, or pre-unlock it.
- 4 blinks → ICCID unreadable. SIM may be damaged or incompatible.
- 5 blinks → IMSI unreadable. SIM may not be activated or is locked to another device.

### Device shows ORANGE blinks at startup
**Network connectivity issue.** Count the blinks:
- 3 blinks → No network registration after 4 minutes. Check coverage, antenna, and that the SIM's IoT plan supports LTE-M / NB-IoT.
- 5 blinks → No IP address. Network attached but PDP context failed. Check APN configuration and SIM data plan.

### Device shows CYAN blinks at startup
**Power saving feature issue.** The SIM/network does not support the requested power saving mode:
- 2 blinks → PSM not granted. The network operator may not support PSM. The device will still work but with higher battery drain.
- 3 blinks → eDRX not supported. Similar to PSM — informational, device still operates.

### Device shows YELLOW blinks at startup
**Internet access issue:**
- 3 blinks → DNS resolution failed. Network is connected but cannot resolve hostnames. Check firewall/NAT settings on the IoT plan.

### Device shows MAGENTA blinks at startup
**Modem issue:**
- 2 blinks → Could not read modem firmware version.
- 4 blinks → CEREG format configuration failed.

### Device shows WHITE blinks at startup
**Signal quality warning:**
- 2 slow blinks → RSRP is below −120 dBm. Consider relocating the device or improving antenna placement.

### Device shows GREEN blinks at startup
**All checks passed.** Three quick green flashes indicate the SIM, modem, network, and signal are all healthy.

---

## LED Conflict Resolution

The selftest module and main application state machine both publish LED patterns via `LED_CHAN`. To prevent the main state machine from overwriting selftest diagnostic patterns, the selftest module sets an **atomic flag** (`selftest_led_active_flag`) during the LED signaling window.

All LED publish sites in `main.c` check `selftest_led_is_active()` before sending their patterns. While the selftest LED window is active (up to 60 seconds after test completion), main state machine LED updates are silently suppressed. Once the selftest LED window expires and the flag is cleared, normal LED operation resumes.

This ensures that:
- Selftest error patterns are always visible and uninterrupted.
- No application state change can overwrite diagnostic blink sequences.
- After the selftest window, normal operation LED patterns resume automatically.

---

## SIM Card Diagnostics — Interpreting Results

### Good SIM Card (IoT-grade)
A properly configured IoT SIM card should produce:
- **Green LED** (3 flashes) — all pass
- MQTT report with `"pass": true`, `"flags": 0`
- PSM granted, valid ICCID/IMSI, strong signal, DNS working

### Bad/Incompatible SIM Card
A consumer-grade or poorly configured SIM card may show:
- **Red LED** — SIM not detected or not responding (card not IoT-compatible, contacts dirty, not inserted properly)
- **Orange LED** — Network won't register (plan doesn't support LTE-M/NB-IoT, wrong APN)
- **Cyan LED** — PSM/eDRX not granted (operator doesn't support power saving — device works but with higher battery drain)
- **Yellow LED** — DNS fails (firewall or NAT restrictions on IoT plan)

### Common Issue: Device Stops Connecting After Some Time
If the device connects initially but loses connection after hours/days:
- Check the MQTT `selftest_report` for `psm_not_granted` — without PSM the modem stays active and may hit data limits or network timeouts.
- Check for `weak_signal` — marginal signal causes intermittent disconnections.
- Check for `no_ip_address` — PDP context failures indicate APN or plan issues.
- Verify the SIM plan supports **always-on IoT** connectivity (some consumer plans disconnect idle devices).
