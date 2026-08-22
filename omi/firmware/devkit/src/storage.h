#ifndef STORAGE_H
#define STORAGE_H
#include <stdbool.h>

/**
 * @brief Initializes the Storage Transport thread
 *
 * Initializes the Storage Transport thread
 *
 * @return 0 if successful, negative errno code if error
 */
int storage_init();

/**
 * @brief Erase every recording, then release the Bluetooth bond.
 *
 * Handing the device to someone else must not hand over the previous owner's audio, so releasing
 * the bond destroys the card contents first. Safe to call from a GATT handler: it only raises a
 * flag, and the storage thread does the filesystem work (a GATT handler touching FatFs is the
 * crash in DEBUGGING.md trap 7).
 *
 * The order is deliberate. The wipe finishes before the bond is released, so losing power midway
 * leaves the old bond in place and no new device can pair and read whatever survived. Releasing
 * first would open exactly that window.
 */
void storage_request_unbond_wipe(void);

#endif