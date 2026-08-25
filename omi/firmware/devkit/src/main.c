#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "button.h"
#include "codec.h"
#include "config.h"
#include "led.h"
#include "lib/battery/battery.h"
#include "mic.h"
#include "rtc.h"
#include "sdcard.h"
#include "speaker.h"
#include "storage.h"
#include "transport.h"
#include "usb.h"
#include "utils.h"
#include "wdog_facade.h"
#define BOOT_BLINK_DURATION_MS 600
#define BOOT_PAUSE_DURATION_MS 200
#define VBUS_DETECT (1U << 20)
#define WAKEUP_DETECT (1U << 16)
LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static void codec_handler(uint8_t *data, size_t len)
{
    int err = broadcast_audio_packets(data, len);
    if (err) {
        LOG_ERR("Failed to broadcast audio packets: %d", err);
    }
}

static void mic_handler(int16_t *buffer)
{
    int err = codec_receive_pcm(buffer, MIC_BUFFER_SAMPLES);
    if (err) {
        LOG_ERR("Failed to process PCM data: %d", err);
    }
}

void bt_ctlr_assert_handle(char *name, int type)
{
    LOG_INF("Bluetooth assert: %s (type %d)", name ? name : "NULL", type);
}

static void print_reset_reason(void)
{
    uint32_t reas = NRF_POWER->RESETREAS;

    // Clear the reset reason register
    NRF_POWER->RESETREAS = reas;

    if (reas & POWER_RESETREAS_DOG_Msk) {
        printk("Reset by WATCHDOG\n");
    } else if (reas & POWER_RESETREAS_NFC_Msk) {
        printk("Wake up by NFC field detect\n");
    } else if (reas & POWER_RESETREAS_RESETPIN_Msk) {
        printk("Reset by pin-reset\n");
    } else if (reas & POWER_RESETREAS_SREQ_Msk) {
        printk("Reset by soft-reset\n");
    } else if (reas & POWER_RESETREAS_LOCKUP_Msk) {
        printk("Reset by CPU LOCKUP\n");
    } else if (reas) {
        printk("Reset by a different source (0x%08X)\n", reas);
    } else {
        printk("Power-on-reset\n");
    }
}

bool is_connected = false;
bool is_charging = false;
extern bool is_off;
extern bool usb_charge;

// First init failure of this boot, or NULL. Init must never return out of main(): the watchdog is
// started early and only main's final loop feeds it, so `return err` from any init step is a
// silent 30-second reset loop -- and a reset loop cannot be asked what is wrong. A corrupted SD
// card bricked this board that way when it should only have disabled recording.
static const char *boot_fault_what;
static int boot_fault_err;

static void note_boot_fault(const char *what, int err)
{
    LOG_ERR("boot fault: %s (err %d) -- continuing in degraded mode", what, err);
    if (!boot_fault_what) {
        boot_fault_what = what;
        boot_fault_err = err;
    }
}

bool boot_faulted(void)
{
    return boot_fault_what != NULL;
}
static void boot_led_sequence(void)
{
    // Red blink
    set_led_red(true);
    k_msleep(BOOT_BLINK_DURATION_MS);
    set_led_red(false);
    k_msleep(BOOT_PAUSE_DURATION_MS);
    // Green blink
    set_led_green(true);
    k_msleep(BOOT_BLINK_DURATION_MS);
    set_led_green(false);
    k_msleep(BOOT_PAUSE_DURATION_MS);
    // Blue blink
    set_led_blue(true);
    k_msleep(BOOT_BLINK_DURATION_MS);
    set_led_blue(false);
    k_msleep(BOOT_PAUSE_DURATION_MS);
    // All LEDs on
    set_led_red(true);
    set_led_green(true);
    set_led_blue(true);
    k_msleep(BOOT_BLINK_DURATION_MS);
    // All LEDs off
    set_led_red(false);
    set_led_green(false);
    set_led_blue(false);
}

