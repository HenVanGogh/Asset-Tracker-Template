# nRF91 → Gateway Usage Guide

**Audience:** the agent / firmware running on the nRF9151 (asset-tracker app)
that talks to the nRF5340 ESL gateway over the SPI shell-bridge.

**Why this guide exists:** the nRF5340 shell accepts free-form commands but
the underlying BLE protocol has hard timing rules and the BT/PAwR controller
only has 4 ACL slots and one PA train. Sending the wrong command at the wrong
time causes silent failures (no response), `EBUSY (-16)` errors, dropped
PAwR events, or stale data in MQTT. Follow this guide and these problems go
away.

---

## 1. Mental model — what the gateway can do

| Subsystem | Resources | Time cost | Notes |
|-----------|-----------|-----------|-------|
| **PAwR** (always on once started) | 1 PA train, 30 subevent groups, 1 response slot per group | 1.76 s interval | Only one TX per group per interval |
| **ACL provisioning** | up to 4 parallel slots | ~4 s per sensor (connect → discover → keys → PAST → disconnect) | Disrupts PAwR slightly |
| **Active scan** | mutually exclusive with ACL connect | up to 30 s per oneshot | Hard cap of 3 s when sensors are already synced |
| **Sensor whitelist** | flash-backed, max 32 entries | instant | Survives reboot |

Implication: the gateway can be polling 30 sensors **and** onboarding 4 new
ones at the same time, but it **cannot** be scanning while doing either —
scan blocks the radio. The shell will accept overlapping commands but they
will queue or fail silently. **Schedule them serially from nRF91 side.**

---

## 2. Boot-time sequence (do once)

```
1.  Wait 3 s after reset for nRF5340 to come up (look for first
    spi1_mgr "shell rc=0" line, or your own SPI link-up notify).

2.  AP_SET_TIME:<unix_epoch>           ← raw cmd, NOT prefixed with "esl_c"
    Confirms: #AP_EPOCH_SET:<epoch>

3.  esl_c pawr start_pawr              ← starts the PA train
    Confirms: nothing explicit — but next "scan" warns "Scan while sensors
    active" once any sensor is synced.

4.  sensor_mgmt status                 ← read whitelist state from flash
    Confirms: #SENSOR_MGMT:STATUS,MODE=<m>,COUNT=<n>,PROVISIONED=<0|1>

5.  IF the gateway returns PROVISIONED=1 → DO NOT scan. Skip to §5
    "steady-state polling". The fleet is already on flash.

6.  IF PROVISIONED=0 → first-boot provisioning needed (one-time):
    a.  sensor_mgmt mode whitelist
    b.  for each known MAC:
        sensor_mgmt add AA:BB:CC:DD:EE:FF
    c.  sensor_mgmt provision_done
        Confirms: #SENSOR_MGMT:PROVISIONED,COUNT=<n>,MODE=whitelist
```

**Do this once at boot. Do not repeat unless you reset the gateway.**

---

## 3. Onboarding a sensor (when you actually need to)

You only need to scan & connect when:

- A sensor in the whitelist is NOT yet bonded to the gateway (no `#SENSOR_SYNCED:`
  printed since boot, or after `esl_c sensor_registry` shows `synced=0`).
- A new sensor is being added to the fleet for the first time.

**Otherwise — DO NOT SCAN.** You will see in your logs:

```
<wrn> esl_c: Scan while sensors active — PA disruption possible (3s cap)
#SCAN:warn_active_sensors
```

This is not just a warning — it actually steals radio time from the
already-synced sensors and may cause `#PAWR_GAP:` events.

### Correct onboarding sequence (per sensor)

