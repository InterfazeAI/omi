#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize timekeeping and start a new boot session.
 *
 * Loads the persisted boot counter from the SD card and increments it. The clock starts
 * invalid: the device has no battery-backed RTC, so wall-clock time is unknown until a
 * client writes it over the Time Sync service.
 */
void rtc_init(void);

/**
 * @brief Whether wall-clock time has been synchronized this boot.
 */
bool rtc_is_valid(void);

/**
 * @brief Current UTC epoch seconds, or 0 when unsynchronized.
 */
uint32_t rtc_get_epoch(void);

/**
 * @brief Anchor wall-clock time to the monotonic uptime and persist it.
 *
 * @param epoch_s UTC epoch seconds supplied by the client.
 * @return 0 on success, negative errno otherwise.
 */
int rtc_set_epoch(uint32_t epoch_s);

/**
 * @brief Monotonic seconds since this boot.
 *
 * Always meaningful, even before a time sync, which is what lets recordings made before
 * the first sync be placed in time retroactively.
 */
uint32_t rtc_get_uptime_s(void);

/**
 * @brief Identifier for the current power-on session.
 *
 * Increments every boot so a host can tell which records share a common uptime origin.
 */
uint32_t rtc_get_boot_id(void);

#endif
