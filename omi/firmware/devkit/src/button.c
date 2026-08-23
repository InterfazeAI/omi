#include "button.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include "led.h"
#include "mic.h"
#include "sdcard.h"
#include "speaker.h"
#include "storage.h"
#include "transport.h"
#include "wdog_facade.h"
LOG_MODULE_REGISTER(button, CONFIG_LOG_DEFAULT_LEVEL);

bool is_off = false;
static void button_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t button_data_read_characteristic(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr,
                                               void *buf,
                                               uint16_t len,
                                               uint16_t offset);
static struct gpio_callback button_cb_data;

static struct bt_uuid_128 button_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x23BA7924, 0x0000, 0x1000, 0x7450, 0x346EAC492E92));
static struct bt_uuid_128 button_characteristic_data_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x23BA7925, 0x0000, 0x1000, 0x7450, 0x346EAC492E92));

static struct bt_gatt_attr button_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&button_uuid),
    BT_GATT_CHARACTERISTIC(&button_characteristic_data_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           button_data_read_characteristic,
                           NULL,
                           NULL),
    BT_GATT_CCC(button_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

static struct bt_gatt_service button_service = BT_GATT_SERVICE(button_service_attr);

static void button_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        LOG_INF("Client subscribed for notifications");
    } else if (value == 0) {
        LOG_INF("Client unsubscribed from notifications");
    } else {
        LOG_ERR("Invalid CCC value: %u", value);
    }
}
/*
 * Both pins are active high, which is what dt_flags = 0 means.
 *
 * These previously held GPIO_OUTPUT_ACTIVE and GPIO_INT_EDGE_RISING. Neither belongs in dt_flags,
 * which carries only the devicetree-level flags (active level, drive strength) and is 16 bits
 * wide -- both constants live above bit 16, so both truncated to 0 and the compiler said so with
 * -Woverflow. Active high is what the rest of this file assumes, so the behaviour was right by
 * accident. Saying 0 outright keeps that behaviour and drops the warnings. The configuration and
 * interrupt flags that were meant here are passed explicitly to the calls in button_init().
 */
struct gpio_dt_spec d4_pin = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin = 4, .dt_flags = 0};
struct gpio_dt_spec d5_pin_input = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin = 5, .dt_flags = 0};

static bool was_pressed = false;

//
// button
//
void button_pressed_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    int temp = gpio_pin_get_raw(dev, d5_pin_input.pin);
    LOG_PRINTK("button_pressed_callback %d\n", temp);
    if (temp) {
        was_pressed = false;
    } else {
        was_pressed = true;
    }
}
#define BUTTON_CHECK_INTERVAL 40 // 0.04 seconds, 25 Hz

void check_button_level(struct k_work *work_item);

K_WORK_DELAYABLE_DEFINE(button_work, check_button_level);

#define DEFAULT_STATE 0
#define SINGLE_TAP 1
#define DOUBLE_TAP 2
#define LONG_TAP 3
#define BUTTON_PRESS 4
#define BUTTON_RELEASE 5

// 4 is button down, 5 is button up
static FSM_STATE_T current_button_state = IDLE;

static int final_button_state[2] = {0, 0};

static inline void notify_press()
{
    final_button_state[0] = BUTTON_PRESS;
    LOG_INF("Button pressed");
    struct bt_conn *conn = get_current_connection();
    if (conn != NULL) {
        bt_gatt_notify(conn, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));
    }
}

static inline void notify_unpress()
{
    final_button_state[0] = BUTTON_RELEASE;
    LOG_INF("Button released");
    struct bt_conn *conn = get_current_connection();
    if (conn != NULL) {
        bt_gatt_notify(conn, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));
    }
}

static inline void notify_tap()
{
    final_button_state[0] = SINGLE_TAP;
    LOG_INF("Button single tap");
    struct bt_conn *conn = get_current_connection();
    if (conn != NULL) {
        bt_gatt_notify(conn, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));
    }
}

static inline void notify_double_tap()
{
    final_button_state[0] = DOUBLE_TAP; // button press
    LOG_INF("Button double tap");
    struct bt_conn *conn = get_current_connection();
    if (conn != NULL) {
        bt_gatt_notify(conn, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));
    }
}

static inline void notify_long_tap()
{
    final_button_state[0] = LONG_TAP; // button press
    LOG_INF("Button long tap");
    struct bt_conn *conn = get_current_connection();
    if (conn != NULL) {
        bt_gatt_notify(conn, &button_service.attrs[1], &final_button_state, sizeof(final_button_state));
    }
}

