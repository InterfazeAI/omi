#!/usr/bin/env bash
# Diagnostic twin of omi_build_secure.sh: same board, same fragments, logging turned on.
# Produces build_diag/zephyr/zephyr.uf2.
#
# Why this exists as its own script: the shipped image layers usb-console-quiet.conf, which sets
# CONFIG_LOG_DEFAULT_LEVEL=0 and so compiles LOG_* out of every module that registers with the
# default level. Only battery.c survives that, because it hardcodes LOG_LEVEL_INF. The result is a
# console that prints the boot banner, the reset cause and two battery lines and nothing else --
# which reads exactly like a hang immediately after battery_init() no matter where the board
# actually stopped. Swapping in debug-usb-log.conf restores level 3 everywhere.
#
# diag-immediate.conf goes on top because deferred logging drops whatever is still queued when the
# CPU faults or the watchdog fires, which is precisely the last line you need when chasing a boot
# loop. Immediate mode is not safe for normal running -- see the comments in debug-usb-log.conf
# about the audio path flooding a CDC ring that nobody is draining -- so do not ship this image.
set -e
cd /omi/firmware/v2.7.0
west build -p always -b xiao_ble/nrf52840/sense -d build_diag /omi/firmware/devkit -- \
  -DCONF_FILE=prj_xiao_ble_sense_devkitv2-adafruit.conf \
  -DEXTRA_CONF_FILE="/omi/firmware/devkit/debug-usb-log.conf;/omi/firmware/devkit/diag-immediate.conf;/omi/firmware/devkit/sd-on-button-no-speaker.conf;/omi/firmware/devkit/secure-pairing.conf" \
  -DDTC_OVERLAY_FILE=/omi/firmware/devkit/overlay/xiao_ble_sense_devkitv2-adafruit.overlay
echo BUILD_COMPLETE
