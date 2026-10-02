//
// limits — stored travel range and its enforcement. See limits.h.
//
// Coordinates. Everything here is on an "axis" measured from min:
//
//   counts   u   0 at min, |enc_span| at max, in sensor counts of the measured
//                shaft; negative below min, past |enc_span| beyond max. Comes
//                straight from the raw angle, so it is absolute.
//   steps    p   0 at min, span_steps at max, in microsteps; span_steps carries
//                the sign of the motor direction that goes from min to max.
//                Comes from the step counter, so it is exact between moves and
//                wrong after lost steps or a hand-turned shaft.
//
// The step counter is the working coordinate — it is what a move is made of —
// and the angle is the truth it is checked against: before every move the two
// are compared, and if they disagree by more than LIMIT_RESYNC_DEG the origin is
// re-seeded from the angle and the operator is told.
//
// Backlash. A gearbox has play: after a reversal the motor turns some number of
// microsteps before the output moves at all. The step axis is kept in *output*
// terms by treating the motor as resting on one side of that play or the other
// — contact() is the offset between motor and output for the direction it last
// moved — and every move that reverses direction is lengthened by the play so
// the output travels the distance asked for. The amount is measured by
// 'lim play' (creep across the reversal until the encoder moves) or typed in,
// and stored with the ends. Left at zero it changes nothing.
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
// The flap section of the one settings record — the rig's record field for
// field, minus its magic: settings.c versions the whole record instead.
#define F_EXPANDED       1u
#define F_COLLAPSED       2u
#define R           (settings()->lim)

// A mark made this session carries what a stored one cannot: the counters at
// the moment it was made, so the second mark can measure the span exactly.
typedef struct {
    bool     session;
    int64_t  counts;
    int32_t  pos;
    int8_t   dir;           // which side of the play the motor sat on
    uint32_t sgen, egen;
} mark_t;

static mark_t s_min_mark, s_max_mark;

static bool     s_free;
static bool     s_synced;
static int32_t  s_origin;       // stepper_pos() at which the axis reads 0
static uint32_t s_origin_gen;
static bool     s_bypass;       // the move in flight started outside; guard off

static volatile bool    s_tripped;
static volatile int64_t s_trip_u;

static int32_t s_target;        // axis steps the last targeted move aimed at
static bool    s_have_target;

bool limits_targeted(void) { return s_have_target; }

#define GUARD_COUNTS    ((int64_t)(LIMIT_GUARD_DEG  / 360.0 * ENCODER_CPR))
#define RESYNC_COUNTS   ((int64_t)(LIMIT_RESYNC_DEG / 360.0 * ENCODER_CPR))

// ---- geometry -------------------------------------------------------------

static bool calibrated(void) { return (R.flags & (F_EXPANDED | F_COLLAPSED)) == (F_EXPANDED | F_COLLAPSED); }
static uint32_t span_mag(void) { return (uint32_t)(R.enc_span < 0 ? -R.enc_span : R.enc_span); }
static double deg(int64_t counts) { return (double)counts * 360.0 / ENCODER_CPR; }

// Raw angle -> counts along the axis. The subtraction is done in whichever
// direction the encoder counts from min to max, masked to one turn, and the
// far side of the circle is folded negative so "a bit past expanded" reads as a
// small negative number rather than as nearly a full turn past max.
static int64_t along(uint32_t raw)
{
    uint32_t u   = (R.enc_span > 0 ? raw - R.expanded_raw : R.expanded_raw - raw) & (ENCODER_CPR - 1);
    uint32_t mag = span_mag();
    if (u > mag + (ENCODER_CPR - mag) / 2)
        return (int64_t)u - (int64_t)ENCODER_CPR;
    return u;
}

static int32_t to_steps(int64_t u)
{
    return (int32_t)llround((double)u * R.span_steps / (double)span_mag());
}

// Motor position minus output position, for the side of the play the motor
// last pushed on. Moving forward it leads the output by the play; moving
// backward the two coincide. Which side is "forward" is arbitrary — what
// matters is that it is used consistently.
static int32_t contact(int8_t dir) { return dir > 0 ? R.backlash : 0; }

