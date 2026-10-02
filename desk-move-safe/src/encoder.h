#ifndef ENCODER_H
#define ENCODER_H
//
// encoder — turns the MT6835's within-one-turn angle into a continuous position.
//
// Ported from ../../../test/nema_MT6835, less the travel-limit hooks: those
// arrive with limits.c in step 3.
//
// The sensor only knows where the shaft is inside a single revolution: it counts
// 0..2097151 and wraps. encoder_task polls it and accumulates the differences,
// so the count keeps going past a full turn and can be compared directly
// against the stepper's commanded position.
//
// WHY IT IS HERE AT ALL. Open-loop step/dir emits an identical pulse train
// whether the motor followed it or not, so a stall is completely silent — the
// firmware reports a clean move either way. Measuring the shaft turns "did it
// actually get there" from something you listen for into a number in degrees.
//
// The polling rate is the correctness constraint. Unwrapping assumes the shaft
// moved less than half a turn between samples. The magnet is on the OUTPUT of a
// reducer, which is the slow side: at the fastest the motor will run, the output
// turns well under one revolution per second, so even a 100 ms period would
// unwrap correctly. ENCODER_PERIOD_MS is far below that — the margin is for the
// display being responsive, not for correctness — and encoder_jumps() counts
// how close any sample came to the limit.
//
#include <stdbool.h>
#include <stdint.h>

#include "mt6835.h"

void encoder_task(void *arg);

// Counts per revolution of the measured shaft, straight from the sensor.
#define ENCODER_CPR     MT6835_CPR

// True once the sensor has answered at least once. Everything else returns
// zeroes when it has not, so the firmware still runs open-loop without a sensor.
bool encoder_available(void);

// Continuous position, ENCODER_CPR counts per revolution, signed, relative to
// the last encoder_zero(). Sampled by encoder_task, so it lags reality by up to
// one polling period. 64-bit because 2^21 counts a turn overflows 32 bits after
// only 1024 revolutions.
int64_t encoder_counts(void);
double  encoder_degrees(void);
double  encoder_revs(void);

// Reset the continuous count to zero at the shaft's current angle.
void encoder_zero(void);

// Bumped by every encoder_zero(), so a caller holding a count taken earlier can
// tell it is no longer comparable with the current one.
uint32_t encoder_zero_gen(void);

// The most recent raw 21-bit sample, 0..ENCODER_CPR-1 — the shaft's absolute
// angle within a turn, untouched by zeroing. What the travel limits will be
// stored in, since it is the one number that means the same thing after a reset.
uint32_t encoder_raw(void);

// The status bits that came with the most recent good sample, and how many
// samples since boot carried the WEAK FIELD flag. A magnet too far from the die
// sets that flag only through part of the turn when it is also off-centre — so
// the count says more than a single reading does.
uint8_t  encoder_status(void);
uint32_t encoder_weak(void);

// Good samples since boot, so encoder_weak() can be read as a FRACTION rather
// than a bare total. A cumulative count on its own cannot tell "the magnet is
// marginal" from "the magnet was marginal while I was still positioning it",
// and those need opposite reactions.
uint32_t encoder_samples(void);

// Milliseconds since the last sample that carried WEAK FIELD, or UINT32_MAX if
// none ever has. This is the one that answers "is it bad NOW".
uint32_t encoder_weak_age_ms(void);

// ---- gearbox -------------------------------------------------------------
// Motor revolutions per output revolution — the N of an N:1 reducer, 1.0 for a
// magnet mounted straight on the motor shaft.
//
// Everything that compares commanded against measured does it in *output-shaft*
// degrees: the commanded step count is divided by this ratio, and the sensor
// reading is left exactly as it came out of the chip. That direction matters.
// The step counter is an exact integer of pulses emitted, so dividing it costs
// nothing, while multiplying the sensor reading up to motor terms would present
// the sensor's noise and mounting error scaled by the ratio as though it were
// motor motion. Scale the exact quantity, never the measured one.
void   encoder_set_gear(double motor_revs_per_output_rev);
double encoder_gear(void);

// Smallest error worth calling a lost step, in *output* degrees:
// POSITION_TOLERANCE_DEG referred through the reducer, but never finer than
// ENCODER_NOISE_FLOOR_DEG — otherwise a perfect move fails the check on sensor
// noise alone.
double encoder_tolerance_deg(void);

// SPI frames that failed their CRC since boot. A steadily climbing count means
// wiring — a lead, a missing ground, a clock too fast for it — not magnets.
uint32_t encoder_errors(void);

// Two ways the position can be wrong while every read succeeds:
//
//   late   samples that missed their slot. The task was not scheduled in time.
//   jumps  samples where the shaft moved more than a quarter turn since the
//          last one. The unwrap only survives up to half a turn, so this is the
//          margin running out. Once it goes past half, the fold silently picks
//          the wrong direction and the position is fiction from then on, with
//          no error reported anywhere.
//
// Both climbing together means the sampling cannot keep up with the speed.
uint32_t encoder_late(void);
uint32_t encoder_jumps(void);

#endif // ENCODER_H
