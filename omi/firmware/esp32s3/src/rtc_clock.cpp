#include "rtc_clock.h"

#include <esp_timer.h>

// Epochs below this are nonsense (2023-11-14). Used to reject a garbage write
// rather than to gate recording.
#define RTC_MIN_PLAUSIBLE_EPOCH 1700000000UL

static uint32_t base_epoch = 0;
static int64_t base_uptime_us = 0;
static bool synced = false;
static volatile uint32_t clock_generation = 0;

static void rtc_anchor(uint32_t epoch)
{
    base_epoch = epoch;
    base_uptime_us = esp_timer_get_time();
    clock_generation++;
}

void rtc_restore(uint32_t persisted_epoch)
{
    if (synced || persisted_epoch < RTC_MIN_PLAUSIBLE_EPOCH) {
        return;
    }
    rtc_anchor(persisted_epoch);
    Serial.printf("RTC: restored estimate %u (not authoritative)\n", (unsigned) persisted_epoch);
}

bool rtc_set_utc(uint32_t epoch)
{
    if (epoch < RTC_MIN_PLAUSIBLE_EPOCH) {
        Serial.printf("RTC: rejecting implausible epoch %u\n", (unsigned) epoch);
        return false;
    }
    const uint32_t before = rtc_now();
    rtc_anchor(epoch);
    synced = true;
    Serial.printf("RTC: synced to %u (was %u)\n", (unsigned) epoch, (unsigned) before);
    return true;
}

uint32_t rtc_now()
{
    if (base_epoch == 0) {
        return 0;
    }
    const int64_t elapsed_s = (esp_timer_get_time() - base_uptime_us) / 1000000;
    return (uint32_t) ((int64_t) base_epoch + elapsed_s);
}

bool rtc_is_valid()
{
    return synced;
}

uint32_t rtc_generation()
{
    return clock_generation;
}
