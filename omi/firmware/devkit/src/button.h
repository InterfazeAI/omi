#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>

typedef enum { IDLE, GRACE } FSM_STATE_T;

/**
 * @brief True while the button has been held long enough to be warning about a reset.
 *
 * The LED is yellow for the seconds between the warning and the reset itself, and letting go
 * during them cancels it. set_led_state() must leave the LEDs alone while this is set, or its
 * refresh overwrites the warning and the only feedback before an irreversible erase disappears.
 */
extern bool button_reset_warning;

int button_init();
void activate_button_work();
void register_button_service();
void turnoff_all();
FSM_STATE_T get_current_button_state();

void force_button_state(FSM_STATE_T state);

#endif
