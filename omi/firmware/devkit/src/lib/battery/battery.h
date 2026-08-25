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

#include <stdint.h>

#ifndef __BATTERY_H__
#define __BATTERY_H__

/**
 * @brief Set battery charging to fast charge (100mA).
 *
 * @retval 0 if successful. Negative errno number on error.
 */
int battery_set_fast_charge(void);

/**
 * @brief Set battery charging to slow charge (50mA).
 *
 * @retval 0 if successful. Negative errno number on error.
 */
int battery_set_slow_charge(void);

/**
 * @brief Whether the charger is currently charging the cell.
 *
 * @retval 1 charging, 0 not charging, negative errno on error.
 *
 * @note Not charging covers three different situations -- fully charged, no USB, and no cell --
 * because the BQ25100 reports them all the same way on one pin. Charging starting and then
 * stopping after a minute is how a *fully charged* cell looks, and also how a *missing* one does.
 */
int battery_is_charging(void);

/**
 * @brief Calculates the battery voltage using the ADC.
 *
 * @param[in] battery_millivolt Pointer to where battery voltage is stored.
 *
 * @retval 0 if successful. Negative errno number on error.
 */
int battery_get_millivolt(uint16_t *battery_millivolt);

/**
 * @brief Calculates the battery percentage using the battery voltage.
 *
 * @param[in] battery_percentage  Pointer to where battery percentage is stored.
 *
 * @param[in] battery_millivolt Voltage used to calculate the percentage of how much energy is left in a 3.7V LiPo
 * battery.
 *
 * @retval 0 if successful. Negative errno number on error.
 */
int battery_get_percentage(uint8_t *battery_percentage, uint16_t battery_millivolt);

/**
 * @brief Initialize the battery charging circuit.
 *
 * @retval 0 if successful. Negative errno number on error.
 */
int battery_init(void);

/**
 * @brief Everything behind the battery percentage, for working out why it is wrong.
 *
 * The gauge has exactly one output and three ways to be wrong -- a disabled divider, a failed
 * init, or bad arithmetic -- and they are indistinguishable from the percentage alone. A floating
 * ADC input in particular reports 0% or 100% depending only on the sign of the noise, which reads
 * as a flat or a full battery rather than as no measurement at all.
 *
 * Serial is not usable on this board (DEBUGGING.md trap 8), so these go out over BLE.
 *
 * `off_counts` is the reading with the divider switched off, and it is the field that identifies
 * *which* part is broken. A reading of zero while switched on is ambiguous on its own -- it fits
 * both an open high side and a switch stuck off -- so the complementary state has to be sampled
 * to tell them apart:
 *
 *   off high, on low     both resistors fine, the switch is not switching
 *   off low,  on low     nothing is arriving from BAT+; the high side is open
 *   off high, on scaled  working normally
 *
 * `vdd_mv` reads the chip's own 3.3 V supply through the same ADC, reference and gain, and is
 * what makes any of the above admissible. A broken converter returns zero exactly like a dead
 * pin does, so without a known-good input on the same hardware every conclusion drawn from a
 * zero is really a guess about which of the two failed.
 */
struct battery_diag {
    int16_t raw_counts;       /**< averaged raw ADC counts, signed: near zero means nothing is connected */
    int32_t adc_mv;           /**< raw_counts converted to millivolts at the ADC pin */
    int32_t battery_mv;       /**< adc_mv scaled by the divider, before any clamping */
    uint8_t percentage;       /**< what the gauge would report */
    uint8_t read_enable;      /**< OUT register for P0.14: 0 = we are driving the divider on */
    uint8_t enable_is_output; /**< DIR register for P0.14: 0 means the drive above never happened */
    int16_t off_counts;       /**< counts with the divider switched off -- see below */
    int32_t vdd_mv;           /**< same ADC pointed at the 3.3V supply: the control, see below */
    uint8_t charging;         /**< P0.17 (~CHG) from the BQ25100: 1 = charging */
    uint16_t boot_mv;         /**< what the boot gate measured before any load came up; 0 if it did not run */
    int8_t init_err;          /**< result of the last battery_init() */
    int8_t setup_err;         /**< result of adc_channel_setup() during init */
    int8_t gpio_err;          /**< result of configuring the three control pins */
    int8_t read_err;          /**< result of the last adc_read() */
};

/**
 * @brief Fill in a snapshot of the gauge's inputs. Performs a fresh ADC read.
 */
int battery_get_diagnostics(struct battery_diag *diag);

/**
 * @brief Record the voltage the boot gate measured, before the radio, mic and card came up.
 *
 * Exists to make the gate's threshold answerable with evidence rather than argument. The threshold
 * has to sit above the runtime shutdown threshold by however much the cell sags under load, and
 * that figure was assumed rather than measured when the gate was written. Reading this alongside
 * the running voltage on the same charge gives the difference directly: boot_mv is taken with
 * essentially nothing switched on, the running value with everything.
 *
 * Reported over BLE because the interesting case -- waking after a low-battery shutdown -- happens
 * on battery, where there is no USB console to print to.
 */
void battery_note_boot_reading(uint16_t mv);

#endif