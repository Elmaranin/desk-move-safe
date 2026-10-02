#ifndef STEPPER_H
#define STEPPER_H
//
// stepper — the step-pulse engine, ported from ../../../test/nema_MT6835.
//
// Everything the driver knows about MOTION comes from the interval between
// STEP edges, so this module is really a timer that reschedules itself with a
// shrinking (then growing) period. What the driver knows about ITSELF —
// current, microstepping, chopper — comes from its own strapping, or over UART
// if that is enabled; either way not from here. See tmc2209.h.
//
// The ramp is Austin's integer recurrence from AVR446, "Linear speed control of
// a stepper motor": no sqrt and no floating point per step, only one multiply
// and one divide, so the whole update fits comfortably in the alarm ISR.
//
//   d[0] = 0.676 * 1e6 * sqrt(2 / accel)      microseconds, computed once
//   d[n] = d[n-1] - 2 * d[n-1] / (4n + 1)     per step while accelerating
//
// The same recurrence with a negative n *increases* the interval, which is how
// deceleration works: to stop from speed we set n = -n, and the ramp retraces
// itself step for step. That is also why a trapezoid needs no precomputed
// profile — n is the number of steps we climbed, so it is exactly the number we
// need to come back down.
//
// All calls are non-blocking; the motion continues in the alarm ISR. Only one
// move may be in flight at a time — start a new one and it is rejected until
// the current one finishes or is stopped.
//
// NOTHING HERE TOUCHES THE DESK BUS. The alarm ISR runs below FreeRTOS's
// syscall priority and does nothing but toggle a pin, so the bus task keeps
// its deadline whatever the motor is doing.
//
#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// Claim the pins and a hardware alarm, and leave the motor DISABLED. Call once
// from main() before the scheduler starts, and before tmc2209_init(): the EN
// pin has to be holding the coils off before anything else happens.
void stepper_init(void);

// Both halves of "enabled": the EN pin AND the driver's chopper over UART.
// They are deliberately separate — see tmc2209.h. Returns false if the driver
// could not be told, in which case nothing is enabled and no move may start.
//
// stepper_move()/stepper_run() enable implicitly. Disabling lets the flap be
// moved by hand and drops the holding current.
bool stepper_enable(bool on);
bool stepper_enabled(void);

// Profile limits, in microsteps. Applied to the *next* move, not the one in
// flight. Both are clamped to something the hardware can actually deliver.
void     stepper_set_speed(uint32_t steps_per_s);
void     stepper_set_accel(uint32_t steps_per_s2);
uint32_t stepper_speed(void);
uint32_t stepper_accel(void);

// Relative move with a full accelerate / cruise / decelerate profile. Negative
// steps run the other way. Returns false if a move is already in flight, or if
// the driver could not be enabled.
bool stepper_move(int32_t steps);

// Accelerate to the speed limit and hold it until stepper_stop().
bool stepper_run(bool forward);

// Decelerate down the same ramp we climbed and stop. Safe to call when idle.
void stepper_stop(void);

// Cut the pulse train this instant. The motor will almost certainly lose
// position — this is the panic button, not a way to end a move. Deliberately
// does NOT talk to the driver: it must work with the UART link dead.
void stepper_stop_hard(void);

// Block until the motion ends. Returns false on timeout. Only one task may wait
// at a time; a second waiter silently displaces the first.
bool stepper_wait(uint32_t timeout_ms);

// Microsteps in one motor revolution — follows the live UART microstep
// setting, so "revolutions" means the same thing however it is configured.
uint32_t stepper_steps_per_rev(void);

bool     stepper_busy(void);
int32_t  stepper_pos(void);      // signed microstep counter since the last zero
void     stepper_zero(void);
// Bumped every time stepper_pos() stops meaning what it did: on a zero and on
// a hard stop. Anything that keeps an offset against the counter (limits.c,
// when it arrives) compares this to know its offset is stale.
uint32_t stepper_pos_gen(void);
// Direction of the most recent motion, +1 or -1 — which side of the gearbox's
// play the motor is resting on. +1 before anything has moved since boot.
int8_t   stepper_last_dir(void);
uint32_t stepper_cur_sps(void);  // instantaneous step rate, 0 when idle

#endif // STEPPER_H