static int32_t lo_steps(void) { return R.span_steps < 0 ? R.span_steps : 0; }
static int32_t hi_steps(void) { return R.span_steps > 0 ? R.span_steps : 0; }

// "Inside" with the settle tolerance of slack: a shaft parked on an end sits a
// count or two either side of it, and that is on the end, not past it.
static bool inside_counts(int64_t u)
{
    const int64_t slack = (int64_t)(LIMIT_SETTLE_DEG / 360.0 * ENCODER_CPR);
    return u >= -slack && u <= (int64_t)span_mag() + slack;
}
static bool inside_steps(int32_t p)  { return p >= lo_steps() && p <= hi_steps(); }

// How far outside, in steps; 0 inside.
static int32_t outside_by(int32_t p)
{
    if (p < lo_steps()) return lo_steps() - p;
    if (p > hi_steps()) return p - hi_steps();
    return 0;
}

// Which end sits at the low step count depends on which way max is from min.
static const char *past_end(bool at_lo)
{
    bool min_at_lo = R.span_steps > 0;
    return (at_lo == min_at_lo) ? "past expanded" : "past collapsed";
}

// Output degrees for a step figure, via the stored span — so it needs no gear
// ratio and is right even if 'gear' is wrong.
static double steps_deg(int32_t steps)
{
    return (double)steps * deg(span_mag()) / (double)abs(R.span_steps);
}

static int32_t axis_pos(void) { return stepper_pos() - contact(stepper_last_dir()) - s_origin; }

static void say_where(int64_t u)
{
    if (inside_counts(u))
        printf("shaft at %.2f deg from expanded (%ld steps), range %.2f deg — IN RANGE\n",
               deg(u), (long)to_steps(u), deg(span_mag()));
    else if (u < 0)
        printf("shaft is OUT OF RANGE: %.2f deg past expanded\n", -deg(u));
    else
        printf("shaft is OUT OF RANGE: %.2f deg past collapsed\n", deg(u - span_mag()));
}

// ---- persistence ----------------------------------------------------------

// Into the settings record; settings.c writes it once the motor is still
// and nothing has changed for SETTINGS_QUIET_MS.
static bool save(void)
{
    R.steps_per_rev = stepper_steps_per_rev();
    settings_mark_dirty();
    return true;
}

void limits_init(void)
{
    // A record with no range in it has never had an approach chosen either:
    // a zeroed record would otherwise read as "approach 0".
    if (!(R.flags & (F_EXPANDED | F_COLLAPSED)))
        R.approach_deg = LIMIT_APPROACH_DEG;

    // span_steps was counted at one microstep setting; if the build or the
    // MS pins changed, scale it — it is a ratio of two exact integers.
    uint32_t spr = stepper_steps_per_rev();
    if (calibrated() && R.steps_per_rev && R.steps_per_rev != spr) {
        printf("[boot] limits: span rescaled from %lu to %lu steps/rev\n",
               (unsigned long)R.steps_per_rev, (unsigned long)spr);
        R.span_steps    = (int32_t)llround((double)R.span_steps * spr / R.steps_per_rev);
        R.steps_per_rev = spr;
        settings_mark_dirty();
    }
    if (calibrated())
        printf("[boot] limits: expanded raw %lu, collapsed raw %lu, %.2f deg = %ld steps, play %ld — motion bounded\n",
               (unsigned long)R.expanded_raw, (unsigned long)R.collapsed_raw,
               deg(span_mag()), (long)R.span_steps, (long)R.backlash);
    else
        printf("[boot] limits: NOT STORED (%s%s) — motor LOCKED and the DESK will not move.\n"
               "       'calibrate' calibrates the flap.\n",
               R.flags & F_EXPANDED ? "expanded stored" : "expanded missing",
               R.flags & F_COLLAPSED ? ", collapsed stored" : ", collapsed missing");
}

