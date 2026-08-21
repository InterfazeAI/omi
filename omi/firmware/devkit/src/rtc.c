#include "rtc.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sdcard.h"

LOG_MODULE_REGISTER(rtc, CONFIG_LOG_DEFAULT_LEVEL);

static K_MUTEX_DEFINE(rtc_lock);

// Wall clock is expressed as an offset from monotonic uptime rather than a free-running
// counter, so it cannot drift relative to the timestamps written into the index.
static int64_t base_epoch_ms;
static int64_t base_uptime_ms;
static bool utc_valid;
static uint32_t boot_id;

void rtc_init(void)
{
    k_mutex_lock(&rtc_lock, K_FOREVER);

    utc_valid = false;
    base_epoch_ms = 0;
    base_uptime_ms = 0;
    boot_id = 0;

#ifdef CONFIG_OMI_ENABLE_OFFLINE_STORAGE
    // There is no battery-backed RTC and no NVS in this build, so the boot counter lives on
    // the card alongside the recording it labels.
    uint32_t stored = 0;
    if (storage_load_boot_id(&stored) == 0) {
        boot_id = stored + 1;
    } else {
        boot_id = 1;
    }
    if (storage_save_boot_id(boot_id) != 0) {
        LOG_WRN("could not persist boot id %u", boot_id);
    }
#else
    boot_id = 1;
#endif

    k_mutex_unlock(&rtc_lock);
    LOG_INF("RTC initialized, boot id %u, clock not yet synchronized", boot_id);
}

bool rtc_is_valid(void)
{
    k_mutex_lock(&rtc_lock, K_FOREVER);
    bool v = utc_valid;
    k_mutex_unlock(&rtc_lock);
    return v;
}

uint32_t rtc_get_epoch(void)
{
    k_mutex_lock(&rtc_lock, K_FOREVER);
    uint32_t out = 0;
    if (utc_valid) {
        int64_t now = base_epoch_ms + (k_uptime_get() - base_uptime_ms);
        out = (uint32_t) (now / 1000);
    }
    k_mutex_unlock(&rtc_lock);
    return out;
}

int rtc_set_epoch(uint32_t epoch_s)
{
    if (epoch_s == 0) {
        return -EINVAL;
    }

    k_mutex_lock(&rtc_lock, K_FOREVER);
    base_epoch_ms = (int64_t) epoch_s * 1000;
    base_uptime_ms = k_uptime_get();
    bool first_sync = !utc_valid;
    utc_valid = true;
    uint32_t id = boot_id;
    k_mutex_unlock(&rtc_lock);

    LOG_INF("clock synchronized to %u (boot %u)", epoch_s, id);

#ifdef CONFIG_OMI_ENABLE_OFFLINE_STORAGE
    // Record the moment the mapping became known so everything already captured in this boot
    // can be placed in time retroactively.
    storage_index_mark(first_sync);
#else
    ARG_UNUSED(first_sync);
#endif

    return 0;
}

uint32_t rtc_get_uptime_s(void)
{
    return (uint32_t) (k_uptime_get() / 1000);
}

uint32_t rtc_get_boot_id(void)
{
    k_mutex_lock(&rtc_lock, K_FOREVER);
    uint32_t id = boot_id;
    k_mutex_unlock(&rtc_lock);
    return id;
}