```
1.  Confirm tag is in the whitelist:
    sensor_mgmt list                   → look for the MAC

2.  esl_c acl scan 1 1                 ← oneshot scan, 30 s window when no
                                         sensors synced, 3 s when some are
    Wait for ONE of:
      #TAG_SCANNED: <n>,<MAC> to list  → success, gateway will auto-connect
      #SCAN_SUMMARY:oneshot_match      → success
      #SCAN:timeout_30s OR #SCAN:timeout_3s → tag not in range, give up
                                         this round — DO NOT retry
                                         immediately, wait at least 30 s

3.  Gateway auto-runs: connect → GATT discover → key push → PAST → disconnect
    Watch for:
      #PAST_DONE_DISCONNECT             → success
      #SENSOR_SYNCED:0x<addr>           → tag now on PAwR
    Total time: ~4 s per tag.

4.  Once #SENSOR_SYNCED arrives, immediately:
    esl_c nus get_name 0x<addr>         ← cache the human-readable name
                                          BEFORE first poll (see §4 caveat)

5.  Done. Tag will be auto-polled on the next poll round.
```

### Multi-sensor onboarding throttle

The gateway has 4 parallel ACL slots. If you trigger more than 4 scans
back-to-back before they finish, the 5th fails with `-16 (EBUSY)`. Pace
yourself:

```
For each pending sensor:
    issue scan
    wait for #SENSOR_SYNCED OR #SCAN:timeout
    move to next
```

Or if you want parallel: only ever have ≤ 4 outstanding scans.

---

## 4. The address rules (this is what bit you)

Every NUS-over-PAwR command needs a **unicast** ESL address. The address is
a 16-bit value: `(group_id << 8) | low_byte`. Group 0 / low 1 = `0x0001`.

| Command | Addr arg required? | Default if missing | Notes |
|---------|-------------------|--------------------|-------|
| `nus get_name` | yes (recommended) | `0x0001` (single-sensor compat) | Sensor will NOT respond to broadcast 0x0000 |
| `nus status` | yes | `0x0000` (broadcast → silent) | Same: must be unicast |
| `nus sensors` | yes | `0x0000` (broadcast → silent) | Same: must be unicast |
| `nus config` | yes | `0x0000` (broadcast → silent) | Same |
| `nus set_time` | no | broadcast | Time-set is intentionally broadcast |
| `nus get_time` | yes | `0x0000` (silent) | unicast |
| `nus reset` | yes | broadcast (DANGEROUS) | Specify or it resets all tags |
| `nus led <idx> <st>` | optional | `0x0000` (broadcast = all tags) | OK to broadcast |

### Wrong: what nRF91 was doing
```
esl_c nus status              ← addr defaults to 0x0000, sensor ignores
esl_c nus status 2            ← addr=0x0002 = nonexistent tag, no response
esl_c nus get_name            ← used to default 0x0000 silent; now defaults
                                0x0001 with a printed warning
```

### Right
```
esl_c nus status   0x0001     ← unicast to the actual tag
esl_c nus sensors  0x0001
esl_c nus get_name 0x0001
esl_c nus get_time 0x0001
```

When more sensors arrive: build the addr from the registry (`esl_c
sensor_registry` or the `#SENSOR_SYNCED:` events you saw), do not invent
indexes.

---

## 5. Steady-state polling (no manual NUS needed in most cases)

The gateway has **automatic** sensor reporting once the first tag is synced:

```
#SENSOR_REPORTING:STARTED,interval=300s
```

It walks the registry round-robin every 300 s and emits:

```
#PAWR_POLL:TEMP,0x0001
#SENSOR_REPORT:TEMP,0x0001,27.18
#PAWR_POLL:BATT,0x0001
#SENSOR_REPORT:BATT,0x0001,4132
#POLL_ROUND_DONE:next=300s
```

This is the data you should publish to MQTT. Do **not** spam manual `nus
sensors` requests — they conflict with the auto poll and cause `-16 EBUSY`.

If you want a faster cadence:
```
esl_c sensor_report_start 60000     ← 60 s instead of 300 s
esl_c sensor_report_stop
esl_c sensor_report_status          ← shows last_temp / last_batt cache
```

Lower bound is ~10 s per tag because each poll uses 2 PA intervals
(TEMP then BATT, ≥1.76 s apart).

