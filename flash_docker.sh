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
# (external flash) containing the auto-generated device name and MQTT topics.
# If you need a full recovery (bricked device), run manually:
#   $NRFUTIL_PATH device recover --serial-number 1050739777

# Flash the merged hex directly (sector-erase only — preserves external flash settings)
if [ -f "$PROJ_DIR/build/merged.hex" ]; then
    echo "Flashing $PROJ_DIR/build/merged.hex"
    $NRFUTIL_PATH device program --serial-number 1050739777 --firmware "$PROJ_DIR/build/merged.hex"
else
    echo "Error: merged.hex not found at $PROJ_DIR/build/merged.hex"
    exit 1
fi

echo "Flash complete!"
