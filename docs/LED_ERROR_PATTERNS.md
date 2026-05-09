# RGB LED Error Pattern Reference

## Overview

The Thingy91x asset tracker uses the onboard RGB LED to communicate device status and diagnostic results at startup. LED signaling is designed to be **power-efficient** — patterns are displayed only during a limited window after boot and then the LED is turned off entirely.

This document provides a complete reference for interpreting LED blink patterns.

---

## When LEDs Are Active

| Phase | Duration | Description |
|-------|----------|-------------|
| Startup self-test | 3–240+ seconds | Self-test runs automatically 3 seconds after boot |
| Error signaling | Up to **60 seconds** after self-test completes | Sequential error patterns shown |
| Normal operation | Brief flashes only on state transitions | Minimal power impact |

**Important:** During the self-test LED signaling window (up to 60 seconds), the selftest module has exclusive LED control. Normal application LED patterns (yellow/green/blue state indicators) are suppressed to prevent interference with diagnostic patterns.

After the signaling window expires, the LED turns off and normal application LED behavior resumes.

---

## Self-Test Error Patterns (Startup Only)

These patterns are shown **once**, sequentially, after the startup self-test completes. If multiple errors are detected, each pattern is shown in order with a 600 ms pause between patterns.

### SIM Card Issues — RED

| Blinks | On/Off (ms) | Total Time | Issue | Action |
|--------|-------------|------------|-------|--------|
| **2** | 150/150 | ~0.9 s | SIM not detected | Check SIM is inserted correctly, contacts are clean |
| **3** | 150/150 | ~1.5 s | SIM PIN required | Use a SIM without PIN lock or pre-unlock it |
| **4** | 150/150 | ~1.8 s | ICCID unreadable | SIM may be damaged or incompatible |
| **5** | 150/150 | ~2.1 s | IMSI unreadable | SIM may not be activated or is locked |

**RGB values:** R=255, G=0, B=0

**Note:** The device waits **4 minutes** (240 seconds) before declaring a SIM card as bad. Some IoT SIM cards are slow to initialize, especially on first use or in weak coverage areas.

---

### Network Connectivity Issues — ORANGE

| Blinks | On/Off (ms) | Total Time | Issue | Action |
|--------|-------------|------------|-------|--------|
| **3** | 300/300 | ~2.4 s | No network registration | Check coverage area, antenna, SIM IoT plan (LTE-M/NB-IoT) |
| **5** | 300/300 | ~3.6 s | No IP address | PDP context failed — check APN and data plan |

**RGB values:** R=255, G=100, B=0

**Note:** Orange blinks use **slow** timing (300 ms on/off) to distinguish from the faster red SIM patterns.

---

### Modem Issues — MAGENTA

| Blinks | On/Off (ms) | Total Time | Issue | Action |
|--------|-------------|------------|-------|--------|
| **2** | 200/200 | ~1.4 s | Modem firmware unreadable | Modem may need reflash or recovery |
| **4** | 200/200 | ~2.2 s | CEREG format config failed | Modem state issue — reboot device |

**RGB values:** R=255, G=0, B=200

---

### Power Saving Issues — CYAN

| Blinks | On/Off (ms) | Total Time | Issue | Action |
|--------|-------------|------------|-------|--------|
| **2** | 200/200 | ~1.4 s | PSM not granted | Operator may not support PSM — higher battery drain expected |
| **3** | 200/200 | ~1.8 s | eDRX not supported | Operator may not support eDRX — informational |

**RGB values:** R=0, G=200, B=200

**Note:** These are **warnings**, not critical errors. The device operates normally without PSM/eDRX but will consume more power.

---

### Internet Access Issues — YELLOW

| Blinks | On/Off (ms) | Total Time | Issue | Action |
|--------|-------------|------------|-------|--------|
| **3** | 200/200 | ~1.8 s | DNS resolution failed | Check firewall/NAT settings on IoT plan |

**RGB values:** R=255, G=200, B=0

---

### Signal Quality Warning — WHITE

| Blinks | On/Off (ms) | Total Time | Issue | Action |
|--------|-------------|------------|-------|--------|
| **2** | 400/400 | ~2.2 s | Weak signal (RSRP < −120 dBm) | Relocate device or improve antenna placement |
| **4** | 400/400 | ~4.0 s | Poor quality (RSRQ < −15.0 dB) | High interference — relocate device; RSRQ low even if RSRP is adequate |

