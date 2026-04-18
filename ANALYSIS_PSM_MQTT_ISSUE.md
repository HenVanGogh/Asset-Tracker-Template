# Analysis: PSM Sleep & Missing MQTT Commands

## Executive Summary

The gateway nRF9151 is likely **entering Power Saving Mode (PSM)** after 6 seconds of inactivity, causing **MQTT commands sent while sleeping to be silently lost** with no confirmation. The device only processes commands during brief 6-second active windows before returning to sleep mode.

---

## Root Cause Analysis

### 1. PSM Configuration (CRITICAL)

**File**: `/app/prj.conf`

```
CONFIG_LTE_PSM_REQ=y                          # PSM enabled
CONFIG_LTE_PSM_REQ_RAT_SECONDS=6              # ⚠️ ONLY 6 seconds active time
CONFIG_LTE_PSM_REQ_RPTAU_SECONDS=7200         # Sleep for 2 hours between TAU updates
CONFIG_LTE_EDRX_REQ=y                         # Extended DRX enabled (5.12s)
```

### 2. Device Lifecycle During PSM

```
Timeline per PSM cycle:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

AWAKE (6 seconds)
├─ LTE connection active
├─ MQTT subscribed and receiving messages  ✅
├─ Commands processed immediately
└─ Responses published

ENTERS PSM SLEEP (after 6 seconds)
├─ Modem powered down
├─ NO incoming MQTT processing            ❌
├─ Commands sent = LOST
└─ No Keep-Alive or ping mechanism

SLEEPS until TAU expires
├─ Default TAU: 7200 seconds (2 hours)   ← Very long!
├─ OR: Network forces wakeup
└─ Then repeats cycle
```

### 3. Why Commands Are Not Received

| When Command Sent | Device State | Outcome |
|---|---|---|
| During 6-sec awake window | **CONNECTED** | ✅ Processed immediately, response sent |
| During PSM sleep period | **ASLEEP** | ❌ **Lost silently** |
| Just after entering PSM | **Transitioning** | ❌ Partial/no processing |

**There is NO MESSAGE QUEUE or BUFFERING** for incoming MQTT commands while asleep. This is standard MQTT behavior—subscribed messages are not retained for offline clients in normal broker configurations.

---

## Architecture Evidence

### MQTT Command Handler Chain

**File**: `app/src/modules/custom_mqtt/custom_mqtt.c` (lines 314-900+)

```c
mqtt_evt_handler()
├─ case MQTT_EVT_PUBLISH:  // ← Incoming command
│  ├─ mqtt_read_publish_payload_blocking()  // Read from socket
│  ├─ cJSON_Parse()  // Parse JSON
│  ├─ Process command (esl_set_expected_tags, etc.)
│  └─ cJSON_Print() + publish response
│
└─ Command callback ONLY fires if MQTT_EVT_PUBLISH received
   └─ Which requires ACTIVE LTE connection
      └─ NOT available during PSM sleep ❌
```

### No Local Command Queue

**Kconfig**: `app/src/modules/custom_mqtt/Kconfig.custom_mqtt`

```c
config APP_CUSTOM_MQTT_MESSAGE_QUEUE_SIZE
	int "Message queue size"
	default 10
```

⚠️ This queue is for **internal MQTT module processing**, **NOT** for buffering incoming messages during sleep. Once the modem sleeps, socket receives become unavailable.

### Network State Management

**File**: `app/src/modules/network/network.c` (lines 200-244)

```c
case LTE_LC_EVT_PSM_UPDATE:
    LOG_DBG("PSM parameters received, TAU: %d, Active time: %d",
        msg.psm_cfg.tau, msg.psm_cfg.active_time);
```

Network module **tracks PSM state** but **does NOT** prevent or handle PSM during active MQTT subscriptions. There's no keep-alive or "wake-on-command" mechanism.

---

## Symptom Confirmation Checklist

You're experiencing this issue if:

