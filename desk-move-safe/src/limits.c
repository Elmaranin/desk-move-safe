//
// limits — stored travel range and its enforcement. See limits.h.
//
// One coordinate: u, encoder counts along the range from the expanded end —
// 0 at expanded, span_mag() at collapsed, negative past expanded, beyond the
// span past collapsed. It comes straight from the raw angle, so it is absolute
// and needs no seeding, and it is the only position anything here trusts.
//
// Motor steps are only ever an ESTIMATE of encoder counts, through the gear
// ratio (steps_per_count()): good enough to size a jog, a braking distance or
// a creep increment, never used to decide where the flap is.
//
// Backlash. A gearbox has play: after a reversal the motor turns some number of
// microsteps before the output moves at all. Open-loop moves (jogs, 'mot move')
// that reverse direction are lengthened by it so the output travels the
// distance asked for. Encoder-steered moves do not need it — they stop on the
// encoder — and the creep deliberately leaves it out. Measured by 'lim play'
// or typed in; zero changes nothing.
//
#include "limits.h"
#include "settings.h"
#include "stepper.h"
#include "encoder.h"
#include "board_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// ---- what is stored -------------------------------------------------------
#define F_EXPANDED      1u
#define F_COLLAPSED     2u
#define R               (settings()->lim)
#define MASK            (ENCODER_CPR - 1)

// A mark made this session: the continuous encoder count when it was made, so
// the second mark can tell which way round the flap went between them.
typedef struct {
    bool     session;
    int64_t  counts;
    uint32_t egen;
} mark_t;

static mark_t s_exp_mark, s_col_mark;

static bool              s_free;
static volatile bool     s_bypass;      // the move in flight started outside; guard off

static volatile bool     s_tripped;
static volatile int64_t  s_trip_u;

static int64_t s_target;                // u counts the last targeted move aimed at
static int64_t s_from;                  // ... and where it started
static bool    s_have_target;

bool limits_targeted(void) { return s_have_target; }

#define GUARD_COUNTS    ((int64_t)(LIMIT_GUARD_DEG  / 360.0 * ENCODER_CPR))
#define SETTLE_COUNTS   ((int64_t)(LIMIT_SETTLE_DEG / 360.0 * ENCODER_CPR))

// ---- geometry -------------------------------------------------------------

static double deg(int64_t counts) { return (double)counts * 360.0 / ENCODER_CPR; }
static int64_t counts_of(double degrees) { return (int64_t)llround(degrees / 360.0 * ENCODER_CPR); }

static bool ends_stored(void) { return (R.flags & (F_EXPANDED | F_COLLAPSED)) == (F_EXPANDED | F_COLLAPSED); }
static bool calibrated(void)  { return ends_stored() && R.enc_dir != 0 && R.mot_dir != 0; }

// The arc from expanded to collapsed, the way the flap travels it.
static uint32_t arc(uint32_t from, uint32_t to, int32_t dir)
{
    return (dir > 0 ? to - from : from - to) & MASK;
}
static uint32_t span_mag(void) { return arc(R.expanded_raw, R.collapsed_raw, R.enc_dir); }

// Raw angle -> u. The far side of the circle is folded negative, so "a bit past
// expanded" reads as a small negative number rather than as nearly a full turn.
static int64_t along(uint32_t raw)
{
    uint32_t u   = arc(R.expanded_raw, raw, R.enc_dir);
    uint32_t mag = span_mag();
    if (u > mag + (ENCODER_CPR - mag) / 2)
        return (int64_t)u - (int64_t)ENCODER_CPR;
    return u;
}

static int64_t here(void) { return along(encoder_raw()); }

// ESTIMATES, through the gear ratio. Never a position.
static double  steps_per_count(void) { return encoder_gear() * stepper_steps_per_rev() / (double)ENCODER_CPR; }
static int32_t est_steps(int64_t counts) { return (int32_t)llround((double)counts * steps_per_count()); }
static int64_t est_counts(int32_t steps) { return (int64_t)llround((double)steps / steps_per_count()); }

// "Inside" with the settle tolerance of slack: a shaft parked on an end sits a
// count or two either side of it, and that is on the end, not past it.
static bool inside(int64_t u) { return u >= -SETTLE_COUNTS && u <= (int64_t)span_mag() + SETTLE_COUNTS; }

static void say_where(int64_t u)
{
    if (inside(u))
        printf("shaft at %.2f deg from expanded, range %.2f deg — IN RANGE\n",
               deg(u), deg(span_mag()));
    else if (u < 0)
        printf("shaft is OUT OF RANGE: %.2f deg past expanded\n", -deg(u));
    else
        printf("shaft is OUT OF RANGE: %.2f deg past collapsed\n", deg(u - span_mag()));
}