// Low-battery guard.
//
// Measured on this hardware over 1,333 samples: a full cell runs about 40 h recording
// continuously, falling 22.8 mV/h at the top, 9.1 mV/h across the long 3.8-3.5 V plateau, and
// 18.9 mV/h and steepening at the knee. So a few hundred millivolts of reserve costs hours -- but
// spending them is still the right trade, because running the cell to collapse does not merely
// stop the recording. When it happened during that drain test the card was mid-write, and the SD
// controller dropped an entire 4 MiB erase block: the boot sector and both FAT tables went with
// it, the filesystem became unmountable and the board then watchdog-looped on every boot. Shutting
// down under our own control, while the regulator is still in spec and the file handle can be
// closed, avoids all of that.
//
// The threshold was 3,350 mV, chosen before the discharge was fully characterised. That turned out
// to sit *inside* the estimated cutoff band rather than above it: the board was still running at
// 3,455 mV when logging stopped and was flat within 6.5 h, which at the measured knee rate puts
// collapse at 3,330-3,390 mV. VDD agrees -- it held 3,293-3,309 mV across the whole discharge and
// never sagged, so the floor is the 3.3 V rail plus dropout, not the chemistry. 3,420 mV clears
// that band with margin and costs roughly 3-4 h of the 40.
#define BATT_WARN_MV 3500
#define BATT_CRITICAL_MV 3420
#define BATT_SAMPLE_TICKS 20    // loop runs every 500 ms, so sample every 10 s
#define BATT_WINDOW 8           // single reads jitter +/-40 mV; decide on an average, never one
#define BATT_CRITICAL_STRIKES 3 // and only after the average stays low, never on one excursion

// Boot gate.
//
// battery_guard() protects a running board, but it cannot protect the first two minutes of one.
// After a low-battery shutdown the cell sits just under BATT_CRITICAL_MV, and SYSTEMOFF drops the
// load to roughly a microamp -- at which point an unloaded Li-ion cell recovers 50-100 mV. So the
// next button press sees a healthy-looking voltage, boots, mounts the card, opens a segment for
// append, and sags straight back under load. The guard cannot intervene during any of that: waking
// from SYSTEMOFF is a reset, so its averaging window starts empty and needs 80 s to fill before it
// may decide anything, then 30 s more to strike out. Each press therefore buys about two minutes
// of recording onto a cell already known to be at the edge of brownout, with no warning LED either,
// because battery_low is likewise unset until the window fills. That is precisely the mid-write
// power loss that destroyed a filesystem once already.
//
// So the gate runs before mount_sd_card() and declines to bring the card up at all.
//
// On the threshold, and an earlier claim here that was wrong. This originally read 3,450 mV and
// justified itself as sitting above BATT_CRITICAL_MV "by about the recovery it compensates for",
// quoting 50-100 mV. Those two statements contradict each other -- the gap is only 30 mV -- and the
// quoted figure was assumed, never measured. Reasoning from the actual load makes it worse: the
// board averages about 6 mA (40 h on a 250 mAh cell), so the IR component of the sag is single-
// digit millivolts, and what recovery there is comes from slower diffusion relaxation, tens of
// millivolts at most. A 30 mV gap does not reliably cover even that.
//
// 3,470 mV gives 50 mV of margin over the runtime threshold, which covers plausible relaxation
// without refusing cells that still hold hours. It remains an estimate, so the gate now records
// what it measured via battery_note_boot_reading(); read it against the running voltage over BLE
// (battery.py) on one charge and the sag stops being a guess.
#define BATT_BOOT_MIN_MV 3470
#define BATT_BOOT_SAMPLES 8

static bool battery_low = false;

