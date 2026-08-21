#!/usr/bin/env bash
set -e
cd /omi/firmware/v2.7.0
west build -p always -b xiao_ble/nrf52840/sense -d build_quiet /omi/firmware/devkit -- \
  -DCONF_FILE=prj_xiao_ble_sense_devkitv2-adafruit.conf \
  -DEXTRA_CONF_FILE="/omi/firmware/devkit/usb-console-quiet.conf;/omi/firmware/devkit/sd-on-no-button-speaker.conf" \
  -DDTC_OVERLAY_FILE=/omi/firmware/devkit/overlay/xiao_ble_sense_devkitv2-adafruit.overlay
echo BUILD_COMPLETE