// ---- learning the motor's direction --------------------------------------
// Every motor move that turns the flap noticeably says which way positive
// steps turn the encoder. The encoder task watches each move from start to
// finish and keeps the answer. Calibration turns it into mot_dir; once
// calibrated, a move that disagrees with the stored mot_dir corrects it.
#define LEARN_MIN_COUNTS   ((int64_t)(0.5 / 360.0 * ENCODER_CPR))
#define LEARN_MIN_STEPS    50
static volatile int8_t  s_rel;          // +1: positive steps make the raw angle count up
static bool             s_mv_on;
static int64_t          s_mv_c0;
static int32_t          s_mv_p0;

static void learn_direction(void)
{
    bool busy = stepper_busy();
    if (busy && !s_mv_on) {
        s_mv_on = true;
        s_mv_c0 = encoder_counts();
        s_mv_p0 = stepper_pos();
    } else if (!busy && s_mv_on) {
        s_mv_on = false;
        int64_t dc = encoder_counts() - s_mv_c0;
        int32_t dp = stepper_pos() - s_mv_p0;
        if ((dc > LEARN_MIN_COUNTS || dc < -LEARN_MIN_COUNTS) &&
            (dp > LEARN_MIN_STEPS || dp < -LEARN_MIN_STEPS))
            s_rel = ((dc > 0) == (dp > 0)) ? 1 : -1;
    }
}

// ---- persistence ----------------------------------------------------------

// Into the settings record; settings.c writes it once the motor is still
// and nothing has changed for SETTINGS_QUIET_MS.
static bool save(void)
{
    settings_mark_dirty();
    return true;
}

void limits_init(void)
{
    // A record with no range in it has never had an approach chosen either:
    // a zeroed record would otherwise read as "approach 0".
    if (!(R.flags & (F_EXPANDED | F_COLLAPSED)))
        R.approach_deg = LIMIT_APPROACH_DEG;

    if (calibrated())
        printf("[boot] limits: expanded raw %lu, collapsed raw %lu, %.2f deg, encoder %s, "
               "motor %s to collapse, play %ld — motion bounded\n",
               (unsigned long)R.expanded_raw, (unsigned long)R.collapsed_raw,
               deg(span_mag()), R.enc_dir > 0 ? "counts up" : "counts down",
               R.mot_dir > 0 ? "forward" : "backward", (long)R.backlash);
    else
        printf("[boot] limits: NOT CALIBRATED — motor LOCKED and the DESK will not move.\n"
               "       'calibrate' calibrates the flap.\n");
}

lim_mode_t limits_mode(void)
{
    if (s_free)       return LIM_FREE;
    if (calibrated()) return LIM_BOUNDED;
    return LIM_LOCKED;
}

bool   limits_calibrated(void) { return calibrated(); }
double limits_range_deg(void)  { return calibrated() ? deg(span_mag()) : 0.0; }

void limits_set_free(bool on)
{
    s_free = on;
    if (on)
        printf("FREE motion: limits are NOT enforced and the guard is OFF until reboot or\n"
               "  'lim free' again. Make sure nothing on the shaft can hit anything.\n");
    else
        printf("free motion off — %s\n", calibrated() ? "range enforced again" : "motion locked (not calibrated)");
}

// ---- the gate ---------------------------------------------------------------

static bool refuse_locked(void)
{
    printf("refused: NOT CALIBRATED — so nothing may move but a jog.\n"
           "  'calibrate' stores the ends; 'mot jog <steps>' jogs in small steps.\n");
    return false;
}

static bool refuse_no_encoder(void)
{
    printf("refused: no encoder — cannot tell where the shaft is in the range. 'enc' for why.\n");
    return false;
}

static bool refuse_outside(int64_t target, int64_t u)
{
    printf("refused: would end %.2f deg %s (range %.2f deg, now %.2f deg from expanded)\n",
           target < 0 ? -deg(target) : deg(target - span_mag()),
           target < 0 ? "past expanded" : "past collapsed", deg(span_mag()), deg(u));
    return false;
}

bool limits_set_approach(double degrees)
{
    if (degrees < 0.0 || degrees > 20.0) {
        printf("approach must be 0..20 deg of the measured shaft\n");
        return false;
    }
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    R.approach_deg = (float)degrees;
    save();
    printf("approach %.1f deg: an encoder-steered move ramps down to stop this far short\n"
           "  of its target, and the creep finishes it. Saved.\n", degrees);
    return true;
}

