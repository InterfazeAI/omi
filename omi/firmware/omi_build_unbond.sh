#!/usr/bin/env bash
# LAST-RESORT recovery image: erases every Bluetooth bond at boot, then behaves normally.
#
# Not the normal way to change owner. The paired device can release the board itself over BLE --
# `pairing.py --release`, which also wipes the recordings so a new owner cannot read them. Use this
# image only when that is impossible: the bonded phone is lost, broken, or wiped, so nothing can
# ask for the release. Note it erases the bond but NOT the card, so whoever pairs next inherits the
# existing audio; run a NUKE afterwards if that matters.
#
# Needed because the config keeps a single bond slot with no eviction, so the first device to pair
# owns the board and no other can. Bonds live in the `storage` flash partition, outside the app
# region, so reflashing does not clear them, and this hardware has no button to hold at startup.
#
# secure-pairing.conf must be in this list. CONFIG_OMI_ERASE_BONDS_ON_BOOT depends on BT_SETTINGS,
# which that fragment provides; without it the symbol is silently unselectable and this image boots
# up erasing nothing at all -- a recovery path that quietly does not recover.
#
# Usage: flash build_unbond/zephyr/zephyr.uf2, let it boot once, then flash the normal image and
# pair the device you actually want.
set -e
cd /omi/firmware/v2.7.0
west build -p always -b xiao_ble/nrf52840/sense -d build_unbond /omi/firmware/devkit -- \
  -DCONF_FILE=prj_xiao_ble_sense_devkitv2-adafruit.conf \
  -DEXTRA_CONF_FILE="/omi/firmware/devkit/usb-console-quiet.conf;/omi/firmware/devkit/sd-on-no-button-speaker.conf;/omi/firmware/devkit/secure-pairing.conf;/omi/firmware/devkit/erase-bonds.conf" \
  -DDTC_OVERLAY_FILE=/omi/firmware/devkit/overlay/xiao_ble_sense_devkitv2-adafruit.overlay
echo BUILD_COMPLETE
