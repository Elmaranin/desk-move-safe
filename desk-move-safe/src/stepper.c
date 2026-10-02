//
// stepper — step-pulse generation and the Austin/AVR446 acceleration ramp.
// See stepper.h for the algorithm and the API contract.
//
// Structure: one hardware alarm reschedules itself. Each firing emits a STEP
// pulse and then computes the interval until the next one. Nothing about the
// ramp lives in a task, so a busy task or a long printf can't stutter the
// motor — the ISR keeps the pulse train going on its own.
//
#include "stepper.h"
#include "tmc2209.h"

#include "pico/stdlib.h"
#include "hardware/timer.h"

#include "FreeRTOS.h"
#include "task.h"

#include <math.h>

// ---- hardware limits -----------------------------------------------------
#define STEP_PULSE_US   2u          // STEP high time; the TMC2209 needs >=100 ns
#define DIR_SETUP_US    2u          // DIR settle before the first edge (>=20 ns)
#define MIN_STEP_US     25u         // 40 kHz ceiling — past here the ISR can't keep up
#define MAX_STEP_US     100000u     // 10 steps/s floor; slower is a stall, not a move

// ---- profile limits ------------------------------------------------------
#define MIN_ACCEL       10u
#define MAX_ACCEL       10000000u   // high enough that d0 clamps to d_min: a hard start

typedef enum { M_IDLE, M_MOVE, M_RUN, M_STOP } mode_t;

// Owned by the ISR once a move starts; the API only touches them inside a
// critical section (the alarm IRQ runs below configMAX_SYSCALL_INTERRUPT_
// PRIORITY, so taskENTER_CRITICAL genuinely masks it).
static volatile mode_t   s_mode = M_IDLE;
static volatile int32_t  s_pos;             // signed microsteps since the last zero
static volatile uint32_t s_pos_gen;         // see stepper_pos_gen()
static volatile int32_t  s_n;               // Austin's counter; < 0 while decelerating
static volatile uint32_t s_d;               // current step interval, us
static volatile int32_t  s_rest;            // carried remainder of the recurrence
static volatile uint32_t s_remaining;       // steps left in an M_MOVE
static volatile int8_t   s_dir = 1;
static volatile TaskHandle_t s_waiter;

// Derived from the profile, recomputed at the start of each move.
static uint32_t s_d0;                       // first-step interval
static uint32_t s_d_min;                    // interval at the speed limit
static uint32_t s_sps   = DEFAULT_SPEED_SPS;
static uint32_t s_accel = DEFAULT_ACCEL_SPS2;

static bool s_enabled;
static int  s_alarm = -1;
static absolute_time_t s_next;              // the target we last armed

// ---- profile -------------------------------------------------------------

// d0 is the exact interval of the first step of a ramp: the time to cover one
// step under constant acceleration from rest, times Austin's 0.676 correction
// for the error the discrete recurrence accumulates early on.
static void recalc(void)
{
    double d0 = 0.676 * 1e6 * sqrt(2.0 / (double)s_accel);
    s_d0 = (d0 > (double)MAX_STEP_US) ? MAX_STEP_US : (uint32_t)d0;

    s_d_min = 1000000u / s_sps;
    if (s_d_min < MIN_STEP_US)
        s_d_min = MIN_STEP_US;

    // A ramp that would start slower than it ends is not a ramp; a d0 at or
    // below d_min means "jump straight to full speed" (see the demo's no-ramp
    // case, which is exactly how you provoke a stall).
    if (s_d0 < s_d_min)
        s_d0 = s_d_min;
}

// ---- alarm ---------------------------------------------------------------

// Schedule the next edge relative to the target we last armed, not to "now", so
// IRQ latency doesn't accumulate into audible jitter.
static void arm(uint32_t delay_us)
{
    s_next = delayed_by_us(s_next, delay_us);
    if (hardware_alarm_set_target((uint)s_alarm, s_next)) {
        // Already in the past — we fell behind (usually MIN_STEP_US is too
        // aggressive for the current speed). Give up on catching up silently.
        s_next = make_timeout_time_us(MIN_STEP_US);
        hardware_alarm_set_target((uint)s_alarm, s_next);
    }
}