- [ ] You send a command to `gateway/gateway_XXXX/command` and **get no response**
- [ ] But if you send it **again immediately after**, it sometimes works
- [ ] The device publishes periodic heartbeats (proving it's still alive)
- [ ] But commands don't trigger responses unless sent during specific windows
- [ ] No errors in logs when commands are "lost"
- [ ] Heartbeat interval correlates with the 6-second active window timing

---

## Why No Error/Confirmation?

**When device is asleep:**

1. Command reaches MQTT broker ✓
2. Broker tries to deliver to subscribed client
3. Connection appears valid to broker (no disconnect), but modem is in PSM
4. Delivery attempt fails **silently** (standard MQTT TLS socket behavior)
5. Broker **does NOT** retry or buffer (absent MQTT QoS+retain setup)
6. Device **never sees** the command ✗
7. Device **never sends** a response ✗

---

## Current Configuration Analysis

### Good Settings:
- ✅ Keep-alive: 60 seconds
- ✅ Payload buffer: 512 bytes
- ✅ Command handler exists and works when awake

### Problem Settings:

| Setting | Current | Issue |
|---------|---------|-------|
| **PSM Active Time** | 6 seconds | **TOO SHORT** for reliable MQTT command delivery |
| **PSM TAU** | 7200 seconds (2 hrs) | Device unreachable for long periods |
| **eDRX** | 5.12 seconds | Still very short for command injection |
| **Message Buffering** | None | Commands lost if not received during awake window |
| **Command Acknowledgment** | No handshake | No way to confirm "device received it" |

---

## Impact Assessment

### For `esl_set_expected_tags` Command:

```json
{"command": "esl_set_expected_tags", "count": 3}
```

**Expected flow:**
1. Device awake + subscribed
2. Receives JSON → parses → sets expected tags
3. Publishes response `{"status": "ok", "expected_tags": 3}`

**Actual flow (if asleep):**
1. Device in PSM sleep
2. Command discarded by browser/modem stack
3. **No response** generated
4. You see no confirmation or error

---

## Technical Details: Why This Happens

### Nordic nRF9151 PSM Design

The nRF9151 modem's PSM implementation:
- Modem hardware enters ultra-low-power mode
- All network I/O blocked
- Application cannot **receive** socket events
- **Only wakes on**: TAU expired, NAS paging, or explicit application request

### MQTT Broker Behavior

Standard MQTT brokers **do not queue** messages for sleeping clients unless:
- Session resumption enabled (MQTT3.1.1 persistent sessions)
- Clean session = false
- BUT: Still no guarantee—depends on broker implementation

---

## References in Codebase

### PSM Configuration
- File: `app/prj.conf` (lines 61-80)
- PSM state tracking: `app/src/modules/network/network.c` (lines 225-235)

### MQTT Command Reception
- File: `app/src/modules/custom_mqtt/custom_mqtt.c` (lines 314-400)
- Handler: `mqtt_evt_handler()` function
- Event: `MQTT_EVT_PUBLISH` case

### No Buffering/Queue Protection
- File: `app/src/modules/custom_mqtt/custom_mqtt.c` (no incoming message queue)
- Config: `Kconfig.custom_mqtt` (message queue is internal only)

---

## Verification Steps

### Test 1: Confirm PSM Timing

Send these commands in sequence:

```bash
# Command A - should work if device just woke
mosquitto_pub -h <broker> -t gateway/gateway_XXXX/command \
  -m '{"command": "get_status"}'

# Wait 5 seconds (still in awake window)
sleep 5

# Command B - likely to work
mosquitto_pub -h <broker> -t gateway/gateway_XXXX/command \
  -m '{"command": "esl_set_expected_tags", "count": 3}'

# Wait 10 seconds (device enters PSM after 6 sec)
sleep 10

# Command C - likely FAILS (device asleep)
mosquitto_pub -h <broker> -t gateway/gateway_XXXX/command \
  -m '{"command": "esl_set_expected_tags", "count": 3}'
```

**Expected Result:**
- Commands A & B: Response within 1-2 seconds
- Command C: **NO RESPONSE**

### Test 2: Monitor Heartbeat

Check device publish topic for heartbeat frequency:
```bash
mosquitto_sub -h <broker> -t gateway/gateway_XXXX/data
```

If heartbeat arrives every ~5-6 seconds, that's the active window.

---

## Current State Summary

**The device is working as designed, but the design is incompatible with on-demand MQTT commands:**

✓ **Device can:**
- Connect to MQTT broker
- Receive commands while awake (6-sec window)
- Process commands immediately
- Publish responses

✗ **Device cannot:**
- Reliably receive commands at any time
- Get commands while in PSM sleep
- Buffer/queue missed commands
- Provide feedback when commands are lost

---

## Questions This Raises

1. **How are you sending commands to the device?** (Script? Dashboard? Manual?)
2. **When do you send them relative to device wakeup?** (Random time? Timed?)
3. **What response are you expecting?** (Log messages? MQTT response?)
4. **Are you seeing any error logs?** (Or just silent failures?)
5. **Is the device actually entering PSM?** (Can check modem logs?)

---

## Next Steps (For User Decision)

To truly fix this requires choosing between:

**Option A: Enable "Always Awake" Mode**
- Disable PSM: Set `CONFIG_LTE_PSM_REQ=n`
- Higher power consumption (battery penalty)
- Reliable command reception
- Better for gateways on power

**Option B: Add Client-Side Buffering**
- Keep PSM enabled
- Add local message queue (in flash or RAM)
- Batch commands and execute on next wakeup
- Requires firmware changes

**Option C: On-Device Command Scheduling**
- Publish periodic "are you there?" heartbeat
- Use response to know device is awake
- Batch commands to send immediately after heartbeat

**Option D: Use MQTT features (if broker supports)**
- Enable message retention (`RETAIN` flag)
- Set clean session = false
- But: **Unreliable**—not all brokers support, no standard guarantee

---

## Conclusion

**Your gateway is likely working correctly but is not suitable for on-demand remote command injection while in PSM sleep mode.** This is a fundamental design constraint of LTE PSM on cellular IoT devices, not a bug in the code.

The code correctly:
- ✅ Receives MQTT messages when connected
- ✅ Parses and processes commands
- ✅ Sends responses

But the device design:
- ❌ Enters sleep too aggressively (6-second window)
- ❌ Has no command buffering
- ❌ Has no acknowledgment handshake