lim_mode_t limits_mode(void)
{
    if (s_free)       return LIM_FREE;
    if (calibrated()) return LIM_BOUNDED;
    return LIM_LOCKED;
}

bool limits_calibrated(void) { return calibrated(); }
int32_t limits_span_steps(void) { return R.span_steps; }

void limits_set_free(bool on)
{
    s_free = on;
    if (on)
        printf("FREE motion: limits are NOT enforced and the guard is OFF until reboot or\n"
               "  'lim free' again. Make sure nothing on the shaft can hit anything.\n");
    else
        printf("free motion off — %s\n", calibrated() ? "range enforced again" : "motion locked (not calibrated)");
}

// ---- sync ----------------------------------------------------------------

static bool sync_now(bool verbose)
{
    if (!calibrated() || !encoder_available())
        return false;
    int64_t u = along(encoder_raw());
    s_origin     = stepper_pos() - contact(stepper_last_dir()) - to_steps(u);
    s_origin_gen = stepper_pos_gen();
    s_synced     = true;
    if (verbose)
        say_where(u);
    return true;
}

bool limits_sync(bool verbose)
{
    if (!calibrated()) {
        if (verbose) printf("not calibrated — nothing to sync to\n");
        return false;
    }
    if (!encoder_available()) {
        if (verbose) printf("no encoder — cannot place the shaft in the range. 'enc' for why.\n");
        return false;
    }
    return sync_now(verbose);
}

// Before a move: make sure the step axis agrees with the angle. Returns the
// axis position in counts (absolute) and leaves axis_pos() consistent with it.
static int64_t reconcile(void)
{
    int64_t u = along(encoder_raw());
    if (!s_synced || s_origin_gen != stepper_pos_gen()) {
        sync_now(false);
        return u;
    }
    int32_t p_step = axis_pos();
    int32_t p_enc  = to_steps(u);
    int64_t diff_c = (int64_t)llround((double)(p_step - p_enc) * (double)span_mag() / (double)abs(R.span_steps));
    if (diff_c > RESYNC_COUNTS || diff_c < -RESYNC_COUNTS) {
        printf("position re-seeded from the encoder: the step counter was %+.2f deg off\n"
               "  (lost steps, or the shaft was moved by hand)\n", deg(diff_c));
        sync_now(false);
    }
    return u;
}

// ---- the gate ---------------------------------------------------------------

static bool refuse_outside(int32_t target, int32_t p)
{
    int32_t over = outside_by(target);
    printf("refused: would end %.2f deg %s (range %.2f deg, now %.2f deg from expanded)\n",
           steps_deg(over), past_end(target < lo_steps()), deg(span_mag()),
           steps_deg(p - lo_steps()));
    return false;
}

static int32_t approach_steps(void)
{
    return (int32_t)llround((double)R.approach_deg / deg(span_mag()) * (double)abs(R.span_steps));
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
    if (!save()) {
        printf("flash write FAILED\n");
        return false;
    }
    if (degrees == 0.0)
        printf("approach 0: targeted moves run the whole distance by step count, then the\n"
               "  encoder checks and creeps back if off. A gap the load holds open on one\n"
               "  side WILL carry the shaft past an end by that gap first. Saved.\n");
    else
        printf("approach %.1f deg: the last %.1f deg of every targeted move is crept under\n"
               "  the encoder. Saved.\n", degrees, degrees);
    return true;
}