// d[n] = d[n-1] - (2*d[n-1] + rest) / (4n + 1), carrying the division remainder
// between steps.
//
// The remainder is not an optimisation, it is the whole thing. Without it the
// quotient truncates to zero as soon as 2*d falls below (4n+1) — d stops
// changing, the ramp freezes mid-climb, and the motor tops out at roughly
// (1e6 * accel)^(1/3) steps/s no matter what speed limit was asked for. At
// accel = 8000 that ceiling is about 2000 steps/s, which looks exactly like a
// motor that has run out of torque.
static uint32_t recur(uint32_t d, int32_t n)
{
    int32_t den = 4 * n + 1;                // negative while decelerating
    int32_t num = 2 * (int32_t)d + s_rest;
    s_rest = num % den;
    return (uint32_t)((int32_t)d - num / den);
}

// One recurrence step. Returns false when a deceleration has reached zero.
static bool ramp_next(void)
{
    int32_t  n = s_n;
    uint32_t d = s_d;

    if (n < 0) {
        // Decelerating: 4n+1 is negative, so subtracting the term grows d.
        d = recur(d, n);
        if (++n == 0) {
            s_n = 0;
            return false;
        }
    } else if (d <= s_d_min) {
        // Cruising. n is deliberately frozen here: it holds the height of the
        // ramp we climbed, which is what the deceleration will retrace.
        d = s_d_min;
    } else {
        n++;
        // The recurrence is a poor approximation at n == 1 (it gives 0.6*d0
        // where the true value is 0.4142*d0); AVR446 substitutes 0.4056*d0,
        // which lands the *rest* of the series on the exact curve.
        if (n == 1) {
            d = (uint32_t)((uint64_t)s_d0 * 4056u / 10000u);
            s_rest = 0;
        } else {
            d = recur(d, n);
        }
        if (d < s_d_min)
            d = s_d_min;
    }

    if (d < MIN_STEP_US) d = MIN_STEP_US;
    if (d > MAX_STEP_US) d = MAX_STEP_US;
    s_n = n;
    s_d = d;
    return true;
}

