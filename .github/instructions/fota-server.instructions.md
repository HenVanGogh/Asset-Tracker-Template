---
applyTo: "server/**,backend/**,tools/server*,scripts/server*,**/fota_server*"
---

# FOTA Server — Agent Instructions

You are implementing the server-side component of the Asset Tracker FOTA
(Firmware Over The Air) system for a Thingy:91X (nRF9151) device.

## System Overview

The device (nRF9151 + MCUboot) downloads firmware updates over HTTPS from this
server and applies them via MCUboot swap.  The update is triggered by an MQTT
command published to the device's command topic.

```
[MQTT Broker]  ←→  [Device nRF9151]
      |
[You: Server]  →  HTTPS →  [Device downloads firmware.bin]
```

## Device-Side Contract

### MQTT Topics (existing, do not change)
- **Data topic** (device → broker): `gateway/gateway_XXXX/data`
- **Command topic** (server → device): `gateway/gateway_XXXX/command`

### MQTT FOTA Commands
All commands are JSON, published to the command topic.

**Start update:**
```json
{"command": "fota_start", "url": "https://your-server.example.com/firmware.bin"}
```
Optional: `"sec_tag": 16842754` overrides the TLS credential tag on the device.

**Check status:**
```json
{"command": "fota_status"}
```
Device responds on the data topic:
```json
{"fota_status": "downloading", "progress": 42}
{"fota_status": "idle"}
{"fota_status": "complete_reboot_pending"}
```

**Cancel:**
```json
{"command": "fota_cancel"}
```

### HTTP File Serving Requirements
The device calls `fota_download_start(host, path, sec_tag, 0, 0)` which sends
a plain HTTP GET to `https://<host><path>`.

- Serve the **raw binary** (`app_update.bin`) — NOT the zip
- File must be accessible at the exact URL sent in `fota_start`
- **NO TRAILING SLASH on the firmware path** — `/thingyupdate` must serve the
  file directly; `/thingyupdate/` is a different URL and will 404 or 400
- **NO REDIRECTS on the firmware path** — the nRF9151 downloader does NOT
  follow HTTP 301/302 redirects correctly; the firmware URL must serve the
  binary directly with HTTP 200
