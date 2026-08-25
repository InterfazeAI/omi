/*
 * Copyright 2024 Marcus Alexander Tjomsaas
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "battery.h"

#include <hal/nrf_gpio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

// Kept from the last battery_init()/adc_read() so the BLE diagnostic can report why the gauge is
// not working. Serial is not readable on this board, so an error that is only logged is lost.
static int8_t init_err;
static int8_t setup_err;
static int8_t gpio_err;
static int8_t last_read_err;

static const struct device *gpio_battery_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *adc_battery_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

static K_MUTEX_DEFINE(battery_mut);

/*
 * Pin roles, from Seeed's own board definition for this module (variant.cpp, g_ADigitalPinMap):
 *
 *   13, // D22 is P0.13 (HICHG)     charge current select, output: low = 100 mA, high-Z = 50 mA
 *   17, // D23 is P0.17 (~CHG)      charge status, INPUT from the BQ25100, low while charging
 *   14, // D14 is P0.14 (READ_BAT)  divider enable, output: low = divider switched in
 *   31, // D32 is P0.31 (VBAT)      divider tap, AIN7
 *
 * P0.17 was previously named CHARGING_ENABLE here and driven as a push-pull output. There is no
 * such control: the BQ25100 decides when to charge, and that pin is how it reports what it chose.
 * Driving it fought the charger's open-drain output every time it pulled low, and threw away the
 * one signal that distinguishes "on USB and charging" from "running the cell down".
 */
#define GPIO_BATTERY_CHARGE_SPEED 13
#define GPIO_BATTERY_CHARGE_STATUS 17
#define GPIO_BATTERY_READ_ENABLE 14

// Change this to a higher number for better averages
// Note that increasing this holds up the thread / ADC for longer.
#define ADC_TOTAL_SAMPLES 10
int16_t sample_buffer[ADC_TOTAL_SAMPLES];

#define ADC_RESOLUTION 12
#define ADC_CHANNEL 7
#define ADC_PORT SAADC_CH_PSELP_PSELP_AnalogInput7 // AIN7
#define ADC_REFERENCE ADC_REF_INTERNAL             // 0.6V
#define ADC_GAIN ADC_GAIN_1_6                      // ADC REFERENCE * 6 = 3.6V

// The divider presents 1M||510k = 338k to the SAADC, and Nordic's acquisition-time table wants
// 40us for anything up to 800k. At the 10us default the sampling capacitor never finishes
// charging, so every reading comes in low -- a quiet few percent of error, not a failure, which
// is the kind that survives for years.
#define ADC_ACQUISITION ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40)

struct adc_channel_cfg channel_7_cfg = {.gain = ADC_GAIN,
                                        .reference = ADC_REFERENCE,
                                        .acquisition_time = ADC_ACQUISITION,
                                        .channel_id = ADC_CHANNEL,
#ifdef CONFIG_ADC_NRFX_SAADC
                                        .input_positive = ADC_PORT
#endif
};

/*
 * The SAADC can sample the chip's own 3.3 V supply with no external wiring, which makes it the
 * control for every other reading. A reading from AIN7 is evidence about the divider only if the
 * ADC is known to work, and a misconfigured ADC returns zero exactly like a dead pin does. VDD is
 * a known 3.3 V, so if it reads correctly then the converter, reference, gain and conversion are
 * all sound and a zero from AIN7 can only be the pin.
 *
 * VDDHDIV5 was tried here too, in the hope that this module runs in high-voltage mode and feeds
 * VDDH from the cell -- that would be a battery gauge with no divider and no wiring. It does not:
 * the reading moved by 3 mV across a full USB removal. See DEBUGGING.md trap 14.
 */
#define ADC_VDD_CHANNEL 0

static int16_t reference_buffer;

static int read_reference(uint8_t channel_id, uint8_t input, int32_t *out_mv)
{
    struct adc_channel_cfg cfg = {.gain = ADC_GAIN,
                                  .reference = ADC_REFERENCE,
                                  .acquisition_time = ADC_ACQUISITION,
                                  .channel_id = channel_id,
#ifdef CONFIG_ADC_NRFX_SAADC
                                  .input_positive = input
#endif
    };
    struct adc_sequence seq = {
        .channels = BIT(channel_id),
        .buffer = &reference_buffer,
        .buffer_size = sizeof(reference_buffer),
        .resolution = ADC_RESOLUTION,
    };
    int mv;

    if (adc_channel_setup(adc_battery_dev, &cfg) != 0 || adc_read(adc_battery_dev, &seq) != 0) {
        return -EIO;
    }

    mv = reference_buffer;
    if (adc_raw_to_millivolts(adc_ref_internal(adc_battery_dev), ADC_GAIN, ADC_RESOLUTION, &mv) != 0) {
        return -EIO;
    }

    *out_mv = mv;
    return 0;
}