**RGB values:** R=200, G=200, B=200

**Note:** Two patterns exist for signal issues:
- **2 blinks** = RSRP (signal strength) too low — not enough signal reaching the antenna.
- **4 blinks** = RSRQ (signal quality) too low — signal present but heavily interfered. A device can have adequate RSRP but poor RSRQ due to nearby interference, resulting in the same unreliable connectivity.

---

### All Pass — GREEN

| Blinks | On/Off (ms) | Total Time | Meaning |
|--------|-------------|------------|---------|
| **3** | 200/200 | ~1.5 s | All self-test checks passed |

**RGB values:** R=0, G=255, B=0

---

## Normal Operation LED Patterns

These patterns are shown briefly during application state transitions, **after** the self-test LED window has expired.

| Colour | RGB | On/Off (ms) | Reps | State | Description |
|--------|-----|-------------|------|-------|-------------|
| **Yellow** | 255/255/0 | 50/2000 | 3 | Idle | Waiting for cloud connection |
| **Green** (dim) | 0/55/0 | 50/2000 | 3 | Sampling | Collecting sensor data |
| **Blue** (dim) | 0/0/55 | 50/2000 | 3 | Wait for trigger | Sleeping between sample intervals |
| **Purple** | 160/32/240 | 250/2000 | ∞ | FOTA download | Firmware update in progress |

Normal operation patterns are very brief (50 ms on, 2 second off) to minimize power consumption.

---

## Quick Reference Chart

```
STARTUP (first ~5 minutes):

  RED  (fast)   = SIM problem     (count blinks: 2=missing, 3=PIN, 4=ICCID, 5=IMSI)
  ORANGE (slow) = Network problem (count blinks: 3=no registration, 5=no IP)
  MAGENTA       = Modem problem   (count blinks: 2=firmware, 4=CEREG)
  CYAN          = PSM/eDRX issue  (count blinks: 2=PSM, 3=eDRX)
  YELLOW        = DNS failed      (3 blinks)
  WHITE (slow)  = Signal issue    (count blinks: 2=RSRP weak, 4=RSRQ poor)
  GREEN         = All OK          (3 blinks)

NORMAL OPERATION:

  YELLOW (dim, brief) = Idle / waiting for connection
  GREEN  (dim, brief) = Sampling sensor data
  BLUE   (dim, brief) = Sleeping between samples
  PURPLE (ongoing)    = FOTA download active
```

---

## Pattern Display Order

When multiple self-test errors are present, patterns are displayed in this fixed order:

1. SIM not detected (RED × 2)
2. SIM PIN required (RED × 3)
3. ICCID read fail (RED × 4)
4. IMSI read fail (RED × 5)
5. Modem FW read fail (MAGENTA × 2)
6. No network registration (ORANGE × 3)
7. No IP address (ORANGE × 5)
8. PSM not granted (CYAN × 2)
9. eDRX not supported (CYAN × 3)
10. DNS resolution fail (YELLOW × 3)
11. CEREG format fail (MAGENTA × 4)
12. Weak signal RSRP (WHITE × 2)
13. Poor signal quality RSRQ (WHITE × 4)

Each pattern is followed by a **600 ms pause** (LED off) before the next pattern starts.

**Maximum total display time** for all 12 patterns simultaneously: ~24 seconds. The 60-second window is more than sufficient.

---

## Power Considerations

- Self-test LED signaling runs **once** at startup, never again.
- The LED window is hard-limited to **60 seconds** (configurable via `CONFIG_APP_SELFTEST_LED_DURATION_SECONDS`). Set to `0` to disable LED signaling entirely.
- Normal operation LED flashes are **50 ms on, 2000 ms off** — negligible power impact.
- After the self-test window, the self-test thread exits permanently with zero ongoing power cost.
- During sleep (PSM), no LEDs are active.

---

## Configuration

| Kconfig Option | Default | Description |
|---------------|---------|-------------|
| `CONFIG_APP_SELFTEST` | `y` | Enable/disable entire self-test module |
| `CONFIG_APP_SELFTEST_LED_DURATION_SECONDS` | `60` | LED signaling window (0 = disable LEDs) |
| `CONFIG_APP_SELFTEST_SIM_WAIT_SECONDS` | `240` | Max SIM/network wait (4 minutes) |
| `CONFIG_APP_LED` | `y` | Enable/disable LED module entirely |
