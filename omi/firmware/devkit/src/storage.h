#ifndef STORAGE_H
#define STORAGE_H
#include <stdbool.h>
#include <stdint.h>

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

/**
 * @brief Remaining 500 ms LED half-cycles in the "erase and unbond finished" signal.
 *
 * Set by the storage thread once the card has actually been cleared and the bond released;
 * counted down and rendered by set_led_state() in main.c.
 *
 * It is deliberately not driven from the button release. The erase is asynchronous -- the button
 * handler only raises a flag, and clearing a full card takes long enough to matter -- so a signal
 * at release would confirm something that had not happened yet, which is worse than no signal at
 * all for an irreversible operation.
 */
extern volatile uint8_t storage_unbond_done_blinks;

#endif