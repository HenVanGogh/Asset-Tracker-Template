#!/bin/bash
set -e

# Define image name
IMAGE="ncs-v2.6.1-debian"
PROJ_DIR="$(pwd)/app"
NRFUTIL_PATH="/home/mic/ncs/v3.0.2/.venv/bin/nrfutil"

# Check if nrfutil exists locally
if [ ! -f "$NRFUTIL_PATH" ]; then
    echo "Error: nrfutil not found at $NRFUTIL_PATH"
    exit 1
fi

# ── JLink serial detection ──────────────────────────────────────────────────
# Optional: pass serial number as first argument to target a specific device.
# Without an argument, auto-detects the first connected J-Link programmer.
# For 200 devices on the same host, pass the serial explicitly:
#   ./flash_docker.sh 1050739777
#
if [ -n "$1" ]; then
    JLINK_SERIAL="$1"
    echo "Using specified J-Link serial: $JLINK_SERIAL"
else
    # Parse `nrfutil device list` output: find the serial number of the first
    # device whose Traits line contains "jlink".  Serial is on the line
    # immediately before the "Product" line for each device block.
    JLINK_SERIAL=$("$NRFUTIL_PATH" device list 2>&1 | awk '
        /^[[:alnum:]]+$/ { cur = $0 }
        /Traits.*jlink/  { print cur; exit }
    ')
    if [ -z "$JLINK_SERIAL" ]; then
        echo "Error: No J-Link programmer found. Connect one or pass serial as argument:"
        echo "  ./flash_docker.sh <serial_number>"
        exit 1
    fi
    echo "Auto-detected J-Link serial: $JLINK_SERIAL"
fi

echo "Starting docker build with image: $IMAGE"

# Build inside Docker
docker run --rm \
    -v "$PROJ_DIR":/workspace/app \
    -w /workspace/app \
    "$IMAGE" \
    west build -d build -- -DPM_STATIC_YML_FILE=/workspace/app/pm_static_thingy91x_nrf9151_ns.yml

# Flash using host nrfutil (workaround for Docker J-Link connection issues)
echo "Flashing using host nrfutil..."

# NOTE: 'nrfutil device recover' is intentionally NOT used here.
# Recover performs a full chip erase which would destroy the settings partition
# (external flash) containing the deterministic device name and MQTT topics.
# Even though the device name is now derived from FICR hardware ID (not random),
# recovering would wipe the settings_storage partition which holds other
# runtime config (MQTT host, credentials, etc.).
# If you need a full recovery (bricked device), run manually:
#   $NRFUTIL_PATH device recover --serial-number "$JLINK_SERIAL"
#
# IMPORTANT: Always build via this script (or pass -DPM_STATIC_YML_FILE).
# The pm_static YAML defines the nrf5340_dfu and nrf52840_dfu external flash
# partitions used by ext_dfu/spi_dfu. Building without it leaves those
# PM_NRF5340_DFU_* macros undefined and the build will fail.

# Flash the merged hex directly (sector-erase only — preserves external flash settings)
if [ -f "$PROJ_DIR/build/merged.hex" ]; then
    echo "Flashing $PROJ_DIR/build/merged.hex to J-Link $JLINK_SERIAL"
    $NRFUTIL_PATH device program --serial-number "$JLINK_SERIAL" --firmware "$PROJ_DIR/build/merged.hex"
else
    echo "Error: merged.hex not found at $PROJ_DIR/build/merged.hex"
    exit 1
fi

echo "Flash complete!"