static bool refuse_locked(void)
{
    printf("refused: NOT CALIBRATED — no ends stored, so nothing may move.\n"
           "  'mot jog <steps>' jogs in small steps; 'lim expanded' / 'lim collapsed' store the ends.\n");
    return false;
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
    if (!encoder_available()) {
        printf("refused: no encoder — cannot tell where the shaft is in the range. 'enc' for why.\n");
        return false;
    }
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int64_t u      = reconcile();
    int32_t p      = axis_pos();
    int32_t target = p + steps;
    if (!inside_steps(target))
        return refuse_outside(target, p);
    // A move that starts outside — after a hand turn, say — and ends inside is
    // the way back in, and the guard would stop it on its first sample.
    return start(steps, !inside_counts(u));
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
    if (!encoder_available()) {
        printf("refused: no encoder — cannot tell where the shaft is in the range. 'enc' for why.\n");
        return false;
    }
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int32_t target = forward ? hi_steps() : lo_steps();
    printf("running %s to %s\n", forward ? "forward" : "backward",
           target == R.span_steps ? "collapsed" : "expanded");
    return limits_goto(target);
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
    if (s_free || !calibrated())
        return start(steps, true);          // finding the ends: no range to check yet

    if (!encoder_available()) {
        printf("refused: no encoder — cannot tell where the shaft is in the range. 'enc' for why.\n");
        return false;
    }
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int64_t u = reconcile();
    int32_t p = axis_pos();
    if (inside_counts(u))
        return limits_goto(p + steps);          // targeted, like any bounded move

    // Outside: only towards the range, and never through it to the far side.
    bool    below = p < lo_steps();
    int32_t t     = p + steps;
    bool    home  = below ? (t > p && t <= hi_steps()) : (t < p && t >= lo_steps());
    if (!home) {
        printf("refused: the shaft is %.2f deg %s and this jog does not bring it back —\n"
               "  jog %s, and not past the far end\n",
               steps_deg(outside_by(p)), past_end(below), below ? "forward (+)" : "backward (-)");
        return false;
    }
    return start(steps, true);
}

bool limits_goto(int32_t axis_steps)
{
    if (!calibrated())
        return refuse_locked();
    if (!encoder_available()) {
        printf("refused: no encoder — cannot tell where the shaft is in the range. 'enc' for why.\n");
        return false;
    }
    if (stepper_busy()) {
        printf("busy — ignored\n");
        return false;
    }
    int64_t u = reconcile();
    int32_t p = axis_pos();
    // Under 'lim free' the ends are still the axis's landmarks, just not walls.
    if (!s_free && !inside_steps(axis_steps))
        return refuse_outside(axis_steps, p);

    int32_t steps  = axis_steps - p;
    int32_t margin = approach_steps();
    s_target       = axis_steps;
    s_have_target  = true;

    // Open-loop only to `margin` short of the target; limits_settle() creeps
    // the rest with the encoder, so the shaft arrives from inside and the
    // load cannot carry it past.
    int32_t run = 0;
    if (steps > margin)       run = steps - margin;
    else if (steps < -margin) run = steps + margin;

    if (margin)
        printf("going to %ld (%.2f deg from expanded): %+ld steps, the last %.1f deg under the encoder\n",
               (long)axis_steps, steps_deg(axis_steps), (long)steps,
               run ? (double)R.approach_deg : steps_deg(steps < 0 ? -steps : steps));
    else
        printf("going to %ld (%.2f deg from expanded): %+ld steps by count, then checked\n",
               (long)axis_steps, steps_deg(axis_steps), (long)steps);
    if (run && !start(run, !inside_counts(u))) {
        s_have_target = false;
        return false;
    }
    return true;
}

// The encoder finishes the move. Near a point where the load torque changes
// sign the output floats in the play, so where it landed is only known once
// it has stopped; measure, and move by the difference. The origin is
// re-seeded first so the correction is made in the shaft's own terms.
#define SETTLE_INC_COARSE   32      // creep increment while more than SETTLE_COARSE_FROM off
#define SETTLE_COARSE_FROM  64      // microsteps — under a degree of shaft on this rig
#define SETTLE_INC          8       // creep increment near the target
#define SETTLE_INC_FINE     2       // after an overshoot
#define SETTLE_PAUSE_MS     30      // for the load to come to rest after each

static int32_t creep_inc(int32_t err, bool overshot)
{
    if (overshot) return SETTLE_INC_FINE;
    int32_t mag = err < 0 ? -err : err;
    return mag > SETTLE_COARSE_FROM ? SETTLE_INC_COARSE : SETTLE_INC;
}