static void battery_boot_gate(void)
{
    // Charging outranks the check. On USB the charger sustains the rail whatever the cell is doing,
    // and refusing to boot would leave no way to talk to the device while it recovers -- which is
    // the one moment you most want it reachable.
    if (battery_is_charging() == 1) {
        return;
    }

    // The runtime guard spaces samples 10 s apart to average over load variation. Nothing is loaded
    // yet, so consecutive reads suffice and the whole check costs milliseconds rather than 80 s --
    // which matters, since every millisecond of it is drawn from a suspect cell.
    uint32_t sum = 0;
    uint8_t taken = 0;
    for (uint8_t i = 0; i < BATT_BOOT_SAMPLES; i++) {
        uint16_t mv = 0;
        if (battery_get_millivolt(&mv) == 0 && mv != 0) {
            sum += mv;
            taken++;
        }
    }

    // Same reasoning as the runtime guard: a gauge that is not reading is not evidence of a flat
    // battery. Refusing to boot on no evidence would brick every board with a broken divider, which
    // is a fault this project has already shipped once.
    if (taken == 0) {
        LOG_WRN("Battery boot gate: no usable reading, continuing");
        return;
    }

    uint16_t avg = (uint16_t) (sum / taken);

    // Recorded either way. The reading that matters most is the one from a boot that was allowed
    // to proceed, because that is the one with a running counterpart to compare against.
    battery_note_boot_reading(avg);

    if (avg >= BATT_BOOT_MIN_MV) {
        LOG_INF("Battery %u mV at rest, continuing boot", avg);
        return;
    }

    LOG_ERR("Battery too flat to boot (%u mV at rest): powering off without mounting the card", avg);

    // Three yellow blinks, then dark. Yellow because red means recording and this board is not
    // going to record; three deliberate blinks because the alternative -- going straight to
    // SYSTEMOFF -- is indistinguishable from a board that is simply broken.
    for (int i = 0; i < 3; i++) {
        set_led_red(true);
        set_led_green(true);
        k_msleep(120);
        set_led_red(false);
        set_led_green(false);
        k_msleep(200);
    }

    enter_system_off();
}

static void battery_guard(void)
{
    static uint16_t window[BATT_WINDOW];
    // `next` is deliberately separate from `filled`. Indexing with `filled % BATT_WINDOW` looks
    // equivalent and is not: `filled` stops incrementing once the window is full, so every later
    // sample lands in slot 0 and the other seven stay pinned to the first 80 seconds after boot.
    // That does not merely bias the mean, it caps it -- with a full cell at boot the average cannot
    // reach BATT_CRITICAL_MV even at 0 V, so the shutdown and the warning LED never fire at all.
    static uint8_t ticks, next, filled, strikes;

    if (++ticks < BATT_SAMPLE_TICKS) {
        return;
    }
    ticks = 0;

    uint16_t mv = 0;
    if (battery_get_millivolt(&mv) != 0 || mv == 0) {
        return; // a gauge that is not reading is not evidence of a flat battery
    }

    window[next] = mv;
    next = (uint8_t) ((next + 1) % BATT_WINDOW);
    if (filled < BATT_WINDOW) {
        filled++;
        return; // no decision until the average means something
    }

    uint32_t sum = 0;
    for (int i = 0; i < BATT_WINDOW; i++) {
        sum += window[i];
    }
    uint16_t avg = (uint16_t) (sum / BATT_WINDOW);

    // Published before the charging check below, so the reported percentage keeps tracking a cell
    // that is on the charger. Only the shutdown logic wants to stand down while charging.
    battery_note_smoothed_reading(avg);

    if (battery_is_charging() == 1) {
        strikes = 0;
        battery_low = false;
        return;
    }

    battery_low = (avg < BATT_WARN_MV);

    if (avg >= BATT_CRITICAL_MV) {
        strikes = 0;
        return;
    }

    if (++strikes >= BATT_CRITICAL_STRIKES) {
        LOG_ERR("battery critical (%u mV): closing files and powering off", avg);
        turnoff_all(); // flushes the write batch and closes the segment before SYSTEMOFF
    }
}

