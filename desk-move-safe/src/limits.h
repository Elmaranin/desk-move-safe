#ifndef LIMITS_H
#define LIMITS_H
//
// limits — the flap's calibrated travel range: a stored expanded and collapsed
// end, motion refused outside them, and nothing but a capped jog until both
// are stored.
//
// THE TWO ENCODER ANGLES ARE THE ONLY TRUTH. The MT6835's raw 21-bit angle
// means the same thing after a power cut, so each end is stored as one. Beside
// them only two signs are kept, both found at calibration:
//
//   enc_dir   which way round the circle the flap travels from expanded to
//             collapsed (+1: the raw angle counts up). Two angles on a circle
//             make two arcs; this says which one is the flap's.
//   mot_dir   which way the motor turns to collapse it (+1: positive steps),
//             learned from watching the encoder while the motor jogs.
//
// No step count is stored. A move is STEERED BY THE ENCODER: the motor runs
// towards the target, the encoder task watches the remaining distance on
// every sample and ramps it down so it stops lim_approach_deg short, and the
// creep finishes onto the target. Lost steps, a miscounted calibration or a
// gear ratio that is a little off cannot make it fall short. The gear ratio
// is used for ESTIMATES only — the braking distance, what a jog is in degrees.
//
// The values live in settings()->lim; every change marks the settings dirty
// and settings.c writes the one record. limits.c never calls nvs itself.
//
// The whole range must fit inside one turn of the shaft the magnet is on.
//
// Three states:
//   LOCKED    no range stored. Only 'mot jog' moves the motor, capped to one
//             motor revolution per command — enough to find the ends.
//   BOUNDED   both ends stored. Every move must end inside them, and the
//             encoder guard hard-stops anything that leaves anyway.
//   FREE      session-only override ('lim free'). Off at every boot.
//
// The desk also depends on this: mode.c refuses every desk move until both
// ends are stored and the encoder answers. See mode.h.
//
#include <stdbool.h>
#include <stdint.h>

typedef enum { LIM_LOCKED, LIM_BOUNDED, LIM_FREE } lim_mode_t;

// After settings_load(): print one boot line.
void       limits_init(void);
lim_mode_t limits_mode(void);
bool       limits_calibrated(void);
void       limits_set_free(bool on);

// The gate every motor move goes through. Prints why when it refuses.
bool limits_move(int32_t steps);        // relative microsteps, range-checked
bool limits_run(bool forward);          // to the end of travel that way
bool limits_jog(int32_t steps);         // the one move allowed while LOCKED

// Encoder-steered moves along the range: degrees from the expanded end.
bool    limits_goto_deg(double deg_from_expanded);
bool    limits_goto_end(bool collapsed);    // false: expanded, true: collapsed
double  limits_range_deg(void);             // expanded -> collapsed, degrees

// How much of the last targeted move is done, by the encoder: 0 at its start,
// 1 on the target (clamped). Negative if it cannot be told.
double  limits_progress(void);

// Is the flap at that end now, by the encoder, to within a few degrees?
// False if it cannot be told (no range, no encoder).
bool    limits_at_end(bool collapsed);

// Which end the flap is nearer now: 0 expanded, 1 collapsed, -1 unknown.
int     limits_nearer_end(void);

// After a targeted move has stopped: creep onto the target under the encoder.
// 0 on target, 1 gave up, -1 nothing to settle.
bool limits_targeted(void);
int  limits_settle(double *err_deg, int32_t *crept);

// Store the shaft's position as the expanded or collapsed end. Both in one
// session, with the encoder not zeroed in between, so the direction of travel
// is known — and with the motor jogged at least once, so its direction is.
bool limits_mark(bool collapsed);
bool limits_clear(void);                // 'reset': forget the calibration only

bool limits_set_approach(double degrees);
bool limits_measure_play(double move_deg);
bool limits_set_play(int32_t steps);

void limits_report(void);

// ---- encoder task, every sample: the guard and the steering ----
void limits_guard(uint32_t raw);
// ---- console loop: print a guard trip or a run fault, once ----
void limits_poll_trip(void);

#endif // LIMITS_H