static int32_t settle_error(int64_t *u_out)
{
    int64_t u = along(encoder_raw());
    sync_now(false);
    *u_out = u;
    return s_target - to_steps(u);
}

int limits_settle(double *err_deg, int32_t *crept)
{
    *err_deg = 0.0;
    *crept   = 0;
    if (!s_have_target || s_free || !calibrated() || !encoder_available() || stepper_busy())
        return -1;
    int32_t tol = (int32_t)llround(LIMIT_SETTLE_DEG / deg(span_mag()) * (double)abs(R.span_steps));
    if (tol < 1) tol = 1;

    int64_t u;
    int32_t err = settle_error(&u);
    *err_deg = steps_deg(err);
    if (err <= tol && err >= -tol)
        return 0;

    // Creep toward the target. The increment is never lengthened by the play
    // — the whole point is to find out, one increment at a time, when the
    // output starts to follow — and drops to a fine step once the error has
    // changed sign, so an overshoot comes back without ringing. A gap cannot
    // exceed a motor revolution, so that plus the error is the budget.
    int32_t cap = (err < 0 ? -err : err) + (int32_t)stepper_steps_per_rev();
    bool    overshot = false;
    bool    bypass   = !inside_counts(u);
    while (*crept < cap) {
        int8_t  dir = err > 0 ? 1 : -1;
        int32_t inc = creep_inc(err, overshot);
        if (!start_raw(dir * inc, bypass, false) || !stepper_wait(1000))
            return 1;
        *crept += inc;
        vTaskDelay(pdMS_TO_TICKS(SETTLE_PAUSE_MS));
        int32_t next = settle_error(&u);
        *err_deg = steps_deg(next);
        if (next <= tol && next >= -tol)
            return 0;
        if ((int64_t)next * err < 0)
            overshot = true;                    // crossed the target: come back gently
        err = next;
    }
    return 1;
}

bool limits_near_target(double max_deg)
{
    if (!s_have_target || !calibrated() || !encoder_available())
        return false;
    int32_t err = s_target - to_steps(along(encoder_raw()));
    return steps_deg(err < 0 ? -err : err) <= max_deg;
}

#define AT_END_DEG  3.0
bool limits_at_end(bool collapsed)
{
    if (!calibrated() || !encoder_available())
        return false;
    int64_t u    = along(encoder_raw());
    int64_t end  = collapsed ? (int64_t)span_mag() : 0;
    int64_t off  = u > end ? u - end : end - u;
    return deg(off) <= AT_END_DEG;
}

int limits_nearer_end(void)
{
    if (!calibrated() || !encoder_available())
        return -1;
    return along(encoder_raw()) * 2 > (int64_t)span_mag() ? 1 : 0;
}

bool limits_goto_end(bool to_max)
{
    return limits_goto(to_max ? R.span_steps : 0);
}

// ---- calibration ------------------------------------------------------------

static bool finish_calibration(void)
{
    int64_t enc  = s_max_mark.counts - s_min_mark.counts;
    // Output positions, not motor positions: an end reached going forward has
    // the play between motor and output, one reached going backward has not.
    int32_t span = (s_max_mark.pos - contact(s_max_mark.dir)) - (s_min_mark.pos - contact(s_min_mark.dir));

    if (enc == 0 || span == 0) {
        printf("expanded and collapsed are the same place — jog between them first\n");
        return false;
    }
    if (enc >= (int64_t)ENCODER_CPR - 2 * GUARD_COUNTS || enc <= -((int64_t)ENCODER_CPR - 2 * GUARD_COUNTS)) {
        printf("the range spans %.1f deg of the measured shaft — it must fit inside one\n"
               "  turn, with %.1f deg to spare at each end for the guard. Not stored.\n",
               deg(enc < 0 ? -enc : enc), LIMIT_GUARD_DEG);
        return false;
    }
    R.enc_span   = (int32_t)enc;
    R.span_steps = span;
    R.flags      = F_EXPANDED | F_COLLAPSED;

    // Sanity, not a gate: the two spans should agree with the gear ratio. A
    // big disagreement means steps were lost while jogging, or the magnet is
    // not on the shaft the motor drives.
    double expect = deg(enc < 0 ? -enc : enc) / 360.0 * encoder_gear() * stepper_steps_per_rev();
    double ratio  = (double)abs(span) / expect;
    if (ratio < 0.8 || ratio > 1.25)
        printf("  WARNING: %ld steps for %.2f deg is %.0f%% of what gear %.4g predicts —\n"
               "  lost steps while jogging, or the magnet is not on the driven shaft?\n",
               (long)abs(span), deg(enc < 0 ? -enc : enc), 100.0 * ratio, encoder_gear());
    return true;
}