static struct adc_sequence_options options = {
    .extra_samplings = ADC_TOTAL_SAMPLES - 1,
    .interval_us = 500, // Interval between each sample
};

struct adc_sequence sequence = {
    .options = &options,
    .channels = BIT(ADC_CHANNEL),
    .buffer = sample_buffer,
    .buffer_size = sizeof(sample_buffer),
    .resolution = ADC_RESOLUTION,
};

typedef struct {
    uint16_t voltage;
    uint8_t percentage;
} BatteryState;

// Measured on this board, not a datasheet curve.
//
// This was a generic "1S 250mAh LiPo discharge profile" ending {3255,2}, {3164,1}, {3000,0}. Those
// three rows describe voltages the hardware cannot reach: VDD held 3,293-3,309 mV across an entire
// discharge and never sagged, so what ends a run is the 3.3 V rail plus regulator dropout, around
// 3,330-3,390 mV. Reaching the old 1% point would have taken 15 h beyond the last sample of a cell
// that was flat within 6.5 h. In practice the gauge sank to about 10% and the board then died,
// never counting through 5, 2, 1. It also read low throughout the middle -- 50% at 3,756 mV where
// roughly three quarters of the runtime remained.
//
// These points come from 1,333 samples across one full discharge (see DEBUGGING.md trap 14),
// converted to percent-of-runtime-remaining, which is the only honest meaning available: the load
// is constant, so charge burned is proportional to elapsed time. 0% is the observed cutoff rather
// than a nominal empty, so the reading now reaches zero at about the moment the board stops.
//
// Caveats worth knowing before trusting the last digit. One cell, one discharge, one temperature.
// The bottom is bounded by a 6.5 h window in which the cell died unmonitored, and the top is
// extrapolated above 4,020 mV where logging began. Nothing safety-related reads this table --
// battery_guard() and battery_boot_gate() both work in millivolts -- so an error here costs
// display accuracy and nothing else.
#define BATTERY_STATES_COUNT 10
BatteryState battery_states[BATTERY_STATES_COUNT] = {
    {4150, 100}, // full charge as measured off the charger
    {4050, 92},
    {3950, 84},
    {3850, 73},
    {3750, 64},
    {3650, 48},
    {3550, 24}, // the plateau ends here and the knee begins
    {3500, 15}, // BATT_WARN_MV: yellow LED starts blinking
    {3450, 6},
    {3400, 0} // regulator dropout: the board stops within a few hundred mV/h of here
};

static uint8_t is_initialized = false;

static int battery_enable_read()
{
    return gpio_pin_set(gpio_battery_dev, GPIO_BATTERY_READ_ENABLE, 1);
}

int battery_set_fast_charge()
{
    if (!is_initialized) {
        return -ECANCELED;
    }

    return gpio_pin_set(gpio_battery_dev, GPIO_BATTERY_CHARGE_SPEED, 1); // FAST charge 100mA
}

int battery_set_slow_charge()
{
    if (!is_initialized) {
        return -ECANCELED;
    }

    return gpio_pin_set(gpio_battery_dev, GPIO_BATTERY_CHARGE_SPEED, 0); // SLOW charge 50mA
}

int battery_is_charging()
{
    if (!is_initialized) {
        return -ECANCELED;
    }

    // Configured GPIO_ACTIVE_LOW, so the logical value already reads the way the pin is named:
    // the BQ25100 pulls ~CHG low while it is charging, which arrives here as 1.
    return gpio_pin_get(gpio_battery_dev, GPIO_BATTERY_CHARGE_STATUS);
}

// Divider fitted on the XIAO between BAT+ and P0.31, switched to ground by P0.14.
#define BATTERY_R1 1000 // 1M ohm, high side
#define BATTERY_R2 510  // 510K ohm, low side