- TLS certificate must be signed by ISRG Root X1 (Let's Encrypt) — the device
  has this CA provisioned at modem sec_tag `16842754`

## KNOWN SERVER BUG (must fix before FOTA works)

**Verified April 2 2026** — `https://t4as.org/thingyupdate` redirects 301 to
`https://t4as.org/thingyupdate/` (trailing slash). That URL returns `200 OK`
but serves `Content-Type: text/html` with no `Content-Length` header — it is
an nginx directory listing page, not the firmware binary.

The nRF9151 downloader aborts with `File size not set` when `Content-Length`
is missing (required for MCUboot DFU target size allocation).

**Root cause**: nginx treats `/thingyupdate` as a directory name and auto-
redirects to the trailing-slash form, then serves an HTML index of the
directory. The `app_update.bin` file is likely placed inside a folder named
`thingyupdate/` on the server instead of being served directly as the
`/thingyupdate` path.

**Fixes (pick one)**:

### Fix A — nginx exact-match location (recommended)
```nginx
server {
    listen 443 ssl;
    server_name t4as.org;
    ssl_certificate     /etc/letsencrypt/live/t4as.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/t4as.org/privkey.pem;

    # Exact match — no trailing slash redirect, serves binary directly
    location = /thingyupdate {
        alias /var/www/fota/app_update.bin;
        add_header Content-Type application/octet-stream;
        add_header Content-Disposition "attachment; filename=app_update.bin";
    }
}
server {
    listen 80;
    server_name t4as.org;
    return 301 https://$host$request_uri;
}
```

Note: `location =` (exact match) does NOT redirect to a trailing slash.
`location /thingyupdate` (prefix match) would auto-redirect to `/thingyupdate/`.

### Fix B — use a `.bin` extension in the URL
Place `app_update.bin` at `/var/www/html/thingyupdate.bin` and use:
```json
{"command": "fota_start", "url": "https://t4as.org/thingyupdate.bin"}
```
nginx serves files with extensions directly without the trailing-slash redirect.

### Fix C — Caddy (simplest, handles TLS automatically)
Replace nginx with Caddy:
```caddyfile
t4as.org {
    handle /thingyupdate {
        respond * "not found" 404
    }
    file_server /thingyupdate {
        root /var/www/fota
        index app_update.bin
    }
}
```

**After fixing, verify with curl before sending the MQTT command**:
```bash
# All three must pass:

# 1. No redirect (must be 200, not 301)
curl -sv --no-location https://t4as.org/thingyupdate -o /tmp/fw.bin 2>&1 | grep "< HTTP"
#    Expected: < HTTP/2 200

# 2. Content-Type is binary, Content-Length is set
curl -sI https://t4as.org/thingyupdate | grep -iE "content-type|content-length"
#    Expected: content-type: application/octet-stream
#    Expected: content-length: <size in bytes>

# 3. File size matches build artifact
wc -c /tmp/fw.bin
wc -c /path/to/app_update.bin   # must match
```

## Firmware Artifact

Build output:
```
app/build/dfu_application.zip      # nrfutil package (for tooling)
app/build/dfu_application.zip_manifest.json
```

Extract the binary to serve:
```bash
cd app/build && unzip -o dfu_application.zip
# Produces: app_update.bin  ← this is what the device downloads
```

The binary is signed by MCUboot's key (`app/sysbuild/mcuboot/`) so the device
verifies its signature before applying.

## Server Architecture

Implement a minimal server with these components. Keep it simple and stateless.

### 1. Firmware File Server (HTTPS)
- Serve `app_update.bin` at a configurable path
- Must use a Let's Encrypt TLS cert (so the device can verify it)
- Recommend: nginx / caddy / any static file server behind Let's Encrypt
- Example path: `https://t4as.org/thingyupdate`

### 2. MQTT Publisher (optional automation)
If you add a CI/CD trigger or web UI for publishing updates:
- Connect to MQTT broker at `217.154.155.83:1883`
- Username: `mqttuser`, Password: `mqttuser`
- Publish `fota_start` command to `gateway/gateway_XXXX/command`

### 3. OTA Workflow Automation (optional)
Script that:
1. Receives built `dfu_application.zip` (from CI or manual upload)
2. Extracts `app_update.bin`
3. Copies binary to the web root
4. Publishes `fota_start` MQTT command to the device
5. Subscribes to data topic and monitors progress until `complete_reboot_pending`

## Security Considerations

- The MQTT broker (`217.154.155.83:1883`) is plaintext — do not send sensitive
  data through it. The firmware binary itself does not need to be secret (it is
  signed and verified by MCUboot).
- For production: enable MQTT TLS (`CONFIG_APP_CUSTOM_MQTT_USE_TLS=y`) and
  provision the MQTT broker cert at a separate sec_tag.
- Do NOT expose the firmware endpoint with directory listing enabled.
- Rate-limit the firmware endpoint to prevent DoS-triggered device restarts.

## Tech Stack Preferences

Use the simplest viable approach:
- **Static serving**: nginx/caddy with Let's Encrypt (certbot / Caddy auto-TLS)
- **MQTT client**: `paho-mqtt` (Python) or `mosquitto_pub` for simple scripts
- **Automation**: Python script or shell — no frameworks needed
- **Do not** add auth on the firmware endpoint (binary is cryptographically
  signed; adding HTTP auth would break the device download)

## File Naming Convention

```
server/
  serve_firmware.py     # MQTT trigger + file copy automation
  nginx.conf            # or Caddyfile
  deploy.sh             # build → extract → deploy one-liner
  README.md
```

## Partition Layout Reference (do not change)

These values are frozen in `app/pm_static_thingy91x_nrf9151_ns.yml`.
The secondary slot must match the primary exactly.

| Partition         | Location | Address   | Size   |
|-------------------|----------|-----------|--------|
| mcuboot_primary   | internal | 0x30000   | 832 KB |
| mcuboot_secondary | ext QSPI | 0x00000   | 832 KB |
| settings_storage  | ext QSPI | 0x4D0000  | 8 KB   |

Maximum firmware image size: **832 KB** (0xD0000 bytes).

## Cert Provisioning (already done on target device)

Before FOTA works the device needs the ISRG Root X1 cert at sec_tag 16842754.
See `docs/FOTA_PROVISIONING.md` for the full step-by-step procedure.

## Build Command

```bash
# From project root (builds + flashes via JLink):
./flash_docker.sh

# Build only (no device required):
docker run --rm -v "$(pwd)/app":/workspace/app -w /workspace/app \
  ncs-v2.6.1-debian west build -d build
```

## Do Not Break

- `app/src/modules/custom_mqtt/custom_mqtt.c` — existing MQTT commands and
  all non-FOTA device functionality must continue to work unchanged
- `app/pm_static_thingy91x_nrf9151_ns.yml` — partition addresses are fixed;
  any change requires a full JLink re-flash
- MQTT broker address / credentials in `app/prj.conf` — do not modify
- The device sends telemetry to the data topic independently of FOTA state