// Move one end of an existing range to where the shaft is now. The axis is
// already known — the step counter is seeded from the angle — so the new span
// is simply the current position: from min for a new max, and the old span
// less the current position for a new min. A range can only shrink this way
// from inside it; to push an end outward, 'lim free', jog past it, then mark.
static bool remark(bool is_max)
{
    int64_t u = reconcile();                    // counts from min, and axis_pos() agrees
    int32_t p = axis_pos();
    int64_t u_signed = R.enc_span > 0 ? u : -u;   // back to raw counting direction
    int64_t enc  = is_max ? u_signed : R.enc_span - u_signed;
    int32_t span = is_max ? p : R.span_steps - p;

    if (enc == 0 || span == 0) {
        printf("the shaft is on the other end — nothing between them\n");
        return false;
    }
    int64_t lim = (int64_t)ENCODER_CPR - 2 * GUARD_COUNTS;
    if (enc >= lim || enc <= -lim) {
        printf("that would make the range %.1f deg — it must fit inside one turn\n",
               deg(enc < 0 ? -enc : enc));
        return false;
    }
    if (is_max) R.collapsed_raw = encoder_raw(); else R.expanded_raw = encoder_raw();
    R.enc_span   = (int32_t)enc;
    R.span_steps = span;
    if (!save()) {
        printf("flash write FAILED\n");
        return false;
    }
    sync_now(false);
    printf("%s moved here: range now %.2f deg = %ld steps, %s. Saved.\n",
           is_max ? "collapsed" : "expanded", deg(span_mag()), (long)R.span_steps,
           R.span_steps > 0 ? "collapsed is forward of expanded" : "collapsed is backward of expanded");
    return true;
}

bool limits_mark(bool is_max)
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
        return remark(is_max);
    mark_t *mine  = is_max ? &s_max_mark : &s_min_mark;
    mark_t *other = is_max ? &s_min_mark : &s_max_mark;
    uint32_t oflag = is_max ? F_EXPANDED : F_COLLAPSED;

    mine->session = true;
    mine->counts  = encoder_counts();
    mine->pos     = stepper_pos();
    mine->dir     = stepper_last_dir();
    mine->sgen    = stepper_pos_gen();
    mine->egen    = encoder_zero_gen();
    uint32_t raw  = encoder_raw();
    if (is_max) R.collapsed_raw = raw; else R.expanded_raw = raw;
    R.flags |= is_max ? F_COLLAPSED : F_EXPANDED;

    printf("%s stored: raw %lu (%.3f deg)\n", is_max ? "collapsed" : "expanded",
           (unsigned long)raw, (double)raw * 360.0 / ENCODER_CPR);
    if (!R.backlash && !(R.flags & oflag))
        printf("  no play stored — if the gearbox has backlash, 'reset' then 'lim play' first\n");

    if (R.flags & oflag) {
        bool ok;
        if (!other->session) {
            printf("  %s was stored in an earlier session, so the steps between the ends\n"
                   "  cannot be counted — jog there and '%s' again\n",
                   is_max ? "expanded" : "collapsed", is_max ? "lim expanded" : "lim collapsed");
            ok = false;
        } else if (other->sgen != mine->sgen || other->egen != mine->egen) {
            printf("  a counter was zeroed or hard-stopped since %s was stored — jog\n"
                   "  there and '%s' again\n",
                   is_max ? "expanded" : "collapsed", is_max ? "lim expanded" : "lim collapsed");
            ok = false;
        } else {
            ok = finish_calibration();
        }
        if (!ok) {
            R.flags &= ~oflag;      // keep only this end
            other->session = false;
        }
    }

    if (!save()) {
        printf("  flash write FAILED\n");
        return false;
    }
    if (calibrated()) {
        printf("CALIBRATED: %.2f deg of travel = %ld steps, %s. Saved.\n"
               "  'mot go expanded' / 'mot go collapsed' move between the ends; 'lim' shows where you are.\n",
               deg(span_mag()), (long)R.span_steps,
               R.span_steps > 0 ? "collapsed is forward of expanded" : "collapsed is backward of expanded");
        sync_now(false);
    } else {
        printf("  saved. Jog to the other end and 'lim %s'.\n", is_max ? "expanded" : "collapsed");
    }
    return true;
}