---

## 6. When to use which command

| You want… | Send | Result event |
|-----------|------|--------------|
| Periodic temp/batt of all known tags | nothing — auto-runs | `#SENSOR_REPORT:TEMP/BATT,0x<addr>,<value>` every 300 s |
| Friendly name for a tag (once, after sync) | `esl_c nus get_name 0x<addr>` | `#SENSOR_NAME:0x<addr>,<name>` |
| Tag uptime + battery + flags | `esl_c nus status 0x<addr>` | `#NUS_STATUS:ADDR=0x<addr>,UP=<s>,BATT=<mV>,FLAGS=0x<f>` |
| Snapshot temp+batt+rd_count | `esl_c nus sensors 0x<addr>` | `#NUS_SENSORS:ADDR=0x<addr>,TEMP=<v>,BATT=<mV>,CNT=<n>` |
| Push UTC to all tags (on epoch change) | `esl_c nus set_time <epoch>` | `#NUS_SET_TIME:EPOCH=…,ANCHOR_UP=…` per tag |
| Discover new sensor in range | `esl_c acl scan 1 1` | `#TAG_SCANNED:` then `#SENSOR_SYNCED:` |
| What's in my whitelist | `sensor_mgmt list` | `#SENSOR_MGMT:ENTRY=…` then `#SENSOR_MGMT:LIST_DONE,COUNT=<n>` |
| What's currently synced | `esl_c sensor_registry` | one `#SENSOR_REG:…` line per tag |

---

## 7. Things that will silently fail or break PAwR

1. **Scanning when sensors are synced** without a real reason. Each scan
   steals up to 3 s of PA-train air-time. If you must scan every minute
   you will see `#PAWR_GAP:` lines climb. Scan at most every few minutes
   and only when you have a reason (a tag missing from `sensor_registry`
   that should be there, or onboarding).
2. **Sending NUS to broadcast 0x0000**. Sensor's `set_pawr_response` logs
   `"All broadcast TLV or no addressed TLV, no need replied"` and does
   nothing. The shell still returns `rc=0` because the cmd was accepted —
   the failure is invisible from your side unless you wait for the response
   event and time it out.
3. **Two NUS cmds in <1.76 s**. The second one returns `-16 (EBUSY)`. Wait
   for the first response (or for `#NUS_RSP:CMD=0x..,STATUS=…`) before
   sending the next.
4. **`nus reset` to broadcast**. Resets every tag. Use only with explicit
   addr.
5. **Re-issuing `AP_SET_TIME` repeatedly**. Each one is broadcast to all
   synced tags. Once at boot, then only when the epoch jumps (NTP correction
   etc.). Sensor will refuse with `BUSY` if epoch already set this boot.
6. **Onboarding more than 4 sensors in parallel**. Slot 5 returns `EBUSY`.
   Pace yourself.

---

## 8. Recommended event-handler skeleton (nRF91 side)

```c
static void on_gateway_line(const char *line)
{
    /* per-sensor data — publish to MQTT */
    if (starts_with(line, "#SENSOR_REPORT:TEMP,")) handle_temp(line);
    else if (starts_with(line, "#SENSOR_REPORT:BATT,")) handle_batt(line);
    else if (starts_with(line, "#SENSOR_NAME:0x"))      handle_name(line);
    else if (starts_with(line, "#NUS_STATUS:ADDR=0x"))  handle_status(line);
    else if (starts_with(line, "#NUS_SENSORS:ADDR=0x")) handle_sensors(line);

    /* lifecycle */
    else if (starts_with(line, "#SENSOR_SYNCED:0x")) {
        uint16_t addr = parse_hex(line + 16);
        /* immediately request the friendly name (one-time per session) */
        send_cmd("esl_c nus get_name 0x%04x", addr);
    }
    else if (starts_with(line, "#SENSOR_DESYNC:0x")) {
        /* tag dropped — flag it in MQTT, do NOT auto-rescan; wait for
         * automatic re-sync via PAwR (sensor side will retry every ~1 s) */
    }
    else if (starts_with(line, "#PAST_DONE_DISCONNECT")) {
        /* a tag finished onboarding — name request will be sent on
         * the #SENSOR_SYNCED line that follows */
    }

    /* mgmt */
    else if (starts_with(line, "#SENSOR_MGMT:STATUS,"))   handle_mgmt_status(line);
    else if (starts_with(line, "#SENSOR_MGMT:PROVISIONED")) provisioning_complete = true;

    /* warnings worth surfacing */
    else if (starts_with(line, "#PAWR_GAP:"))             telemetry_inc("pawr_gap");
    else if (starts_with(line, "#PAWR_TX_FAIL:"))         telemetry_inc("pawr_tx_fail");
    else if (starts_with(line, "#SCAN:warn_active_sensors")) {
        /* you scanned while sensors are live — re-evaluate whether
         * the scan was actually necessary */
    }
}
```

