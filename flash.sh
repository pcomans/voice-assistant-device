#!/bin/bash
# Flash smart assistant device firmware

set -e  # Exit on error

# Source ESP-IDF environment
. ~/esp/v5.5.1/esp-idf/export.sh

# Auto-detect ESP32 device
DEVICE=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1)

if [ -z "$DEVICE" ]; then
    echo "Error: No ESP32 device found (looking for /dev/cu.usbmodem*)"
    exit 1
fi

echo "Found device: $DEVICE"

# Kill any processes using the device
if lsof "$DEVICE" >/dev/null 2>&1; then
    echo "Killing processes using $DEVICE..."
    lsof "$DEVICE" | awk 'NR>1 {print $2}' | xargs kill -9 2>/dev/null || true
    sleep 1
fi

# Flash the device
idf.py -p "$DEVICE" flash

# Optional: Start monitoring immediately after flash
# Uncomment the next line if you want to see logs automatically
# idf.py -p "$DEVICE" monitor