static int battery_measure(struct battery_diag *diag)
{
    int ret = 0;
    uint16_t adc_vref = adc_ref_internal(adc_battery_dev);
    int32_t counts = 0;
    int adc_mv;

    k_mutex_lock(&battery_mut, K_FOREVER);
    ret = adc_read(adc_battery_dev, &sequence);
    if (ret) {
        LOG_WRN("ADC read failed (error %d)", ret);
        k_mutex_unlock(&battery_mut);
        last_read_err = (int8_t) ret;
        return ret;
    }
    last_read_err = 0;

    for (uint8_t sample = 0; sample < ADC_TOTAL_SAMPLES; sample++) {
        counts += sample_buffer[sample];
    }
    counts /= ADC_TOTAL_SAMPLES;

    adc_mv = (int) counts;
    ret = adc_raw_to_millivolts(adc_vref, ADC_GAIN, ADC_RESOLUTION, &adc_mv);
    k_mutex_unlock(&battery_mut);
    if (ret) {
        return ret;
    }

    diag->raw_counts = (int16_t) counts;
    diag->adc_mv = adc_mv;
    // Scale in one expression rather than computing the ratio first. As two integers,
    // (R1 + R2) / R2 truncates 2.96 to 2, which someone previously compensated for by inflating
    // R1 until it truncated to 3 instead -- leaving a "calibration" that was really a rounding
    // artefact, and a reading 1.3% high.
    diag->battery_mv = ((int32_t) adc_mv * (BATTERY_R1 + BATTERY_R2)) / BATTERY_R2;
    return 0;
}

int battery_get_millivolt(uint16_t *battery_millivolt)
{
    struct battery_diag diag = {0};
    int ret = battery_measure(&diag);

    if (ret) {
        return ret;
    }

    // Clamp rather than cast. The SAADC returns small negative values when its input is floating,
    // and a negative millivolt count assigned straight to a uint16_t wraps to about 65000 -- sails
    // past the 4074 mV top of the discharge curve and reports a *full* battery. A device that
    // cannot measure its cell must never claim it is full; reporting empty at least fails loudly.
    if (diag.battery_mv < 0) {
        LOG_WRN("negative battery reading (%d mV) - sense input is probably floating", diag.battery_mv);
        *battery_millivolt = 0;
    } else if (diag.battery_mv > UINT16_MAX) {
        *battery_millivolt = UINT16_MAX;
    } else {
        *battery_millivolt = (uint16_t) diag.battery_mv;
    }

    LOG_DBG("%d mV", *battery_millivolt);
    return 0;
}

// Survives from the boot gate into the running system, which is the whole point: the pair of
// readings only means something if they come from the same charge.
static uint16_t boot_mv = 0;

void battery_note_boot_reading(uint16_t mv)
{
    boot_mv = mv;
}

int battery_get_diagnostics(struct battery_diag *diag)
{
    struct battery_diag off = {0};
    int ret;

    memset(diag, 0, sizeof(*diag));
    diag->init_err = init_err;
    diag->setup_err = setup_err;
    diag->gpio_err = gpio_err;
    diag->charging = (uint8_t) (battery_is_charging() == 1);
    diag->boot_mv = boot_mv;
    // Read both registers back from the hardware rather than trusting that the write happened.
    // OUT alone is not enough: it reads 0 on a pin that was never made an output, which looks
    // identical to a pin correctly driven low while the divider is actually switched off.
    diag->read_enable = (uint8_t) nrf_gpio_pin_out_read(GPIO_BATTERY_READ_ENABLE);
    diag->enable_is_output = (uint8_t) (nrf_gpio_pin_dir_get(GPIO_BATTERY_READ_ENABLE) == NRF_GPIO_PIN_DIR_OUTPUT);

    // Switch the divider off and sample the other state. Zero in both states means nothing is
    // arriving from BAT+ at all; a high reading here with zero when switched on would instead
    // mean the resistors are fine and the switch is at fault. Restored immediately after.
    if (gpio_pin_set(gpio_battery_dev, GPIO_BATTERY_READ_ENABLE, 0) == 0) {
        k_msleep(2);
        if (battery_measure(&off) == 0) {
            diag->off_counts = off.raw_counts;
        }
        (void) battery_enable_read();
        k_msleep(2);
    }

    ret = battery_measure(diag);
    diag->read_err = last_read_err;

    // Set up lazily, so the normal gauge path never pays for the control.
    k_mutex_lock(&battery_mut, K_FOREVER);
    (void) read_reference(ADC_VDD_CHANNEL, SAADC_CH_PSELP_PSELP_VDD, &diag->vdd_mv);
    k_mutex_unlock(&battery_mut);

    if (ret) {
        return ret;
    }

    battery_get_percentage(&diag->percentage, diag->battery_mv < 0 ? 0 : (uint16_t) diag->battery_mv);
    return 0;
}