bool limits_set_span(int32_t steps)
{
    if (!calibrated()) {
        printf("no range stored — 'lim expanded' / 'lim collapsed' first\n");
        return false;
    }
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    if (steps == 0 || (steps > 0) != (R.span_steps > 0)) {
        printf("span must keep its sign: collapsed is %s of expanded, so %s\n",
               R.span_steps > 0 ? "forward" : "backward",
               R.span_steps > 0 ? "positive" : "negative");
        return false;
    }
    int32_t old = R.span_steps;
    R.span_steps = steps;
    if (!save()) {
        R.span_steps = old;
        printf("flash write FAILED\n");
        return false;
    }
    sync_now(false);
    printf("span %ld -> %ld steps for %.2f deg (%.1f steps/deg). Saved.\n",
           (long)old, (long)steps, deg(span_mag()), (double)abs(steps) / deg(span_mag()));
    return true;
}

bool limits_store_span(void)
{
    if (!calibrated()) {
        printf("no range stored — 'lim expanded' / 'lim collapsed' first\n");
        return false;
    }
    if (!encoder_available()) {
        printf("no encoder — cannot tell where the shaft is. 'enc' for why.\n");
        return false;
    }
    if (stepper_busy()) {
        printf("stop first\n");
        return false;
    }
    if (!s_synced || s_origin_gen != stepper_pos_gen()) {
        printf("the step counter is not seeded — 'mot go expanded' then 'mot go collapsed' first\n");
        return false;
    }
    int64_t u   = along(encoder_raw());
    int64_t tol = (int64_t)(LIMIT_SETTLE_DEG / 360.0 * ENCODER_CPR);
    if (u < (int64_t)span_mag() - tol || u > (int64_t)span_mag() + tol) {
        printf("the shaft is %.2f deg from expanded, not at collapsed — 'mot go collapsed' first, then this\n", deg(u));
        return false;
    }
    int32_t p = axis_pos();
    if (p == 0 || (p > 0) != (R.span_steps > 0)) {
        printf("the step counter reads %ld from expanded — not a span. 'mot go expanded', 'mot go collapsed', then this\n", (long)p);
        return false;
    }
    printf("shaft at collapsed (%.3f deg from it); step counter says %ld from expanded\n",
           deg(u - (int64_t)span_mag()), (long)p);
    return limits_set_span(p);
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
    R.expanded_raw       = 0;
    R.collapsed_raw       = 0;
    R.enc_span      = 0;
    R.span_steps    = 0;
    R.steps_per_rev = 0;
    settings_mark_dirty();
    s_min_mark = s_max_mark = (mark_t){ 0 };
    s_synced = false;
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
    if (calibrated()) {
        printf("the stored span was measured with the play as it was — 'reset' first\n");
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
    if (calibrated()) {
        printf("the stored span was measured with the play as it was — 'reset' first\n");
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

// The flash record, field by field, exactly as stored — no derivation.
static void dump_record(void)
{
    printf("  stored: flags 0x%lx (%s%s) | expanded_raw %lu | collapsed_raw %lu | enc_span %ld | span_steps %ld | steps_per_rev %lu | play %ld | approach %.1f deg\n",
           (unsigned long)R.flags,
           R.flags & F_EXPANDED ? "expanded" : "-", R.flags & F_COLLAPSED ? "+collapsed" : "",
           (unsigned long)R.expanded_raw, (unsigned long)R.collapsed_raw,
           (long)R.enc_span, (long)R.span_steps,
           (unsigned long)R.steps_per_rev, (long)R.backlash, (double)R.approach_deg);
}

void limits_report(void)
{
    dump_record();
    if (!calibrated()) {
        printf("limits: NOT CALIBRATED — expanded %s, collapsed %s. Motor LOCKED%s, desk LOCKED.\n",
               R.flags & F_EXPANDED ? "stored" : "missing",
               R.flags & F_COLLAPSED ? "stored" : "missing",
               s_free ? " (overridden by 'lim free')" : "");
        if (R.flags & F_EXPANDED)
            printf("  expanded raw %lu%s\n", (unsigned long)R.expanded_raw,
                   s_min_mark.session ? "" : " (earlier session — store it again)");
        if (R.flags & F_COLLAPSED)
            printf("  collapsed raw %lu%s\n", (unsigned long)R.collapsed_raw,
                   s_max_mark.session ? "" : " (earlier session — store it again)");
        printf("  play %ld microsteps%s\n", (long)R.backlash,
               R.backlash ? "" : " — 'lim play [deg]' measures it, 'eeprom set lim_backlash <n>' sets it");
        printf("  'calibrate' walks through it. By hand: 'mot jog <steps>' to an end, 'lim expanded';\n"
               "  'mot jog' to the other, 'lim collapsed'. Both in one session, no zero or halt in\n"
               "  between. The range must fit in one turn.\n");
        return;
    }
    printf("limits: expanded raw %lu | collapsed raw %lu | %.2f deg of travel = %ld steps (collapsed is %s of expanded) | play %ld%s\n",
           (unsigned long)R.expanded_raw, (unsigned long)R.collapsed_raw,
           deg(span_mag()), (long)R.span_steps,
           R.span_steps > 0 ? "forward" : "backward", (long)R.backlash,
           s_free ? " — NOT ENFORCED ('lim free')" : "");
    if (!encoder_available()) {
        printf("  no encoder — position in the range unknown\n");
        return;
    }
    int64_t u = along(encoder_raw());
    printf("  ");
    say_where(u);
    if (s_synced && s_origin_gen == stepper_pos_gen()) {
        int32_t p_step = axis_pos(), p_enc = to_steps(u);
        printf("  step counter says %ld, encoder says %ld — %+.3f deg apart\n",
               (long)p_step, (long)p_enc, steps_deg(p_step - p_enc));
    } else {
        printf("  step counter not seeded yet — it is on the next move, or 'lim sync'\n");
    }
    printf("  guard trips %.1f deg past either end\n", LIMIT_GUARD_DEG);
}

// ---- the guard ----------------------------------------------------------------
// Encoder task, every sample. Only judges a motor that is moving under the
// range: a shaft turned by hand while disabled is not the motor's doing, and a
// move that started outside is on its way back in.
void limits_guard(uint32_t raw)
{
    if (s_free || s_bypass || !calibrated() || !stepper_busy())
        return;
    int64_t u = along(raw);
    if (u < -GUARD_COUNTS || u > (int64_t)span_mag() + GUARD_COUNTS) {
        stepper_stop_hard();
        s_trip_u  = u;
        s_tripped = true;
    }
}

void limits_poll_trip(void)
{
    if (!s_tripped)
        return;
    s_tripped = false;
    int64_t u = s_trip_u;
    printf("\nLIMIT GUARD: the shaft went %.2f deg %s — HARD STOP. The step counter is\n"
           "  re-seeded from the encoder on the next move; 'lim' to see where it is.\n> ",
           u < 0 ? -deg(u) : deg(u - span_mag()), u < 0 ? "past expanded" : "past collapsed");
}
