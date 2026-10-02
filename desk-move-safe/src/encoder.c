//
// encoder_task — polls the MT6835 and accumulates a continuous shaft position.
// See encoder.h.
//
// The sensor wraps at one revolution, so the task takes the difference between
// consecutive samples and folds it into the shorter direction. Nothing here is
// time-critical: the step pulses come out of an alarm ISR and this task only
// observes.
//
#include "encoder.h"
#include "mt6835.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

// Two milliseconds, which is 250 revolutions per second of trackable speed —
// about three orders of magnitude more than the output shaft of a 17:1 reducer
// will ever see. The period is this short so a hand-turned shaft reads smoothly
// on the console, not because the unwrap needs it.
#define ENCODER_PERIOD_MS   2

static volatile int64_t  s_counts;      // continuous, signed, since the last zero
static volatile uint32_t s_late;        // samples that missed their slot
static volatile uint32_t s_jumps;       // samples too big to trust the unwrap
static volatile uint32_t s_weak;        // samples flagged WEAK FIELD
static volatile uint32_t s_samples;     // good samples, so weak can be a fraction
static volatile TickType_t s_weak_at;   // when the last weak one arrived
static volatile bool     s_weak_ever;
static volatile uint8_t  s_status;      // status bits of the last good sample
static volatile bool     s_ok;
static volatile bool     s_zero_req;
static volatile uint32_t s_zero_gen;

static volatile uint32_t s_last;        // previous raw sample, owned by the task

bool     encoder_available(void) { return s_ok; }
uint32_t encoder_errors(void)    { return mt6835_crc_errors(); }
uint32_t encoder_late(void)      { return s_late; }
uint32_t encoder_jumps(void)     { return s_jumps; }
uint32_t encoder_weak(void)      { return s_weak; }
uint32_t encoder_samples(void)   { return s_samples; }

uint32_t encoder_weak_age_ms(void)
{
    if (!s_weak_ever)
        return UINT32_MAX;
    return (uint32_t)((xTaskGetTickCount() - s_weak_at) * portTICK_PERIOD_MS);
}
uint8_t  encoder_status(void)    { return s_status; }
uint32_t encoder_raw(void)       { return s_last; }
uint32_t encoder_zero_gen(void)  { return s_zero_gen; }

// A 64-bit load is two words on Cortex-M33, and this task can preempt a reader
// between them. The critical section makes the copy whole.
int64_t encoder_counts(void)
{
    taskENTER_CRITICAL();
    int64_t c = s_counts;
    taskEXIT_CRITICAL();
    return c;
}

double encoder_degrees(void) { return (double)encoder_counts() * 360.0 / ENCODER_CPR; }
double encoder_revs(void)    { return (double)encoder_counts() / ENCODER_CPR; }

// Starts from the measured value in board_config.h, so the firmware comes up
// knowing its own mechanics. 'enc gear' overrides it for the session.
static double s_gear = GEAR_RATIO;

void encoder_set_gear(double motor_revs_per_output_rev)
{
    if (motor_revs_per_output_rev > 0.0)
        s_gear = motor_revs_per_output_rev;
}

double encoder_gear(void) { return s_gear; }

double encoder_tolerance_deg(void)
{
    // POSITION_TOLERANCE_DEG is a motor-shaft figure — a bit over half a full
    // step — so referring it to the output shaft divides it too.
    double want = POSITION_TOLERANCE_DEG / s_gear;
    return want > ENCODER_NOISE_FLOOR_DEG ? want : ENCODER_NOISE_FLOOR_DEG;
}

void encoder_zero(void)
{
    // Handled in the task so it cannot race the accumulate below.
    s_zero_req = true;
}

void encoder_task(void *arg)
{
    (void)arg;

    if (!mt6835_init())
        printf("[enc] MT6835 not answering on SPI — CS=GP%d SCK=GP%d MOSI=GP%d"
               " MISO=GP%d, 3V3 and GND. Retrying every second.\n",
               MT6835_PIN_CS, MT6835_PIN_SCK, MT6835_PIN_MOSI, MT6835_PIN_MISO);

    // Seed s_last before accumulating, or the first delta is measured against
    // zero and injects a bogus jump of up to half a revolution.
    //
    // Keep retrying: a sensor plugged in later, or one whose lead was loose at
    // boot, comes back without a reset. SPI has no wedged-bus state to clear —
    // every frame starts fresh at the CS edge — so a retry is just another read.
    uint8_t  st = 0;
    uint32_t raw0;
    for (uint32_t tries = 0; !mt6835_angle(&raw0, &st); tries++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (mt6835_init() && tries)
            printf("[enc] MT6835 back after %lu s\n", (unsigned long)tries + 1);
    }
    s_last   = raw0;
    s_status = st;
    s_ok     = true;
    printf("[enc] MT6835 ok, field %s, raw %lu\n",
           mt6835_status_str(st), (unsigned long)s_last);

    TickType_t next = xTaskGetTickCount();
    for (;;) {
        // vTaskDelayUntil does not delay at all when the wake-up it was given
        // is already in the past. Left alone, `next` then falls a tick further
        // behind on every pass and this loop becomes a busy-wait that starves
        // every lower-priority task — the console included. Resync instead, so
        // a late sample costs samples, not the shell.
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next) > 0) {
            s_late++;
            next = now;
        }
        vTaskDelayUntil(&next, pdMS_TO_TICKS(ENCODER_PERIOD_MS));

        uint32_t raw;
        if (!mt6835_angle(&raw, &st))
            continue;       // CRC failure, counted in the driver. A dropped
                            // sample is not motion, so nothing accumulates.

        s_status = st;
        s_samples++;
        if (st & MT6835_ST_WEAK) {
            s_weak++;
            s_weak_at   = xTaskGetTickCount();
            s_weak_ever = true;
        }

        int32_t d = (int32_t)raw - (int32_t)s_last;
        if (d >  (int32_t)ENCODER_CPR / 2) d -= ENCODER_CPR;    // wrapped backwards
        if (d < -(int32_t)ENCODER_CPR / 2) d += ENCODER_CPR;    // wrapped forwards
        s_last = raw;

        // Everything above rests on the shaft having moved less than half a turn
        // since the last sample. Past that the fold picks the wrong direction
        // and the position is silently, permanently wrong — no bus error, no
        // complaint, just a number that drifts. A quarter turn is the alarm
        // line: still unwrapped correctly, but with no margin left.
        if (d > (int32_t)ENCODER_CPR / 4 || d < -(int32_t)ENCODER_CPR / 4)
            s_jumps++;

        taskENTER_CRITICAL();
        if (s_zero_req) {
            s_zero_req = false;
            s_counts   = 0;
            s_zero_gen++;
        } else {
            s_counts += d;
        }
        taskEXIT_CRITICAL();
    }
}
