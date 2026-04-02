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
- **Data topic** (device → broker): `gateway/thingy91x-asset-tracker/data`
- **Command topic** (server → device): `gateway/thingy91x-asset-tracker/command`

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

**Verified April 2 2026** — both `http://t4as.org/thingyupdate` and
`https://t4as.org/thingyupdate` currently redirect to:
```
http://t4as.org:443/thingyupdate/
```
This URL is **wrong in three ways**:
1. Scheme is `http://` but port is 443 — the device connects plaintext to a
   TLS port and receives HTTP 400 Bad Request
2. A trailing slash was added to the path (`/thingyupdate/` ≠ `/thingyupdate`)
3. This is a redirect at all — redirects are not followed correctly

**Root cause**: the HTTP→HTTPS redirect rule in nginx (or equivalent) is
emitting `http://` with port 443 in the `Location` header instead of `https://`
without an explicit port.

**Fix required in the web server config**:

nginx example (broken):
```nginx
# WRONG — produces Location: http://t4as.org:443/...
server {
    listen 80;
    return 301 http://$host:443$request_uri;  # ← wrong scheme + explicit port
}
```

nginx fix:
```nginx
# CORRECT
server {
    listen 80;
    server_name t4as.org;
    return 301 https://$host$request_uri;     # ← https, no port
}

server {
    listen 443 ssl;
    server_name t4as.org;
    ssl_certificate     /etc/letsencrypt/live/t4as.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/t4as.org/privkey.pem;

    location = /thingyupdate {
        alias /var/www/fota/app_update.bin;
        add_header Content-Type application/octet-stream;
        add_header Content-Disposition "attachment; filename=app_update.bin";
    }
}
```

Caddy fix (auto-TLS, simplest option):
```caddyfile
t4as.org {
    route /thingyupdate {
        file_server {
            root /var/www/fota
            index app_update.bin
        }
    }
}
```

**After fixing, verify with curl before sending the MQTT command**:
```bash
# Must return 200 with binary content, no redirects:
curl -v --no-location https://t4as.org/thingyupdate -o /dev/null
# Check: < HTTP/1.1 200 OK
# Check: < Content-Type: application/octet-stream
# Check: NO 3xx response lines

# Check file size matches the build artifact:
curl -sI https://t4as.org/thingyupdate | grep -i content-length
wc -c app/build/app_update.bin   # sizes must match
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
- Publish `fota_start` command to `gateway/thingy91x-asset-tracker/command`

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