### NUS request scheduler (rate-limit)

```c
/* Don't fire NUS cmds faster than 1 per 2 s — gives PAwR slot time to
 * deliver the response and avoids -16 EBUSY. */
static int64_t last_nus_at;

int gw_nus(uint16_t addr, const char *sub) {
    int64_t now = k_uptime_get();
    if (now - last_nus_at < 2000) {
        k_msleep(2000 - (now - last_nus_at));
    }
    last_nus_at = k_uptime_get();

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "esl_c nus %s 0x%04x", sub, addr);
    return uart_sensor_esl_command(cmd);
}
```

### Scan scheduler (back-off)

```c
/* At most one scan every 5 min unless we have a real reason. */
static int64_t last_scan_at;
#define SCAN_MIN_INTERVAL_MS  (5 * 60 * 1000)

bool gw_should_scan(void) {
    return any_whitelist_entry_unsynced() &&
           (k_uptime_get() - last_scan_at) > SCAN_MIN_INTERVAL_MS;
}

int gw_scan(void) {
    if (!gw_should_scan()) return -EAGAIN;
    last_scan_at = k_uptime_get();
    return uart_sensor_esl_command("acl scan 1 1");
}
```

---

## 9. Quick fault-finding

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| `shell rc=0` but no event arrives within 3 s | NUS cmd was broadcast (addr=0x0000) — sensor ignored | re-send with explicit `0x<addr>` |
| `shell rc=-16` | EBUSY: sync_buf in use by previous NUS or PAwR poll | wait 2 s, retry once |
| `#PAWR_GAP:skipped=2` once every few minutes | RF noise or scan happening | reduce scan frequency, ignore single events |
| `#PAWR_GAP:skipped≥10` | gateway radio was blocked (long scan, ACL conn stall) | check what command preceded it |
| `#SENSOR_DESYNC:0x<addr>` | tag out of range OR sensor crashed/reset | wait for auto-resync, or `nus status` to probe |
| MQTT never sees TEMP/BATT but `#SENSOR_REPORT:` shows on RTT | nRF91 parser bug (see `NRF91_FIX_MEMO_SENSOR_REPORT.md`) | apply that fix |
| `sensor_mgmt status` returns nothing | check RTT for `notify[..]: #SENSOR_MGMT:STATUS,…` — if present, parser issue on nRF91 | fix nRF91 parser |

---

## 10. Summary — the 6 rules

1. **Onboard once**, then trust the whitelist + auto-poll.
2. **Scan only when needed** (a whitelist entry isn't synced yet).
3. **Always include `0x<addr>`** for NUS commands.
4. **Pace NUS commands**: ≥ 2 s between successive `esl_c nus …` cmds.
5. **Don't fight the auto-poll**: if you want faster data use
   `sensor_report_start <ms>`, don't spam manual `nus sensors`.
6. **Listen for `#SENSOR_SYNCED:`** to drive any per-tag setup
   (name fetch, status check, etc.) — don't drive on a wall-clock timer.