int battery_get_percentage(uint8_t *battery_percentage, uint16_t battery_millivolt)
{
    // Ensure voltage is within bounds
    if (battery_millivolt >= battery_states[0].voltage) {
        *battery_percentage = battery_states[0].percentage;
        LOG_DBG("%d %%", *battery_percentage);
        return 0;
    }
    if (battery_millivolt <= battery_states[BATTERY_STATES_COUNT - 1].voltage) {
        *battery_percentage = battery_states[BATTERY_STATES_COUNT - 1].percentage;
        LOG_DBG("%d %%", *battery_percentage);
        return 0;
    }

    for (uint16_t i = 0; i < BATTERY_STATES_COUNT - 1; i++) {
        // Find the two points battery_millivolt is between
        if (battery_millivolt <= battery_states[i].voltage && battery_millivolt > battery_states[i + 1].voltage) {
            // Linear interpolation
            float voltage_range = (float) (battery_states[i].voltage - battery_states[i + 1].voltage);
            float percentage_range = (float) (battery_states[i].percentage - battery_states[i + 1].percentage);
            float position = (float) (battery_states[i].voltage - battery_millivolt) / voltage_range;

            *battery_percentage = battery_states[i].percentage - (uint8_t) (position * percentage_range);

            LOG_DBG("%d %%", *battery_percentage);
            return 0;
        }
    }
    return -ESPIPE;
}

int battery_init()
{
    int gpio_ret;

    // ADC
    if (!device_is_ready(adc_battery_dev)) {
        LOG_ERR("ADC device not found!");
        init_err = -EIO;
        return -EIO;
    }

    setup_err = (int8_t) adc_channel_setup(adc_battery_dev, &channel_7_cfg);
    if (setup_err) {
        LOG_ERR("ADC setup failed (error %d)", setup_err);
    }

    // GPIO
    if (!device_is_ready(gpio_battery_dev)) {
        LOG_ERR("GPIO device not found!");
        init_err = -EIO;
        return -EIO;
    }

    // ~CHG is an input: it is the charger reporting, not us commanding. The pull-up keeps it
    // defined while the charger's open-drain output is released.
    gpio_ret =
        gpio_pin_configure(gpio_battery_dev, GPIO_BATTERY_CHARGE_STATUS, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
    // Driven low here rather than left to battery_enable_read(): GPIO_OUTPUT on its own leaves the
    // level undefined until something sets it, which is a divider that is off for as long as it
    // takes to notice.
    gpio_ret |= gpio_pin_configure(gpio_battery_dev, GPIO_BATTERY_READ_ENABLE, GPIO_OUTPUT_ACTIVE | GPIO_ACTIVE_LOW);
    gpio_ret |= gpio_pin_configure(gpio_battery_dev, GPIO_BATTERY_CHARGE_SPEED, GPIO_OUTPUT | GPIO_ACTIVE_LOW);
    gpio_err = (int8_t) gpio_ret;

    if (gpio_ret) {
        LOG_ERR("GPIO configure failed (error %d)", gpio_ret);
        init_err = (int8_t) gpio_ret;
        return gpio_ret;
    }

    is_initialized = true;
    LOG_INF("Initialized");

    // Enabling the divider is what makes the gauge work at all, so it must not be skipped just
    // because the ADC channel setup complained. Previously a single accumulated `ret` carried the
    // ADC result down here and returned early, leaving P0.31 floating and the reported percentage
    // pinned to 0% or 100% depending on the sign of the noise -- with nothing to say why.
    int enable_ret = battery_enable_read();
    if (enable_ret) {
        LOG_ERR("enabling the battery divider failed (error %d)", enable_ret);
    }
    int charge_ret = battery_set_fast_charge();

    init_err = (int8_t) (enable_ret ? enable_ret : (charge_ret ? charge_ret : setup_err));
    LOG_INF("charger reports %s", battery_is_charging() == 1 ? "charging" : "not charging");
    return init_err;
}
