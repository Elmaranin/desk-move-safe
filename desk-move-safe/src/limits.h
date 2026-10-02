#ifndef LIMITS_H
#define LIMITS_H
//
// limits — the flap's calibrated travel range: a stored min and max, motion
// refused outside them, and nothing but a capped jog until both are stored.
//
// Ported from the bench rig (test/nema_MT6835/src/limits.c). The one change is
// where the numbers live: not a record of its own but settings()->lim, so the
// desk settings and the flap range share the single nvs record. Every change
// here marks the settings dirty and settings.c writes it once the motor is
// still — limits.c never calls nvs itself.
//
// The ends are stored as the MT6835's raw 21-bit angle — the one number that
// means the same thing after a power cycle — plus the exact number of
// microsteps between them. The angle answers "where is the shaft?" at boot;
// the step count is what a move to an end is made of.
//
// The whole range must fit inside one turn of the shaft the magnet is on.
//
// Three states:
//   LOCKED    no range stored. Only 'mot jog' moves the motor, capped to one
//             motor revolution per command — enough to find the ends.
//   BOUNDED   min and max stored. Every move must start and end inside them,
//             and the encoder guard hard-stops anything that leaves anyway.
//   FREE      session-only override ('lim free'). Off at every boot.
//
// The desk also depends on this: mode.c refuses every desk move until both
// ends are stored and the encoder answers. See mode.h.
//
#include <stdbool.h>
#include <stdint.h>

typedef enum { LIM_LOCKED, LIM_BOUNDED, LIM_FREE } lim_mode_t;

// After settings_load(): rescale the stored span if the microstepping changed,
// and print one boot line.
void       limits_init(void);
lim_mode_t limits_mode(void);
bool       limits_calibrated(void);
void       limits_set_free(bool on);

// The gate every motor move goes through. Prints why when it refuses.
bool limits_move(int32_t steps);        // relative microsteps, range-checked
bool limits_run(bool forward);          // to the end of travel that way
bool limits_jog(int32_t steps);         // the one move allowed while LOCKED

// Absolute moves along the range: 0 is min, limits_span_steps() is max.
bool    limits_goto(int32_t axis_steps);
bool    limits_goto_end(bool to_max);
int32_t limits_span_steps(void);

// After a targeted move has stopped: creep onto the target under the encoder.
// 0 on target, 1 gave up, -1 nothing to settle.
bool limits_targeted(void);
int  limits_settle(double *err_deg, int32_t *crept);

// Store the shaft's position as min or max. Both in one session, with no
// counter zeroed in between, so the steps between them are counted.
bool limits_mark(bool is_max);
bool limits_clear(void);                // 'reset': forget the calibration only

bool limits_store_span(void);           // re-measure, shaft on max after 'mot go'
bool limits_set_span(int32_t steps);
bool limits_set_approach(double degrees);
bool limits_measure_play(double move_deg);
bool limits_set_play(int32_t steps);

// Re-seed the step counter from the absolute angle.
bool limits_sync(bool verbose);

void limits_report(void);

// ---- encoder task, every sample ----
void limits_guard(uint32_t raw);
// ---- console loop: print a guard trip, once ----
void limits_poll_trip(void);

#endif // LIMITS_H
