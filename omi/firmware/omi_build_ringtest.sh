#!/usr/bin/env bash
# Same image as omi_build.sh but with a deliberately tiny recording ring, so segment rotation
# and eviction of the oldest segment can be observed in minutes. Output goes to a separate
# build directory so it cannot be mistaken for a release image.
set -e
cd /omi/firmware/v2.7.0
west build -p always -b xiao_ble/nrf52840/sense -d build_ringtest /omi/firmware/devkit -- \
  -DCONF_FILE=prj_xiao_ble_sense_devkitv2-adafruit.conf \
  -DEXTRA_CONF_FILE="/omi/firmware/devkit/sd-on-no-button-speaker.conf;/omi/firmware/devkit/debug-usb-log.conf;/omi/firmware/devkit/ring-fast-rotate.conf" \
  -DDTC_OVERLAY_FILE=/omi/firmware/devkit/overlay/xiao_ble_sense_devkitv2-adafruit.overlay
echo BUILD_COMPLETE
