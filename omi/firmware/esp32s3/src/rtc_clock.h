#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include <Arduino.h>
#include <stdint.h>

/// Wall clock for record timestamps.
///
/// The board has no RTC and power is a hard switch, so the only authoritative
/// source is the 4-byte UTC epoch the app writes to 19B10031 on every connect.
/// Between the switch going on and the first pairing there is no true time, only
/// the epoch persisted in the ring metadata at last power-down.
///
/// The reference CV1 firmware refuses to record at all until the RTC is set. On
/// a switch-powered device that would silently record nothing for a user who
/// flips the switch before ever pairing, so instead we record with the restored
/// estimate and report rtc_valid = 0. The app then falls back to
/// `now - duration` rather than trusting a timestamp we know is behind by
/// however long the device was off.

/// Seed the clock from the epoch persisted on the SD card. Pass 0 when there is
/// none. Never marks the clock valid.
void rtc_restore(uint32_t persisted_epoch);

/// Apply an authoritative epoch from the app's time sync. Marks the clock valid.
/// Returns false, changing nothing, when the epoch is implausible — callers must
/// not treat a rejected write as a sync, or they will vouch for timestamps still
/// being stamped from the restored estimate.
bool rtc_set_utc(uint32_t epoch);

/// Current UTC epoch seconds, or 0 when there is no basis at all (no time sync
/// this boot and nothing persisted).
uint32_t rtc_now();

/// True only when the app has synced time during this power-on. A restored
/// estimate deliberately does not count.
///
/// This describes the clock *right now*, which is not the same question as
/// whether an already-journalled record's timestamp can be trusted — see
/// sd_ring_timestamps_authoritative.
bool rtc_is_valid();

/// Bumped every time the clock is re-anchored, by either a restore or a sync.
/// Wall time moves discontinuously at those points, so a caller assembling a
/// timestamped record uses this to avoid straddling the jump.
uint32_t rtc_generation();

#endif // RTC_CLOCK_H