static bool start_raw(int32_t steps, bool bypass, bool compensate)
{
    if (steps == 0)
        return true;
    // A reversal first takes up the play; add it so the *output* moves `steps`.
    int8_t  dir   = steps > 0 ? 1 : -1;
    int32_t extra = (compensate && R.backlash && dir != stepper_last_dir()) ? dir * R.backlash : 0;
    s_bypass = bypass;
    if (!stepper_move(steps + extra)) {
        printf("busy — ignored\n");
        return false;
    }
    return true;
}

static bool start(int32_t steps, bool bypass) { return start_raw(steps, bypass, true); }

bool limits_move(int32_t steps)
{
    s_have_target = false;
    if (s_free)
        return start(steps, true);
    if (!calibrated())
        return refuse_locked();
    if (!encoder_available())
        return refuse_no_encoder();
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int64_t u      = here();
    int64_t target = u + est_counts(steps) * (R.mot_dir > 0 ? 1 : -1);
    if (!inside(target))
        return refuse_outside(target, u);
    // A move that starts outside — after a hand turn, say — and ends inside is
    // the way back in, and the guard would stop it on its first sample.
    return start(steps, !inside(u));
}

bool limits_jog(int32_t steps)
{
    int32_t cap = (int32_t)stepper_steps_per_rev();
    if (steps == 0)
        return true;
    if (steps > cap || steps < -cap) {
        printf("jog is capped at %ld steps (one motor revolution) per command\n", (long)cap);
        return false;
    }
    s_have_target = false;
    if (s_free || !calibrated())
        return start(steps, true);          // finding the ends: no range to check yet

    if (!encoder_available())
        return refuse_no_encoder();
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int64_t u      = here();
    int64_t target = u + est_counts(steps) * (R.mot_dir > 0 ? 1 : -1);
    if (inside(u)) {
        if (!inside(target))
            return refuse_outside(target, u);
        return start(steps, false);
    }
    // Outside: only towards the range, and never through it to the far side.
    bool home = u < 0 ? (target > u && target <= (int64_t)span_mag())
                      : (target < u && target >= 0);
    if (!home) {
        printf("refused: the shaft is %.2f deg %s and this jog does not bring it back\n",
               u < 0 ? -deg(u) : deg(u - span_mag()), u < 0 ? "past expanded" : "past collapsed");
        return false;
    }
    return start(steps, true);
}

// ---- the encoder-steered run ------------------------------------------------
// The motor runs continuously towards the target; limits_guard(), on every
// encoder sample, works out how far it is still to go and how far the motor
// needs to brake from its present speed, and ramps it down when the two meet —
// lim_approach_deg short of the target. It also stops it hard if the flap is
// not following (a stall) or goes the wrong way (a wrong mot_dir).
#define RUN_PROGRESS_DEG   0.2      // must gain at least this ...
#define RUN_PROGRESS_MS    400      // ... in this long, or it is a stall
#define RUN_AWAY_DEG       1.0      // further from the target than at the start

enum { FAULT_NONE, FAULT_STALL, FAULT_WRONG_WAY };

static volatile bool       s_run;
static volatile int64_t    s_run_target;
static volatile int8_t     s_run_sense;     // +1: u must increase
static volatile int64_t    s_run_best;      // least distance still to go so far
static volatile int64_t    s_run_start;     // distance at the start
static volatile TickType_t s_run_best_at;
static volatile uint8_t    s_fault;         // set by the encoder task, said by the console

static int64_t approach_counts(void) { return counts_of((double)R.approach_deg); }

