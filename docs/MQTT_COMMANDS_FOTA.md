# MQTT FOTA Commands — Firmware Over-the-Air Update

Commands for triggering, monitoring, and cancelling firmware updates over HTTPS.

Requires `CONFIG_APP_FOTA=y` in the build.

→ Back to [MQTT_COMMANDS.md](MQTT_COMMANDS.md)

---

## Overview

The FOTA subsystem uses Nordic Semiconductor's `fota_download` library to download a firmware
binary over HTTPS and stage it in the MCUboot secondary partition (external flash). After
successful download, the device disconnects from the network and performs a cold reboot.
MCUboot validates the new image and swaps it into the primary slot.

### End-to-end FOTA workflow

```
[You]                          [Device]                          [HTTP server]
  │                               │                                    │
  │ {"command":"fota_start",      │                                    │
  │  "url":"https://webs.org/..."}│                                    │
  ├──────────────────────────────►│                                    │
  │                               │── GET /thingyupdate ──────────────►│
  │ {"status":"fota_starting"}    │                                    │
  │◄──────────────────────────────┤   ← binary stream (app_update.bin) │
  │                               │◄───────────────────────────────────┤
  │                               │  (writing to ext-flash, 0-100%)    │
  │  [periodic fota_status poll]  │                                    │
  │ {"fota":"downloading",        │                                    │
  │  "progress_pct": 42}          │                                    │
  │                               │                                    │
  │ {"fota":"complete_reboot_     │                                    │
  │  pending","progress_pct":100} │                                    │
  │                               │── NETWORK_DISCONNECT ──────────────│
  │                               │── sys_reboot(COLD) ────────────────│
  │                               │                                    │
  │                    [MCUboot validates + swaps image]               │
  │                               │                                    │
  │                     [Device comes back online]                     │
  │ {"status":"online",           │                                    │
  │  "version":"2.1.0"}           │                                    │
```

---

## Building the firmware binary

The `fota_start` command expects an `app_update.bin` — the raw MCUboot-signed application binary.
This is automatically produced by the build system:

```bash
# Build (from Asset-Tracker-Template root)
./flash_docker.sh

# The DFU package is at:
app/build/dfu_application.zip

# Extract the raw binary (served to the device):
cd app/build
unzip -o dfu_application.zip
# → app_update.bin
```

Serve `app_update.bin` as a static file on your HTTPS server.

### Server requirements