void set_led_state()
{
    // Three red blinks confirming the erase-and-unbond finished, set by the storage thread at the
    // point the work actually completed. Outranks the warning below because the two overlap: the
    // yellow warning stays lit while the button is still held, and going straight to the
    // confirmation is what tells you it is done and you can let go.
    if (storage_unbond_done_blinks) {
        storage_unbond_done_blinks--;
        set_led_red((storage_unbond_done_blinks % 2) == 1);
        set_led_green(false);
        set_led_blue(false);
        return;
    }

    // Yellow while the button is warning about a reset. Checked before anything else because this
    // refresh runs every 500 ms: without the early return it would clear the warning colour almost
    // as soon as button.c set it, leaving no signal at all before the card is erased.
    if (button_reset_warning) {
        return;
    }

    // Low battery outranks connection state: a blink you can miss is better than a board that dies
    // without warning, and the states below would otherwise hold the LED steady.
    //
    // Yellow, not red. Steady red already means "recording, no connection", and asking anyone to
    // tell that apart from a red blink is asking too much of one LED. Red stays recording; yellow
    // means the battery needs attention, at boot and at runtime alike.
    if (battery_low) {
        static bool phase;
        phase = !phase;
        set_led_red(phase);
        set_led_green(phase);
        set_led_blue(false);
        return;
    }

    // Recording and connected state - BLUE

    if (usb_charge) {
        is_charging = !is_charging;
        if (is_charging) {
            set_led_green(true);
        } else {
            set_led_green(false);
        }
    } else {
        set_led_green(false);
    }
    if (is_off) {
        set_led_red(false);
        set_led_blue(false);
        return;
    }
    if (is_connected) {
        set_led_blue(true);
        set_led_red(false);
        return;
    }

    // Recording but lost connection - RED
    if (!is_connected) {
        set_led_red(true);
        set_led_blue(false);
        return;
    }
}