#define BUTTON_PRESSED 1
#define BUTTON_RELEASED 0

#define TAP_THRESHOLD 300    // a press shorter than this counts as a tap
#define MULTI_TAP_WINDOW 600 // longest gap between taps belonging to one gesture
#define RESET_WARN_TIME 2000 // yellow LED: keep holding and the recordings go
#define RESET_HOLD_TIME 5000 // erase the card and release the bond

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_SINGLE_TAP,
    BUTTON_EVENT_DOUBLE_TAP,
    BUTTON_EVENT_TRIPLE_TAP,
} ButtonEvent;

// Ticks of BUTTON_CHECK_INTERVAL. Free-running: every comparison is a subtraction, which stays
// correct across the u32 wrap. The old code zeroed this on release and compared absolute values,
// so a press that straddled the reset measured its duration from the wrong origin.
static uint32_t current_time = 0;
static uint32_t btn_press_start_time;
static uint32_t btn_last_release_time;
static bool btn_is_pressed;
static uint8_t tap_count;
static bool reset_fired;

bool button_reset_warning = false;

static uint32_t elapsed_ms(uint32_t since)
{
    return (current_time - since) * BUTTON_CHECK_INTERVAL;
}

static void show_reset_warning(bool on)
{
    if (button_reset_warning == on) {
        return;
    }
    button_reset_warning = on;

    // Yellow is red and green together. set_led_state() in main.c leaves the LEDs alone while the
    // flag is set, otherwise its 500 ms refresh would blink the warning away as fast as it appears.
    set_led_red(on);
    set_led_green(on);
}

void check_button_level(struct k_work *work_item)
{
    current_time++;

    uint8_t btn_state = was_pressed ? BUTTON_PRESSED : BUTTON_RELEASED;
    ButtonEvent event = BUTTON_EVENT_NONE;

    if (btn_state == BUTTON_PRESSED && !btn_is_pressed) {
        btn_is_pressed = true;
        btn_press_start_time = current_time;
        reset_fired = false;
    } else if (btn_state == BUTTON_RELEASED && btn_is_pressed) {
        btn_is_pressed = false;
        btn_last_release_time = current_time;
        show_reset_warning(false);

        if (elapsed_ms(btn_press_start_time) < TAP_THRESHOLD) {
            tap_count++;
        } else {
            // A hold that was let go before RESET_HOLD_TIME. It is not a tap, and it must not
            // combine with the next one into a two-tap gesture.
            tap_count = 0;
        }
        notify_unpress();
        current_button_state = GRACE;
    }

    // Held down: warn first, then fire once. reset_fired keeps a continued hold from re-arming it.
    if (btn_is_pressed) {
        uint32_t held = elapsed_ms(btn_press_start_time);
        if (held >= RESET_HOLD_TIME) {
            if (!reset_fired) {
                reset_fired = true;
                tap_count = 0;
                LOG_PRINTK("reset hold: erasing the card, then releasing the bond\n");
#if defined(CONFIG_BT_SMP)
                // Only asks. The storage thread erases the card and calls transport_finish_unbond()
                // afterwards, so an interruption leaves the bond intact rather than leaving an
                // unwiped card open to whoever pairs next. Same path as the BLE release command.
                storage_request_unbond_wipe();
#endif
            }
        } else if (held >= RESET_WARN_TIME) {
            show_reset_warning(true);
        }
    }

    // Taps resolve only once the window for another one has closed. Reporting each tap as it
    // happens would make a triple arrive as a single and then a double, and the double is a
    // gesture of its own -- the count has to be settled before anything is emitted.
    if (!btn_is_pressed && tap_count > 0 && elapsed_ms(btn_last_release_time) > MULTI_TAP_WINDOW) {
        event = (tap_count == 1)   ? BUTTON_EVENT_SINGLE_TAP
                : (tap_count == 2) ? BUTTON_EVENT_DOUBLE_TAP
                                   : BUTTON_EVENT_TRIPLE_TAP;
        tap_count = 0;
    }

    switch (event) {
    case BUTTON_EVENT_SINGLE_TAP:
        LOG_PRINTK("single tap detected\n");
        notify_tap();
        break;
    case BUTTON_EVENT_DOUBLE_TAP:
        LOG_PRINTK("double tap detected\n");
        notify_double_tap();
        break;
    case BUTTON_EVENT_TRIPLE_TAP:
        LOG_PRINTK("triple tap detected -- powering down\n");
        is_off = true;
        bt_off();
        turnoff_all(); // does not return: ends in SYSTEMOFF
        break;
    default:
        break;
    }

    k_work_reschedule(&button_work, K_MSEC(BUTTON_CHECK_INTERVAL));
}