static void finish_from_isr(void)
{
    s_mode = M_IDLE;
    s_n    = 0;
    s_d    = s_d0;
    s_rest = 0;

    TaskHandle_t w = s_waiter;
    if (w) {
        BaseType_t woken = pdFALSE;
        s_waiter = NULL;
        vTaskNotifyGiveFromISR(w, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void step_isr(uint alarm)
{
    (void)alarm;

    // Pulse first, arithmetic after: the edge timing is what the motor sees.
    gpio_put(PIN_STEP, 1);
    busy_wait_us_32(STEP_PULSE_US);
    gpio_put(PIN_STEP, 0);

    s_pos += s_dir;

    if (s_mode == M_MOVE) {
        if (--s_remaining == 0) {
            finish_from_isr();
            return;
        }
        // Begin the descent once the steps left match the ramp height. This is
        // what makes a short move come out triangular instead of overshooting:
        // it can flip to decel long before the speed limit is reached.
        if (s_n > 0 && s_remaining <= (uint32_t)s_n) {
            // One *more* than the steps left, because ramp_next() is about to
            // run for this step as well. The move has to end on the step
            // counter reaching zero, not on the ramp running out — size it
            // exactly and every cruising move stops one step short of target.
            s_n    = -(int32_t)(s_remaining + 1);
            s_rest = 0;         // phase change: don't carry accel's remainder into decel
        }
    }

    if (!ramp_next()) {
        finish_from_isr();
        return;
    }
    arm(s_d);
}

// ---- lifecycle -----------------------------------------------------------

void stepper_init(void)
{
#if TMC_USE_EN_PIN
    // EN first and high, before anything else can energise the coils.
    gpio_init(PIN_EN);
    gpio_put(PIN_EN, 1);            // active low -> 1 is disabled
    gpio_set_dir(PIN_EN, GPIO_OUT);
#endif
    s_enabled = false;

    // STEP low and an output before the driver is configured. Motion needs
    // edges and nothing else, so an input pin here is a motor that cannot
    // move however the driver is set up — which is the whole reason dropping
    // EN costs holding current rather than safety.
    gpio_init(PIN_STEP);
    gpio_put(PIN_STEP, 0);
    gpio_set_dir(PIN_STEP, GPIO_OUT);

#if TMC_USE_DIR_PIN
    gpio_init(PIN_DIR);
    gpio_put(PIN_DIR, 0);
    gpio_set_dir(PIN_DIR, GPIO_OUT);
#endif

    s_alarm = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback((uint)s_alarm, step_isr);

    recalc();
    s_d = s_d0;
}

// Two things have to agree before current reaches the coils, and they fail
// differently on purpose. The EN pin is a wire: it holds the coils off through
// reset and through a firmware that never runs. The chopper is a register: it
// is how the driver is told, over the same link that carries the current
// setting, and if that link is down there is no point energising anything at
// an unknown current. So the pin goes last on the way up and first on the way
// down, and a driver that cannot be reached is a motor that will not move.
static void en_pin(bool energised)
{
#if TMC_USE_EN_PIN
    gpio_put(PIN_EN, energised ? 0 : 1);    // active low
#else
    (void)energised;
#endif
}

bool stepper_enable(bool on)
{
    if (!on) {
        en_pin(false);
        bool ok = tmc2209_set_chopper(false);
        s_enabled = false;
        // With an EN pin the coils are already off and the register write was
        // only tidiness. Without one the write IS the release, so a failed one
        // leaves the motor energised and the caller has to hear about it.
        return TMC_USE_EN_PIN ? true : ok;
    }

    if (!tmc2209_set_chopper(true)) {
        en_pin(false);
        s_enabled = false;
        return false;
    }
    en_pin(true);
    s_enabled = true;
    return true;
}

bool stepper_enabled(void) { return s_enabled; }

// ---- profile setters -----------------------------------------------------

void stepper_set_speed(uint32_t steps_per_s)
{
    if (steps_per_s < 1)                  steps_per_s = 1;
    if (steps_per_s > 1000000u / MIN_STEP_US) steps_per_s = 1000000u / MIN_STEP_US;
    s_sps = steps_per_s;
    recalc();
}

void stepper_set_accel(uint32_t steps_per_s2)
{
    if (steps_per_s2 < MIN_ACCEL) steps_per_s2 = MIN_ACCEL;
    if (steps_per_s2 > MAX_ACCEL) steps_per_s2 = MAX_ACCEL;
    s_accel = steps_per_s2;
    recalc();
}

uint32_t stepper_speed(void) { return s_sps; }
uint32_t stepper_accel(void) { return s_accel; }

// ---- motion --------------------------------------------------------------

// Point the motor, OUTSIDE any critical section.
//
// The pin version is instantaneous; the register version is a datagram and a
// verify, about 1.4 ms at 115200. Neither may happen inside taskENTER_CRITICAL
// — the bus task has a hard deadline and 1.4 ms of masked interrupts is more
// than a desk frame — so direction is applied first and launch() only records
// it. Nothing steps in between, because nothing steps until the alarm is armed.
//
// Reversing a moving motor is the one thing that must not happen here, which
// is why both callers check stepper_busy() first. The check and the write are
// not atomic, but only one thing in this firmware commands motion at a time.
static bool apply_dir(int8_t dir)
{
#if TMC_USE_DIR_PIN
    gpio_put(PIN_DIR, dir > 0 ? 1 : 0);
    busy_wait_us_32(DIR_SETUP_US);      // DIR is sampled on the STEP edge
    return true;
#else
    return tmc2209_set_shaft(dir < 0);
#endif
}

// Caller must hold the critical section, and must have called apply_dir().
static void launch(int8_t dir)
{
    s_dir = dir;

    s_n    = 0;
    s_d    = s_d0;
    s_rest = 0;
    s_next = get_absolute_time();
    arm(s_d0);
}

bool stepper_move(int32_t steps)
{
    if (steps == 0)
        return true;

    recalc();
    if (stepper_busy())
        return false;                   // before the direction write, not after
    if (!stepper_enable(true))
        return false;

    int8_t dir = steps > 0 ? 1 : -1;
    if (!apply_dir(dir))
        return false;

    taskENTER_CRITICAL();
    bool ok = (s_mode == M_IDLE);
    if (ok) {
        s_remaining = (uint32_t)(steps < 0 ? -(int64_t)steps : (int64_t)steps);
        s_mode      = M_MOVE;
        launch(dir);
    }
    taskEXIT_CRITICAL();
    return ok;
}

bool stepper_run(bool forward)
{
    recalc();
    if (stepper_busy())
        return false;
    if (!stepper_enable(true))
        return false;

    int8_t dir = forward ? 1 : -1;
    if (!apply_dir(dir))
        return false;

    taskENTER_CRITICAL();
    bool ok = (s_mode == M_IDLE);
    if (ok) {
        s_mode = M_RUN;
        launch(dir);
    }
    taskEXIT_CRITICAL();
    return ok;
}

void stepper_stop(void)
{
    bool halt = false;

    taskENTER_CRITICAL();
    if (s_mode == M_MOVE || s_mode == M_RUN) {
        if (s_n > 0) {
            s_n    = -s_n;              // retrace the ramp we climbed
            s_rest = 0;
            s_mode = M_STOP;
        } else {
            halt = true;                // still on the first step; nothing to unwind
        }
    }
    taskEXIT_CRITICAL();

    if (halt)
        stepper_stop_hard();
}

void stepper_stop_hard(void)
{
    taskENTER_CRITICAL();
    if (s_alarm >= 0)
        hardware_alarm_cancel((uint)s_alarm);
    s_mode = M_IDLE;
    s_n    = 0;
    s_d    = s_d0;
    s_rest = 0;
    s_pos_gen++;                        // the count no longer says where the shaft is
    TaskHandle_t w = s_waiter;
    s_waiter = NULL;
    taskEXIT_CRITICAL();

    if (w)
        xTaskNotifyGive(w);             // outside the critical section on purpose
}

bool stepper_wait(uint32_t timeout_ms)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();

    ulTaskNotifyTake(pdTRUE, 0);        // drop a stale give from an earlier move

    taskENTER_CRITICAL();
    bool idle = (s_mode == M_IDLE);
    if (!idle)
        s_waiter = me;
    taskEXIT_CRITICAL();

    if (idle)
        return true;

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return s_mode == M_IDLE;
}

// Read live rather than cached: microstepping is a register the console can
// change between one move and the next, and a stale figure here turns every
// "revolutions" number in the firmware into a quiet lie.
uint32_t stepper_steps_per_rev(void)
{
    return (uint32_t)FULL_STEPS_REV * tmc2209_microsteps();
}

bool     stepper_busy(void) { return s_mode != M_IDLE; }
int32_t  stepper_pos(void)  { return s_pos; }
// Zeroing mid-move used to be silently ignored, which made 'z' a lie exactly
// when it mattered: the encoder counter zeroed and this one didn't, so the two
// were left measuring from different origins and every error after it was
// nonsense. The move itself doesn't care — it counts down s_remaining, not
// s_pos — so the only real requirement is that the ISR isn't halfway through
// its own increment while we write.
void stepper_zero(void)
{
    taskENTER_CRITICAL();
    s_pos = 0;
    s_pos_gen++;
    taskEXIT_CRITICAL();
}

uint32_t stepper_pos_gen(void) { return s_pos_gen; }
int8_t   stepper_last_dir(void) { return s_dir; }

uint32_t stepper_cur_sps(void)
{
    if (s_mode == M_IDLE)
        return 0;
    uint32_t d = s_d;
    return d ? 1000000u / d : 0;
}