int main(void)
{
    int err;

    // Print and clear reset reason
    print_reset_reason();

    NRF_POWER->DCDCEN = 1;
    NRF_POWER->DCDCEN0 = 1;

    LOG_INF("Booting...\n");

    LOG_INF("Model: %s", CONFIG_BT_DIS_MODEL);
    LOG_INF("Firmware revision: %s", CONFIG_BT_DIS_FW_REV_STR);
    LOG_INF("Hardware revision: %s", CONFIG_BT_DIS_HW_REV_STR);
    // Force QSPI flash into deep sleep mode
    const struct device *flash_dev = DEVICE_DT_GET(DT_NODELABEL(p25q16h));
    if (device_is_ready(flash_dev)) {
        err = pm_device_action_run(flash_dev, PM_DEVICE_ACTION_SUSPEND);
        if (err) {
            LOG_ERR("Failed to suspend QSPI flash: %d", err);
        }
    } else {
        LOG_ERR("QSPI flash device not ready");
    }
    LOG_PRINTK("\n");
    LOG_INF("Initializing LEDs...\n");

    err = led_start();
    if (err) {
        // Alone among these, this one runs before watchdog_init, so failing here used to leave a
        // dead board rather than a resetting one. Either way there is no reason three status LEDs
        // should decide whether the microphone runs.
        note_boot_fault("led init", err);
    } else {
        boot_led_sequence();
    }

    // Initialize watchdog early to catch any freezes during boot
    err = watchdog_init();
    if (err) {
        LOG_WRN("Watchdog init failed (err %d), continuing without watchdog", err);
    }

    // Enable battery
#ifdef CONFIG_OMI_ENABLE_BATTERY
    err = battery_init();
    if (err) {
        note_boot_fault("battery init", err);
    } else {
        LOG_INF("Battery initialized");
    }
#endif

    // Enable button
#ifdef CONFIG_OMI_ENABLE_BUTTON
    err = button_init();
    if (err) {
        note_boot_fault("button init", err);
    } else {
        LOG_INF("Button initialized");
        activate_button_work();
    }
#endif

    // Refuse to go further on a cell that cannot sustain a write. Deliberately placed here: after
    // button_init(), so enter_system_off() has a wake source to arm, and before anything opens the
    // SD card, which is the resource being protected.
    battery_boot_gate();

    // Enable accelerometer
#ifdef CONFIG_OMI_ENABLE_ACCELEROMETER
    err = accel_start();
    if (err) {
        note_boot_fault("accelerometer start", err);
    } else {
        LOG_INF("Accelerometer initialized");
    }
#endif

    // Enable speaker
#ifdef CONFIG_OMI_ENABLE_SPEAKER
    err = speaker_init();
    if (err) {
        note_boot_fault("speaker init", err);
    } else {
        LOG_INF("Speaker initialized");
    }
#endif

    // Enable sdcard
#ifdef CONFIG_OMI_ENABLE_OFFLINE_STORAGE
    LOG_PRINTK("\n");
    LOG_INF("Mount SD card...\n");

    err = mount_sd_card();
    if (err) {
        // Everything below needs the card, but the radio does not: without storage the device
        // still streams live audio and still answers diagnostics, which is what lets you find out
        // why the card is missing. Bricking here is strictly worse than recording nothing.
        note_boot_fault("sdcard mount", err);
    } else {
        k_msleep(500);

        // Needs the card mounted: the boot counter is persisted there for want of an RTC or NVS.
        rtc_init();
        // Anchor the offset this session starts at, even though the clock is not yet set.
        storage_index_mark(true);

        LOG_PRINTK("\n");
        LOG_INF("Initializing storage...\n");

        err = storage_init();
        if (err) {
            note_boot_fault("storage init", err);
        }
    }
#endif

    // Enable haptic
#ifdef CONFIG_OMI_ENABLE_HAPTIC
    LOG_PRINTK("\n");
    LOG_INF("Initializing haptic...\n");

    err = init_haptic_pin();
    if (err) {
        note_boot_fault("haptic init", err);
    } else {
        LOG_INF("Haptic pin initialized");
    }
#endif

    // Enable usb
#ifdef CONFIG_OMI_ENABLE_USB
    LOG_PRINTK("\n");
    LOG_INF("Initializing power supply check...\n");

    err = init_usb();
    if (err) {
        note_boot_fault("usb init", err);
    }
#endif

    // Indicate transport initialization
    LOG_PRINTK("\n");
    LOG_INF("Initializing transport...\n");

    set_led_green(true);
    set_led_green(false);

    // Start transport
    int transportErr;
    transportErr = transport_start();
    if (transportErr) {
        // Blink green LED to indicate error
        for (int i = 0; i < 5; i++) {
            set_led_green(!gpio_pin_get_dt(&led_green));
            k_msleep(200);
        }
        set_led_green(false);

        note_boot_fault("transport start", transportErr);
    }

#ifdef CONFIG_OMI_ENABLE_SPEAKER
    play_boot_sound();
#endif

    LOG_PRINTK("\n");
    LOG_INF("Initializing codec...\n");

    set_led_blue(true);

    // Audio codec(opus) callback
    set_codec_callback(codec_handler);
    err = codec_start();
    if (err) {
        // Blink blue LED to indicate error
        for (int i = 0; i < 5; i++) {
            set_led_blue(!gpio_pin_get_dt(&led_blue));
            k_msleep(200);
        }
        set_led_blue(false);
        note_boot_fault("codec start", err);
    }

#ifdef CONFIG_OMI_ENABLE_HAPTIC
    play_haptic_milli(500);
#endif
    set_led_blue(false);

    // Indicate microphone initialization
    LOG_PRINTK("\n");
    LOG_INF("Initializing microphone...\n");

    set_led_red(true);
    set_led_green(true);

    set_mic_callback(mic_handler);
    err = mic_start();
    if (err) {
        // Blink red and green LEDs to indicate error
        for (int i = 0; i < 5; i++) {
            set_led_red(!gpio_pin_get_dt(&led_red));
            set_led_green(!gpio_pin_get_dt(&led_green));
            k_msleep(200);
        }
        set_led_red(false);
        set_led_green(false);
        note_boot_fault("mic start", err);
    }

    set_led_red(false);
    set_led_green(false);

    LOG_PRINTK("\n");
    if (boot_fault_what) {
        LOG_ERR("Device initialized DEGRADED: %s failed (err %d)", boot_fault_what, boot_fault_err);
    } else {
        LOG_INF("Device initialized successfully\n");
    }

    set_led_blue(true);
    k_msleep(1000);
    set_led_blue(false);

    // Main loop
    LOG_PRINTK("\n");
    LOG_INF("Entering main loop...\n");

    while (1) {
        watchdog_feed();

        battery_guard();
        set_led_state();
        k_msleep(500);
    }

    // Unreachable
    return 0;
}