static void steer(int64_t u)
{
    if (!s_run)
        return;
    if (!stepper_busy()) {                  // ended some other way: 'stop', the guard
        s_run = false;
        return;
    }
    int64_t left = (s_run_target - u) * s_run_sense;

    // Braking distance from the present speed, v^2 / 2a, as encoder counts.
    double v      = (double)stepper_cur_sps();
    double brake  = v * v / (2.0 * (double)stepper_accel());
    int64_t stop_at = est_counts((int32_t)brake) + approach_counts();
    if (left <= stop_at) {
        stepper_stop();                     // ramped: lands about approach short
        s_run = false;
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (left < s_run_best - counts_of(RUN_PROGRESS_DEG)) {
        s_run_best    = left;
        s_run_best_at = now;
    } else if (now - s_run_best_at > pdMS_TO_TICKS(RUN_PROGRESS_MS)) {
        stepper_stop_hard();
        s_run   = false;
        s_fault = left > s_run_start + counts_of(RUN_AWAY_DEG) ? FAULT_WRONG_WAY : FAULT_STALL;
    }
}

bool limits_goto_deg(double deg_from_expanded)
{
    if (!calibrated())
        return refuse_locked();
    if (!encoder_available())
        return refuse_no_encoder();
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int64_t u      = here();
    int64_t target = counts_of(deg_from_expanded);
    // Under 'lim free' the ends are still the axis's landmarks, just not walls.
    if (!s_free && !inside(target))
        return refuse_outside(target, u);

    s_target      = target;
    s_from        = u;
    s_have_target = true;
    int64_t dist  = target - u;
    int64_t mag   = dist < 0 ? -dist : dist;

    if (mag <= approach_counts()) {
        printf("going to %.2f deg from expanded: %.2f deg away — crept under the encoder\n",
               deg(target), deg(dist));
        return true;                        // limits_settle() does all of it
    }
    printf("going to %.2f deg from expanded: %+.2f deg, steered by the encoder, the last\n"
           "  %.1f deg crept\n", deg(target), deg(dist), (double)R.approach_deg);

    s_run_target  = target;
    s_run_sense   = dist > 0 ? 1 : -1;
    s_run_best    = mag;
    s_run_start   = mag;
    s_run_best_at = xTaskGetTickCount();
    s_fault       = FAULT_NONE;
    s_bypass      = !inside(u);
    bool forward  = (s_run_sense > 0) == (R.mot_dir > 0);
    // The motor first, THEN the steering: the encoder task ends a run it finds
    // the motor idle for, and it can run between these two lines.
    if (!stepper_run(forward)) {
        s_have_target = false;
        printf("busy — ignored\n");
        return false;
    }
    s_run = true;
    return true;
}

bool limits_goto_end(bool collapsed)
{
    return limits_goto_deg(collapsed ? deg(span_mag()) : 0.0);
}

bool limits_run(bool forward)
{
    if (s_free) {
        if (!stepper_run(forward)) {
            printf("busy — ignored\n");
            return false;
        }
        return true;
    }
    if (!calibrated())
        return refuse_locked();
    bool collapsed = forward == (R.mot_dir > 0);
    printf("running %s to %s\n", forward ? "forward" : "backward", collapsed ? "collapsed" : "expanded");
    return limits_goto_end(collapsed);
}

// ---- the creep --------------------------------------------------------------
// The encoder finishes the move. Near a point where the load torque changes
// sign the output floats in the play, so where it landed is only known once
// it has stopped; measure, and move by the difference.
#define SETTLE_INC_COARSE   32      // creep increment while more than SETTLE_COARSE_FROM off
#define SETTLE_COARSE_FROM  64      // microsteps — under a degree of shaft on this rig
#define SETTLE_INC          8       // creep increment near the target
#define SETTLE_INC_FINE     2       // after an overshoot
#define SETTLE_PAUSE_MS     30      // for the load to come to rest after each
#define SETTLE_DIVERGE_DEG  2.0     // further off than at the start by this: give up

static int32_t creep_inc(int64_t err, bool overshot)
{
    if (overshot) return SETTLE_INC_FINE;
    int32_t mag = est_steps(err < 0 ? -err : err);
    return mag > SETTLE_COARSE_FROM ? SETTLE_INC_COARSE : SETTLE_INC;
}

int limits_settle(double *err_deg, int32_t *crept)
{
    *err_deg = 0.0;
    *crept   = 0;
    if (!s_have_target || s_free || !calibrated() || !encoder_available() || stepper_busy())
        return -1;

    int64_t err = s_target - here();
    *err_deg = deg(err);
    if (err <= SETTLE_COUNTS && err >= -SETTLE_COUNTS)
        return 0;

    // Creep toward the target. The increment is never lengthened by the play
    // — the whole point is to find out, one increment at a time, when the
    // output starts to follow — and drops to a fine step once the error has
    // changed sign, so an overshoot comes back without ringing. A gap cannot
    // exceed a motor revolution, so that plus the error is the budget.
    int64_t start_err = err < 0 ? -err : err;
    int32_t cap       = est_steps(start_err) * 2 + (int32_t)stepper_steps_per_rev();
    bool    overshot  = false;
    bool    bypass    = !inside(here());
    while (*crept < cap) {
        int32_t inc = creep_inc(err, overshot);
        int32_t dir = (err > 0) == (R.mot_dir > 0) ? 1 : -1;
        if (!start_raw(dir * inc, bypass, false) || !stepper_wait(1000))
            return 1;
        *crept += inc;
        vTaskDelay(pdMS_TO_TICKS(SETTLE_PAUSE_MS));
        int64_t next = s_target - here();
        *err_deg = deg(next);
        if (next <= SETTLE_COUNTS && next >= -SETTLE_COUNTS)
            return 0;
        if ((next < 0 ? -next : next) > start_err + counts_of(SETTLE_DIVERGE_DEG))
            return 1;                           // going the wrong way: stop, say so
        if ((next > 0) != (err > 0))
            overshot = true;                    // crossed the target: come back gently
        err = next;
    }
    return 1;
}

double limits_progress(void)
{
    if (!s_have_target || !calibrated() || !encoder_available())
        return -1.0;
    int64_t total = s_target - s_from;
    if (total == 0)
        return 1.0;
    double p = (double)(here() - s_from) / (double)total;
    return p < 0.0 ? 0.0 : p > 1.0 ? 1.0 : p;
}

#define AT_END_DEG  3.0
bool limits_at_end(bool collapsed)
{
    if (!calibrated() || !encoder_available())
        return false;
    int64_t u    = here();
    int64_t end  = collapsed ? (int64_t)span_mag() : 0;
    int64_t off  = u > end ? u - end : end - u;
    return deg(off) <= AT_END_DEG;
}

int limits_nearer_end(void)
{
    if (!calibrated() || !encoder_available())
        return -1;
    return here() * 2 > (int64_t)span_mag() ? 1 : 0;
}

// ---- calibration ------------------------------------------------------------

// Move one end of an existing range to where the shaft is now. The direction
// of travel is already known, so the other end stays as it is.
static bool remark(bool collapsed)
{
    uint32_t raw = encoder_raw();
    uint32_t e   = collapsed ? R.expanded_raw : raw;
    uint32_t c   = collapsed ? raw : R.collapsed_raw;
    uint32_t mag = arc(e, c, R.enc_dir);
    if (mag == 0) {
        printf("the shaft is on the other end — nothing between them\n");
        return false;
    }
    if ((int64_t)mag >= (int64_t)ENCODER_CPR - 2 * GUARD_COUNTS) {
        printf("that would make the range %.1f deg — it must fit inside one turn\n", deg(mag));
        return false;
    }
    if (collapsed) R.collapsed_raw = raw; else R.expanded_raw = raw;
    save();
    printf("%s moved here: range now %.2f deg. Saved.\n",
           collapsed ? "collapsed" : "expanded", deg(span_mag()));
    return true;
}

bool limits_mark(bool collapsed)
{
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    if (!encoder_available()) {
        printf("no encoder — nothing to store. 'enc' for why.\n");
        return false;
    }
    if (calibrated())
        return remark(collapsed);

    mark_t  *mine  = collapsed ? &s_col_mark : &s_exp_mark;
    mark_t  *other = collapsed ? &s_exp_mark : &s_col_mark;
    uint32_t oflag = collapsed ? F_EXPANDED : F_COLLAPSED;
    const char *me = collapsed ? "collapsed" : "expanded", *them = collapsed ? "expanded" : "collapsed";

    mine->session = true;
    mine->counts  = encoder_counts();
    mine->egen    = encoder_zero_gen();
    uint32_t raw  = encoder_raw();
    if (collapsed) R.collapsed_raw = raw; else R.expanded_raw = raw;
    R.flags |= collapsed ? F_COLLAPSED : F_EXPANDED;
    printf("%s stored: raw %lu (%.3f deg)\n", me, (unsigned long)raw, (double)raw * 360.0 / ENCODER_CPR);

    if (R.flags & oflag) {
        bool ok = false;
        if (!other->session || other->egen != mine->egen) {
            printf("  %s was stored in an earlier session, or the encoder was zeroed since —\n"
                   "  the way round between the ends is not known. Go there and store it again.\n", them);
        } else {
            // Which way round the flap went from expanded to collapsed: the
            // continuous count between the marks says, and it must match one
            // of the two arcs the stored angles make.
            int64_t dc  = collapsed ? mine->counts - other->counts : other->counts - mine->counts;
            int32_t dir = dc > 0 ? 1 : -1;
            int64_t mag = dc > 0 ? dc : -dc;
            int64_t a   = arc(R.expanded_raw, R.collapsed_raw, dir);
            if (mag == 0 || a == 0) {
                printf("  expanded and collapsed are the same place — move between them first\n");
            } else if (mag >= (int64_t)ENCODER_CPR - 2 * GUARD_COUNTS) {
                printf("  the range spans %.1f deg of the measured shaft — it must fit inside one\n"
                       "  turn, with %.1f deg to spare at each end for the guard. Not stored.\n",
                       deg(mag), LIMIT_GUARD_DEG);
            } else if (s_rel == 0) {
                printf("  the motor's direction is not known yet — it is learned while the motor\n"
                       "  turns the flap. 'mot jog' it at least once (more than half a degree).\n");
            } else {
                R.enc_dir = dir;
                R.mot_dir = s_rel * dir;    // steps that make u grow
                ok = true;
            }
        }
        if (!ok) {
            R.flags &= ~oflag;              // keep only this end
            other->session = false;
        }
    }

    save();
    if (calibrated()) {
        printf("CALIBRATED: %.2f deg of travel, the encoder %s and the motor turning %s\n"
               "  to collapse. Saved. 'mot go expanded' / 'mot go collapsed' move between\n"
               "  the ends; 'lim' shows where you are.\n",
               deg(span_mag()), R.enc_dir > 0 ? "counting up" : "counting down",
               R.mot_dir > 0 ? "forward" : "backward");
    } else {
        printf("  saved. Move to the other end and store it.\n");
    }
    return true;
}

bool limits_clear(void)
{
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    // The calibration only — the values the desk lock and the motor gate
    // depend on. Backlash and approach are properties of the gearbox and of
    // taste, still true after the ends move, so they stay.
    R.flags         = 0;
    R.expanded_raw  = 0;
    R.collapsed_raw = 0;
    R.enc_dir       = 0;
    R.mot_dir       = 0;
    R.reserved      = 0;
    settings_mark_dirty();
    s_exp_mark = s_col_mark = (mark_t){ 0 };
    s_have_target = false;
    printf("flap calibration cleared — the motor only jogs and the DESK will not\n"
           "  move until both ends are stored again. Backlash %ld and approach\n"
           "  %.1f deg kept.\n",
           (long)R.backlash, (double)R.approach_deg);
    return true;
}

// ---- backlash ----------------------------------------------------------------

// Creep in one direction, a few microsteps at a time, until the output has
// moved PLAY_MOVE_DEG — far enough to be real motion, not the output being
// dragged a fraction of a degree inside the play. Returns the steps it took,
// or -1 past the cap. Direct to the stepper: this is the one motion that must
// not be compensated. With trace on, prints the output every PLAY_TRACE steps
// so the knee between play and motion can be seen.
//
// The driver is switched OFF after every increment and the shaft left to
// settle unpowered before the encoder is read. With the coils held the rotor
// is pinned and the output stays wherever the mesh dragged it; unpowered, it
// falls to wherever the play lets it, which is the figure that matters.
#define PLAY_INC        8
#define PLAY_SETTLE_MS  150
#define PLAY_KNEE_DEG   0.15    // output motion that counts as "the teeth have met"

// What one pass records, so the play can be read off the pass itself:
//   knee   steps at which the output had first moved PLAY_KNEE_DEG
//   half   steps, and the output's actual motion, at the half-way sample
// The local scale (steps per degree) is the slope from knee to half, and the
// play is the knee less the steps that PLAY_KNEE_DEG of real motion took at
// that slope. Using the pass's own slope, not the gear ratio: a magnet a
// little off-centre reads a different scale at every angle, and over ten or
// fifteen degrees that error would swamp the play.
typedef struct {
    int32_t total;          // steps to move_deg, or -1 past the cap
    int32_t knee;
    int32_t half;
    double  half_deg;
} pass_t;

static pass_t creep(int8_t dir, int32_t cap, double move_deg, bool trace)
{
    pass_t  r = { -1, -1, -1, 0.0 };
    const int64_t thresh = (int64_t)(move_deg / 360.0 * ENCODER_CPR);
    const int64_t knee   = (int64_t)(PLAY_KNEE_DEG / 360.0 * ENCODER_CPR);
    // About twenty trace lines per pass whatever the distance, never denser
    // than every 48 steps.
    int32_t expect = (int32_t)(move_deg / 360.0 * encoder_gear() * stepper_steps_per_rev());
    int32_t every  = (expect / 20 / PLAY_INC) * PLAY_INC;
    if (every < 48) every = 48;
    int64_t c0 = encoder_counts();
    for (int32_t n = 0; n < cap; n += PLAY_INC) {
        if (!stepper_move(dir * PLAY_INC) || !stepper_wait(1000))
            return r;
        vTaskDelay(pdMS_TO_TICKS(15));          // finish the last pulse, let a sample land
        stepper_enable(false);                  // coils off: the output goes where the play lets it
        vTaskDelay(pdMS_TO_TICKS(PLAY_SETTLE_MS));
        int64_t d = encoder_counts() - c0;
        if (d < 0) d = -d;
        if (trace && (n + PLAY_INC) % every == 0)
            printf("    %5ld steps  output %.3f deg\n", (long)(n + PLAY_INC), deg(d));
        if (r.knee < 0 && d >= knee)
            r.knee = n + PLAY_INC;
        if (r.half < 0 && d >= thresh / 2) {
            r.half     = n + PLAY_INC;
            r.half_deg = deg(d);
        }
        if (d >= thresh) {
            r.total = n + PLAY_INC;
            return r;
        }
    }
    return r;
}

// Play from one pass, and the local scale it was read at.
static int32_t pass_play(const pass_t *p, double *scale)
{
    *scale = 0.0;
    if (p->knee < 0 || p->half <= p->knee || p->half_deg <= PLAY_KNEE_DEG)
        return -1;
    *scale = (double)(p->half - p->knee) / (p->half_deg - PLAY_KNEE_DEG);   // steps per degree
    int32_t play = p->knee - (int32_t)llround(PLAY_KNEE_DEG * *scale);
    return play < 0 ? 0 : play;
}

static void say_play(void)
{
    printf("play %ld microsteps = %.2f deg of output (at gear %.4g, %lu steps/rev) — added\n"
           "  to every move that reverses direction. Saved.\n",
           (long)R.backlash,
           (double)R.backlash / stepper_steps_per_rev() / encoder_gear() * 360.0,
           encoder_gear(), (unsigned long)stepper_steps_per_rev());
}

bool limits_set_play(int32_t steps)
{
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    if (steps < 0 || steps > (int32_t)stepper_steps_per_rev()) {
        printf("play must be 0..%lu microsteps\n", (unsigned long)stepper_steps_per_rev());
        return false;
    }
    R.backlash = steps;
    if (!save()) {
        printf("flash write FAILED\n");
        return false;
    }
    say_play();
    return true;
}

bool limits_measure_play(double move_deg)
{
    if (move_deg < 0.2 || move_deg > 45.0) {
        printf("distance must be 0.2..45 deg of the measured shaft\n");
        return false;
    }
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    if (!encoder_available()) {
        printf("no encoder — cannot see the output move. 'enc' for why.\n");
        return false;
    }
    // The play cannot exceed a motor turn; the motion asked for comes on top.
    int32_t cap = (int32_t)stepper_steps_per_rev()
                + (int32_t)(move_deg / 360.0 * encoder_gear() * stepper_steps_per_rev());
    s_bypass = true;

    printf("measuring play: the output moves %.1f deg backward, forward, backward — the\n"
           "  motor more — creeping %d microsteps at a time, coils OFF and %d ms to settle\n"
           "  after each, until the encoder has moved that far\n",
           move_deg, PLAY_INC, PLAY_SETTLE_MS);
    // First take up whatever side of the play we are on, so the two
    // measurements each start from firm contact.
    if (creep(-1, cap, move_deg, false).total < 0) {
        printf("  the output never moved in %ld steps backward — is the magnet on this shaft?\n", (long)cap);
        return false;
    }
    printf("  forward:\n");
    pass_t fwd = creep(+1, cap, move_deg, true);
    if (fwd.total < 0) {
        printf("  the output never moved in %ld steps forward\n", (long)cap);
        return false;
    }
    printf("  backward:\n");
    pass_t bwd = creep(-1, cap, move_deg, true);
    if (bwd.total < 0) {
        printf("  the output never moved in %ld steps backward\n", (long)cap);
        return false;
    }
    double  sf, sb;
    int32_t pf = pass_play(&fwd, &sf), pb = pass_play(&bwd, &sb);
    if (pf < 0 || pb < 0) {
        printf("  could not find the knee in one of the passes — use a longer distance\n");
        return false;
    }
    double gear_scale = encoder_gear() * stepper_steps_per_rev() / 360.0;
    printf("  forward:  output first moved at %ld steps, then %.1f steps/deg -> play %ld\n"
           "  backward: output first moved at %ld steps, then %.1f steps/deg -> play %ld\n",
           (long)fwd.knee, sf, (long)pf, (long)bwd.knee, sb, (long)pb);
    if (sf > gear_scale * 1.04 || sf < gear_scale * 0.96 || sb > gear_scale * 1.04 || sb < gear_scale * 0.96)
        printf("  (gear %.4g predicts %.1f steps/deg — the magnet reads a different scale at\n"
               "   this angle, which is why the play is read off the trace, not the ratio)\n",
               encoder_gear(), gear_scale);
    int32_t big = pf > pb ? pf : pb, small = pf > pb ? pb : pf;
    int32_t play = (pf + pb) / 2;
    if (big > small + small / 4 + 16) {
        // Only one side shows a gap: a load holds the output on a flank in
        // the other direction. The gearbox itself contributes the smaller
        // figure on every reversal; the larger one exists only where the load
        // lets go, and 'mot go' settling is what handles that. Store the smaller.
        play = small;
        printf("  %ld one way, %ld the other: a gap on one side only is the load resting\n"
               "  on a flank, not the gearbox. Storing the smaller, %ld — it applies to EVERY\n"
               "  reversal; the larger is what 'mot go' settles out near the balance point.\n"
               "  'eeprom set lim_backlash %ld' if you want the larger anyway.\n",
               (long)big, (long)small, (long)small, (long)big);
    }
    R.backlash = play;
    stepper_enable(false);
    if (!save()) {
        printf("flash write FAILED\n");
        return false;
    }
    say_play();
    return true;
}

// ---- reporting --------------------------------------------------------------

void limits_report(void)
{
    printf("  stored: flags 0x%lx (%s%s) | expanded_raw %lu | collapsed_raw %lu | enc_dir %+ld"
           " | mot_dir %+ld | play %ld | approach %.1f deg\n",
           (unsigned long)R.flags,
           R.flags & F_EXPANDED ? "expanded" : "-", R.flags & F_COLLAPSED ? "+collapsed" : "",
           (unsigned long)R.expanded_raw, (unsigned long)R.collapsed_raw,
           (long)R.enc_dir, (long)R.mot_dir, (long)R.backlash, (double)R.approach_deg);
    if (!calibrated()) {
        printf("limits: NOT CALIBRATED — expanded %s, collapsed %s. Motor LOCKED%s, desk LOCKED.\n",
               R.flags & F_EXPANDED ? "stored" : "missing",
               R.flags & F_COLLAPSED ? "stored" : "missing",
               s_free ? " (overridden by 'lim free')" : "");
        printf("  motor direction %s\n", s_rel ? "learned" : "not learned yet — jog the flap with the motor");
        printf("  'calibrate' walks through it. By hand: move the flap to one end, 'lim expanded';\n"
               "  to the other, 'lim collapsed'. Both in one session, the encoder not zeroed in\n"
               "  between, the motor jogged at least once. The range must fit in one turn.\n");
        return;
    }
    printf("limits: %.2f deg of travel, encoder %s, motor turns %s to collapse | play %ld%s\n",
           deg(span_mag()), R.enc_dir > 0 ? "counts up" : "counts down",
           R.mot_dir > 0 ? "forward" : "backward", (long)R.backlash,
           s_free ? " — NOT ENFORCED ('lim free')" : "");
    if (!encoder_available()) {
        printf("  no encoder — position in the range unknown\n");
        return;
    }
    printf("  ");
    say_where(here());
    printf("  guard trips %.1f deg past either end\n", LIMIT_GUARD_DEG);
}

// ---- the guard, the steering, the learning -----------------------------------
// Encoder task, every sample. The guard only judges a motor that is moving
// under the range: a shaft turned by hand while disabled is not the motor's
// doing, and a move that started outside is on its way back in.
void limits_guard(uint32_t raw)
{
    learn_direction();
    if (!calibrated())
        return;

    // A motor move that disagrees with the stored direction: trust the move.
    // (Should only happen after the motor wiring is swapped.)
    if (s_rel && R.mot_dir != s_rel * R.enc_dir) {
        R.mot_dir = s_rel * R.enc_dir;
        settings_mark_dirty();
    }

    int64_t u = along(raw);
    steer(u);
    if (s_free || s_bypass || !stepper_busy())
        return;
    if (u < -GUARD_COUNTS || u > (int64_t)span_mag() + GUARD_COUNTS) {
        stepper_stop_hard();
        s_run     = false;
        s_trip_u  = u;
        s_tripped = true;
    }
}

void limits_poll_trip(void)
{
    uint8_t f = s_fault;
    if (f) {
        s_fault = FAULT_NONE;
        if (f == FAULT_STALL)
            printf("\n[lim] STALL: the flap stopped following the motor — hard stop. The creep\n"
                   "  finishes slowly; check the driver's supply. 'lim' to see where it is.\n> ");
        else
            printf("\n[lim] WRONG WAY: the flap moved away from the target — hard stop.\n"
                   "  Was the motor wiring changed? 'calibrate' again.\n> ");
    }
    if (!s_tripped)
        return;
    s_tripped = false;
    int64_t u = s_trip_u;
    printf("\nLIMIT GUARD: the shaft went %.2f deg %s — HARD STOP. 'lim' to see where it is.\n> ",
           u < 0 ? -deg(u) : deg(u - span_mag()), u < 0 ? "past expanded" : "past collapsed");
}