- **TLS with a CA-signed certificate** (the device uses the modem's TLS stack with a provisioned root CA)
- File served with a valid `Content-Length` header (required by the downloader)
- No HTTP redirects — the downloader does not follow redirects
- The exact URL passed to `fota_start` must point directly to the binary

> **Nginx example (exact-match location required to avoid trailing-slash redirect):**
> ```nginx
> location = /thingyupdate {
>     alias /var/www/firmware/app_update.bin;
>     add_header Content-Type application/octet-stream;
> }
> ```

### TLS certificate provisioning

The modem needs the server's root CA certificate stored in its credential store.
ISRG Root X1 (Let's Encrypt) is supported with `sec_tag = 16842754`.

See [FOTA_PROVISIONING.md](FOTA_PROVISIONING.md) for the full provisioning procedure.

---

## `fota_start`

Triggers an HTTPS firmware download. The device will:
1. Parse and resolve the URL
2. Start downloading in the background
3. Write the binary to the MCUboot secondary partition (external flash)
4. On completion, disconnect from the network and reboot

### Parameters

| Field | Type | Required | Default | Description |
|---|---|---|---|---|
| `url` | string | **Yes** | — | Full HTTPS (or HTTP) URL to the firmware `.bin` file |
| `sec_tag` | integer | No | `CONFIG_APP_FOTA_SEC_TAG` (16842754) | Modem TLS credential tag for the server root CA |

```json
{
  "command": "fota_start",
  "url": "https://webs.org/thingyupdate"
}
```

```json
{
  "command": "fota_start",
  "url": "https://myserver.example.com/firmware/app_update.bin",
  "sec_tag": 16842754
}
```

### Response — success

```json
{
  "status": "fota_starting",
  "url": "https://webs.org/thingyupdate",
  "sec_tag": 16842754,
  "note": "Device will download and reboot to apply update",
  "command_processed": "fota_start"
}
```

### Response — already downloading

```json
{
  "status": "fota_error",
  "error_code": -16,
  "command_processed": "fota_start"
}
```

`-16` is `-EBUSY` — a download is already in progress.

### Response — missing URL

```json
{
  "status": "missing_url",
  "hint": "{\"command\":\"fota_start\",\"url\":\"https://webs.org/thingyupdate\"}",
  "command_processed": "fota_start"
}
```

### URL parsing

The `fota_download` library requires the host (with scheme) and file path to be passed separately.
The firmware's `parse_url()` function handles this:

- Input: `"https://webs.org/thingyupdate"`
- Host passed to library: `"https://webs.org"` (scheme stays with host)
- File passed to library: `"thingyupdate"` (leading slash stripped — library adds its own separator)

---

## `fota_status`

Query the current status of the FOTA subsystem. Does not affect any in-progress download.

**No parameters required.**

```json
{"command": "fota_status"}
```

### Response — idle (no download in progress)

```json
{
  "status": "ok",
  "fota": "idle",
  "version": "2.0.0",
  "command_processed": "fota_status"
}
```

### Response — download in progress

```json
{
  "status": "ok",
  "fota": "downloading",
  "progress_pct": 57,
  "version": "2.0.0",
  "command_processed": "fota_status"
}
```

`progress_pct` is 0–99 during download.

### Response — download complete, reboot pending

```json
{
  "status": "ok",
  "fota": "complete_reboot_pending",
  "version": "2.0.0",
  "command_processed": "fota_status"
}
```

This state means the binary was fully written to the secondary partition and the device is waiting
for the network to disconnect before rebooting. The reboot happens automatically — there is no need
to send an additional command. If the device hasn't rebooted for an unexpected reason, send
`{"command":"reboot"}` to force it.

---

## `fota_cancel`

Cancel an in-progress download. Has no effect if the device is idle.

**No parameters required.**

```json
{"command": "fota_cancel"}
```

### Response — cancelled

```json
{
  "status": "cancel_requested",
  "command_processed": "fota_cancel"
}
```

The download stops gracefully. The device returns to idle state and no firmware is staged.

### Response — error publishing cancel event

```json
{
  "status": "error",
  "error_code": -11,
  "command_processed": "fota_cancel"
}
```

---

## Monitoring a FOTA download

Poll `fota_status` every 30 seconds to track progress. A typical sequence:

```bash
# 1. Trigger the download
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"fota_start","url":"https://webs.org/thingyupdate"}'

# 2. Poll progress
watch -n 30 mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"fota_status"}'
```

Expected progress on the data topic:

```
{"fota":"downloading","progress_pct":0}
{"fota":"downloading","progress_pct":14}
{"fota":"downloading","progress_pct":28}
...
{"fota":"complete_reboot_pending"}
[device disconnects and reboots — no more MQTT messages for ~10–30 s]
{"status":"online","version":"<new_version>"}
```

---

## After-reboot validation

After the device comes back online, confirm the new firmware version:

```bash
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"mqtt_get_config"}'
```

The response includes `"version": "<APP_VERSION>"` which reflects the running build.

MCUboot writes the image as confirmed (`boot_write_img_confirmed()`) on first boot, preventing
automatic rollback.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `fota_error: -16` | Download already in progress | Send `fota_cancel` first |
| `File size not set` (device log) | HTTP server returned no `Content-Length` | Fix server config; ensure no HTML directory listing |
| `fota_error: -61` or TLS error | Wrong `sec_tag` or certificate not provisioned | Re-provision with correct root CA (see FOTA_PROVISIONING.md) |
| Download starts but stalls | LTE signal weak or HTTP server dropped connection | Retry `fota_start`; check server logs |
| Device reboots but runs old firmware | MCUboot validation failed (bad binary / wrong DFU region) | Check `app_update.bin` is from correct build; verify partition map |
| `fota_start` returns success but nothing happens | `fota_download_start` returned 0 but download didn't proceed | Check device logs; modem may need a moment after LTE attach |

---

## `image_info`

Return the running firmware version and MCUboot image slot state. Use this immediately after
a reboot following a FOTA update to verify the new firmware is running and to determine
whether the image needs confirmation.

**No parameters required. Does not require `CONFIG_APP_FOTA=y`.**

```json
{"command": "image_info"}
```

### Response — confirmed image (permanent, won't roll back)

```json
{
  "status": "ok",
  "version": "2.0.0",
  "confirmed": true,
  "swap_type": "none",
  "command_processed": "image_info"
}
```

### Response — test image (will roll back on next reboot unless confirmed)

```json
{
  "status": "ok",
  "version": "2.0.0",
  "confirmed": false,
  "swap_type": "test",
  "note": "Image not confirmed — will revert on next reboot",
  "command_processed": "image_info"
}
```

### Field reference

| Field | Type | Description |
|---|---|---|
| `version` | string | Running firmware version (`APP_VERSION_STRING`, from `app/VERSION`) |
| `confirmed` | bool | `true` = image is permanent; `false` = image is in test mode and will revert |
| `swap_type` | string | MCUboot swap state: `none`, `test`, `perm`, `revert`, `fail`, `unknown` |
| `note` | string | Present only when `confirmed=false`; actionable hint |

### `swap_type` values

| Value | Meaning |
|---|---|
| `none` | No pending swap — device is running its permanent image |
| `test` | Image was booted in test mode after FOTA — needs `image_confirm` to make permanent |
| `perm` | Image was requested to swap permanently (uncommon) |
| `revert` | MCUboot is in the process of reverting to the previous image |
| `fail` | MCUboot encountered a failure during swap |

---

## `image_confirm`

Permanently confirm the currently running image. After a successful FOTA update MCUboot
boots the new image in **test mode** (`swap_type=test`, `confirmed=false`). If the device
reboots again before the image is confirmed, MCUboot automatically reverts to the old image.

Call `image_confirm` once you have verified the new firmware is working correctly.

**No parameters required. Does not require `CONFIG_APP_FOTA=y`.**

```json
{"command": "image_confirm"}
```

### Response — success

```json
{
  "status": "ok",
  "note": "Image confirmed — will not revert on next reboot",
  "version": "2.0.0",
  "command_processed": "image_confirm"
}
```

### Response — already confirmed

```json
{
  "status": "ok",
  "note": "Image was already confirmed",
  "version": "2.0.0",
  "command_processed": "image_confirm"
}
```

### Response — error

```json
{
  "status": "error",
  "error_code": -5,
  "command_processed": "image_confirm"
}
```

---

## Recommended post-FOTA workflow

```bash
# 1. Start FOTA download
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"fota_start","url":"https://webs.org/thingyupdate"}'

# 2. Wait for device to reboot (watch subscribe topic for "online" status)
mosquitto_sub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/data"

# 3. After reboot, check the new version and swap state
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"image_info"}'
# → expect: "version":"2.0.0", "confirmed":false, "swap_type":"test"

# 4. Confirm the image so it won't roll back
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"image_confirm"}'
# → expect: "status":"ok", "note":"Image confirmed..."

# 5. Verify confirmation
mosquitto_pub -h 217.154.155.83 -p 1883 -u mqttuser -P mqttuser \
  -t "gateway/gateway_XXXX/command" \
  -m '{"command":"image_info"}'
# → expect: "confirmed":true, "swap_type":"none"
```
