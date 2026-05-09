# FOTA Provisioning Guide

## Overview

The firmware update (FOTA) system downloads a signed MCUboot image over HTTPS
and applies it via the MCUboot bootloader.  The nRF9151 modem performs TLS
verification using credentials stored in its own secure credential store.

---

## 1. First-time device flash (JLink required)

Before FOTA can be used at all, the device must have MCUboot, TF-M, and the
initial application programmed via JLink. FOTA cannot install MCUboot —
MCUboot **must** already be present to perform OTA updates.

```bash
# From the project root:
./flash_docker.sh
```

This builds and flashes `app/build/merged.hex` which contains:
| Region            | Address     | Size   | Content                    |
|-------------------|-------------|--------|----------------------------|
| b0 (NSIB)         | 0x00000     | 32 KB  | Immutable secure boot      |
| s0 / MCUboot      | 0x08000     | 80 KB  | Bootloader (primary slot)  |
| s1 / MCUboot      | 0x1C000     | 80 KB  | Bootloader (redundant)     |
| TF-M              | 0x30200     | 31 KB  | TrustZone secure firmware  |
| Application       | 0x38000     | 800 KB | This application           |
| mcuboot_secondary | ext 0x00000 | 832 KB | Cleared (OTA staging area) |
| settings_storage  | ext 0x4D0000| 8 KB   | NVS / Zephyr settings      |

After the first flash the device can receive all future updates wirelessly.

---

## 2. TLS Certificate Provisioning

FOTA downloads from `https://t4as.org/` which is served via Let's Encrypt.
The modem needs the **ISRG Root X1** CA certificate at security tag `16842754`.

### Step 1 — Connect to RTT shell

Open the RTT shell (e.g. `JLinkRTTViewer` or `nrfjprog --rtt`) and confirm
the AT shell responds:

```
uart:~$ at AT+CFUN?
+CFUN: 1
```

### Step 2 — Delete any stale cert at this tag (idempotent)

```
uart:~$ at AT%CMNG=3,16842754,0
```

### Step 3 — Write ISRG Root X1

Paste the following command as a single line in the AT shell.
The PEM content must be the DER-base64 body only (no headers):

```
uart:~$ at AT%CMNG=0,16842754,0,"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA...
```

Full certificate PEM (copy the text between `-----BEGIN CERTIFICATE-----` and
`-----END CERTIFICATE-----`, strip the header/footer lines, pass as the last
quoted argument):

```
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoBggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
```

### Step 4 — Verify provisioning

```
uart:~$ at AT%CMNG=1,16842754,0
%CMNG: 16842754,0,"<fingerprint>"
```

A non-empty response confirms the cert is stored.

---

## 3. Triggering a FOTA update over MQTT

Once cert is provisioned, publish to the device command topic:

```json
{"command": "fota_start", "url": "https://t4as.org/thingyupdate"}
```

Optional overrides:
```json
{"command": "fota_start", "url": "https://t4as.org/v2/firmware.bin", "sec_tag": 16842754}
```

Monitor with:
```json
{"command": "fota_status"}
```

Cancel with:
```json
{"command": "fota_cancel"}
```

The device will:
1. Download the image to the MCUboot secondary slot (ext flash 0x00000)
2. Publish progress messages to the data topic
3. Reboot — MCUboot verifies signature and swaps primary ↔ secondary
4. Confirm image (`boot_write_img_confirmed`) on first boot of new image
5. Old firmware in secondary slot acts as rollback if confirmation never arrives

---

## 4. FOTA update file

The file to serve at `https://t4as.org/thingyupdate`:

```
app/build/dfu_application.zip
```

This is a nrfutil-compatible package produced by the build system containing:
- `app_update.bin` — signed MCUboot image (the actual binary served to device)
- `manifest.json` — image metadata

**The device calls `fota_download_start(host, "/thingyupdate", ...)` which expects
the raw `app_update.bin` at that URL, NOT the zip.**

Serve `app_update.bin` (extracted from `dfu_application.zip`) directly:

```bash
cd app/build
unzip -o dfu_application.zip
# Now serve app_update.bin at https://t4as.org/thingyupdate
```

---

## 5. Repeat FOTA cycle

```bash
# 1. Edit code, increment VERSION file
# 2. Build:
./flash_docker.sh   # builds only (flash step fails gracefully if no device)
# 3. Extract and deploy:
cd app/build && unzip -o dfu_application.zip
# 4. Upload app_update.bin to your server at the FOTA URL
# 5. Send MQTT command to device
```