static ssize_t button_data_read_characteristic(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr,
                                               void *buf,
                                               uint16_t len,
                                               uint16_t offset)
{
    LOG_INF("button_data_read_characteristic");
    LOG_PRINTK("was_pressed: %d\n", final_button_state[0]);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &final_button_state, sizeof(final_button_state));
}

int button_init()
{
    if (gpio_is_ready_dt(&d4_pin)) {
        LOG_INF("D4 Pin ready");
    } else {
        LOG_ERR("Error setting up D4 Pin");
        return -1;
    }
    /*
     * Driven low, so D4 is a ground for a button wired between D4 and D5.
     *
     * It used to be driven high, which cannot work: D5 idles high on its own pull-up, so a button
     * bridging D4 and D5 shorts high to high. The pin never moves, the edge interrupt never fires,
     * and every gesture is silently lost -- the board looks like it has no button at all rather
     * than a broken one.
     *
     * Low also leaves a button wired between D5 and GND working, since D4 is then unconnected.
     * One firmware, both wirings. The press current is the pull-up across ~13k, a couple hundred
     * microamps, which this pin sinks comfortably.
     */
    if (gpio_pin_configure_dt(&d4_pin, GPIO_OUTPUT_INACTIVE) < 0) {
        LOG_ERR("Error setting up D4 Pin Voltage");
        return -1;
    } else {
        LOG_INF("D4 held low as the button return path");
    }
    if (gpio_is_ready_dt(&d5_pin_input)) {
        LOG_INF("D5 Pin ready");
    } else {
        LOG_ERR("D5 Pin not ready");
        return -1;
    }

    /*
     * Pulled up, not bare. The button shorts D5 to ground, so "not pressed" has to be held high
     * by something; with nothing holding it the pin floats, drifts low on its own, and the state
     * machine below reads three seconds of that as a long press -- which is the gesture that
     * calls turnoff_all() and SYSTEMOFF. A board that switches itself off at random looks like a
     * power fault rather than a GPIO configuration bug, and it is why CONFIG_OMI_ENABLE_BUTTON
     * had to be turned off on the devkit build.
     *
     * The pull-up also makes the wake work. turnoff_all() arms GPIO_INT_LEVEL_INACTIVE as the
     * System OFF wake source, which fires when D5 goes low; a floating pin would trip that
     * immediately and the board would wake straight back up.
     */
    int err2 = gpio_pin_configure_dt(&d5_pin_input, GPIO_INPUT | GPIO_PULL_UP);

    if (err2 != 0) {
        LOG_ERR("Error setting up D5 Pin");
        return -1;
    } else {
        LOG_INF("D5 ready");
    }
    // GPIO_INT_LEVEL_INACTIVE
    err2 = gpio_pin_interrupt_configure_dt(&d5_pin_input, GPIO_INT_EDGE_BOTH);

    if (err2 != 0) {
        LOG_ERR("D5 unable to detect button presses");
        return -1;
    } else {
        LOG_INF("D5 ready to detect button presses");
    }

    gpio_init_callback(&button_cb_data, button_pressed_callback, BIT(d5_pin_input.pin));
    gpio_add_callback(d5_pin_input.port, &button_cb_data);

    return 0;
}

void activate_button_work()
{
    k_work_schedule(&button_work, K_MSEC(BUTTON_CHECK_INTERVAL));
}

void register_button_service()
{
    bt_gatt_service_register(&button_service);
}

FSM_STATE_T get_current_button_state()
{
    return current_button_state;
}

void turnoff_all()
{

    mic_off();
    sd_off();
    speaker_off();
    accel_off();
    play_haptic_milli(50);
    k_msleep(100);
    set_led_blue(false);
    set_led_red(false);
    set_led_green(false);
    gpio_remove_callback(d5_pin_input.port, &button_cb_data);
    gpio_pin_interrupt_configure_dt(&d5_pin_input, GPIO_INT_LEVEL_INACTIVE);

    // Disable watchdog before entering system off
    int rc = watchdog_deinit();
    if (rc < 0) {
        LOG_ERR("Failed to deinitialize watchdog (%d)", rc);
    }

    // maybe save something here to indicate success. next time the button is pressed we should know about it
    NRF_USBD->INTENCLR = 0xFFFFFFFF;
    NRF_POWER->SYSTEMOFF = 1;
}

void force_button_state(FSM_STATE_T state)
{
    current_button_state = state;
}
