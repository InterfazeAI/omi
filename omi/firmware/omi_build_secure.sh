#!/usr/bin/env bash
# Production config plus secure-pairing.conf: audio, recordings, the clock and the DFU control
# point all require a paired, encrypted link. Produces build_secure/zephyr/zephyr.uf2.
#
# Pairing is Just Works (no display, no keypad) and there is a single bond slot with no eviction,
# so the first device to pair owns the board. Bonds live in the `storage` flash partition, outside
# the app region, so reflashing this or any other image does not release the slot -- recovery is
# omi_build_unbond.sh.
#
# Built for the board with a momentary power button on D5 rather than a switch in the battery
# line: a long press runs SYSTEMOFF and a press wakes it again. Swap the fragment back to
# sd-on-no-button-speaker.conf for hardware with no button, or the floating pin will shut the
# board down by itself.
set -e
cd /omi/firmware/v2.7.0
west build -p always -b xiao_ble/nrf52840/sense -d build_secure /omi/firmware/devkit -- \
  -DCONF_FILE=prj_xiao_ble_sense_devkitv2-adafruit.conf \
  -DEXTRA_CONF_FILE="/omi/firmware/devkit/usb-console-quiet.conf;/omi/firmware/devkit/sd-on-button-no-speaker.conf;/omi/firmware/devkit/secure-pairing.conf" \
  -DDTC_OVERLAY_FILE=/omi/firmware/devkit/overlay/xiao_ble_sense_devkitv2-adafruit.overlay
echo BUILD_COMPLETE
