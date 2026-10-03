//
// flap_task — the stop / flap / continue sequence.
//
// flap.c decides WHEN; this does the moving. It runs only when flap.c has
// already brought the desk to a halt and handed the bus over, so it owns the
// desk outright for the duration.
//
// Driving to the flap height here, rather than trying to stop the board there,
// is what removes the whole stopping-distance problem: the approach releases
// early and confirms it stopped, in either direction, whatever the speed.
//
#include "flap.h"
#include "bus.h"
#include "wire.h"
#include "board_config.h"
#include "trace.h"
#include "limits.h"
#include "stepper.h"
#include "led.h"
#include "settings.h"

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

#define KEY_IDLE    0x00
#define KEY_DOWN    0x06
#define KEY_UP      0x08
#define KEY_DOWN_E  0x05            // one frame: a 10 mm step
#define KEY_UP_E    0x07

static bool wait_sent(uint32_t timeout_ms);

// The step-in in desk_move_to() relies on this: from more than NEAR short, one
// step cannot end past the target.
_Static_assert(DESK_NEAR_MM >= DESK_NUDGE_MM, "a step could overshoot the target");

static volatile bool s_abort;

static int32_t height_now(void)
{
    uint16_t mm; uint32_t age;
    if (!desk_height_mm(&mm, &age) || age > DESK_HEIGHT_STALE_MS)
        return -1;
    return mm;
}

// Wait for the height to stop changing, and return where it stopped. The quiet
// timer re-arms on every change, so this cannot return while it is drifting.
// Close to the target, a shorter quiet is enough to call the desk stopped.
// Going up the board ends a move with a slow crawl — a millimetre every 0.3 to
// 0.7 s — and waiting out 1.2 s of silence after each one kept the flap job
// (and desk_resume_pct) waiting a second or two for a desk that was, to within
// a millimetre or two, already there.
#define SETTLE_NEAR_QUIET_MS    400

static int32_t settle_near(uint32_t quiet_ms, uint32_t timeout_ms, int32_t target)
{
    TickType_t t0 = xTaskGetTickCount();
    int32_t last = height_now();
    TickType_t stable = t0;
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int32_t cur = height_now();
        if (cur != last) { last = cur; stable = xTaskGetTickCount(); continue; }
        TickType_t quiet = xTaskGetTickCount() - stable;
        int32_t    off   = last - target;
        if (quiet >= pdMS_TO_TICKS(quiet_ms))
            break;
        if (target >= 0 && last >= 0 && (off < 0 ? -off : off) <= DESK_NEAR_MM &&
            quiet >= pdMS_TO_TICKS(SETTLE_NEAR_QUIET_MS))
            break;
    }
    return last;
}

static int32_t settle(uint32_t quiet_ms, uint32_t timeout_ms)
{
    return settle_near(quiet_ms, timeout_ms, -1);
}

// Drive, release early, let it coast, confirm it stopped. That is the whole
// algorithm — there is no correction afterwards. The only fine adjustment this
// desk offers is a 10 mm step, so correcting a 6 mm error means landing 4 mm
// out on the other side: a stop, a pause and a beep to gain two millimetres.
// Within DESK_NEAR_MM is close enough, and stopping SHORT is the safe side.
// on_stop, if given, is called ONCE, the moment the desk is told to stop —
// at the release point, before a step-in, or at once if it is already there —
// so whatever waits for the desk (the flap) can start while it coasts. Not
// called on a failure.
bool desk_move_to(uint16_t target_mm, void (*on_stop)(void))
{
    if (target_mm < DESK_MIN_MM || target_mm > DESK_MAX_MM) {
        printf("[desk] %u mm is outside [%u, %u] — refusing\n",
               target_mm, DESK_MIN_MM, DESK_MAX_MM);
        return false;
    }
    int32_t cur = height_now();
    if (cur < 0) {
        printf("[desk] no fresh height — is the board tap wired?\n");
        return false;
    }

    uint16_t cu, cd;
    flap_coast(&cu, &cd);

    int32_t err = (int32_t)target_mm - cur;
    if ((err < 0 ? -err : err) <= DESK_NEAR_MM) {
        printf("[desk] already at %ld mm, near enough to %u\n", (long)cur, target_mm);
        if (on_stop) on_stop();
        return true;
    }

    bool     up      = err > 0;
    uint16_t lead    = up ? cu : cd;
    int32_t  release = up ? (int32_t)target_mm - lead : (int32_t)target_mm + lead;

    // Already past where we would release: a drive would coast past the
    // target. Step instead — one tap is a fixed DESK_NUDGE_MM — while the
    // desk is more than DESK_NEAR_MM short. Since NEAR >= NUDGE, a step from
    // further than NEAR can never end past the target, so this only ever
    // closes in from the short side. Without it, a desk raised to 754 by hand
    // had its flap run there, 16 mm short of 770, and looked as if it never
    // stopped for the flap at all.
    if (up ? cur >= release : cur <= release) {
        if (on_stop) on_stop();         // the desk is as good as stopped here
        for (int tries = 0; tries < 3 && !s_abort; tries++) {
            int32_t e = (int32_t)target_mm - cur;
            if ((e < 0 ? -e : e) <= DESK_NEAR_MM)
                break;
            printf("[desk] at %ld mm, too close to %u to drive — stepping %s\n",
                   (long)cur, target_mm, e > 0 ? "up" : "down");
            wire_send_once(e > 0 ? KEY_UP_E : KEY_DOWN_E);
            if (!wait_sent(WIRE_ONESHOT_TIMEOUT_MS))
                return false;
            int32_t landed = settle(1200, 6000);
            if (landed < 0 || landed == cur) {
                printf("[desk] the step did not move it — leaving it at %ld mm\n", (long)cur);
                break;
            }
            cur = landed;
        }
        int32_t e = (int32_t)target_mm - cur;
        printf("[desk] at %ld mm, %ld from %u\n", (long)cur, (long)(e < 0 ? -e : e), target_mm);
        return true;
    }

    printf("[desk] %ld -> %u mm (%s), releasing at %ld\n",
           (long)cur, target_mm, up ? "up" : "down", (long)release);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DESK_MOVE_TIMEOUT_MS);
    int32_t    last = cur;
    TickType_t last_change = xTaskGetTickCount();

    for (;;) {
        if (s_abort) { wire_set_pending(KEY_IDLE); return false; }
        cur = height_now();
        if (cur < 0) {
            wire_set_pending(KEY_IDLE);
            printf("[desk] height went stale mid-move\n");
            return false;
        }
        if (up ? cur >= release : cur <= release) break;
        if (xTaskGetTickCount() > deadline) {
            wire_set_pending(KEY_IDLE);
            printf("[desk] TIMEOUT at %ld mm\n", (long)cur);
            return false;
        }
        if (cur != last) { last = cur; last_change = xTaskGetTickCount(); }
        else if (xTaskGetTickCount() - last_change > pdMS_TO_TICKS(DESK_STALL_MS)) {
            wire_set_pending(KEY_IDLE);
            printf("[desk] STALLED at %ld mm — end of travel, or an obstruction\n",
                   (long)cur);
            return false;
        }
        wire_set_pending(up ? KEY_UP : KEY_DOWN);
        vTaskDelay(pdMS_TO_TICKS(DESK_POLL_MS));
    }

    wire_set_pending(KEY_IDLE);
    if (on_stop) on_stop();             // the stop just went out
    int32_t landed = settle_near(1200, 8000, target_mm);
    int32_t off = landed - (int32_t)target_mm;
    printf("[desk] stopped at %ld mm, %ld from %u\n",
           (long)landed, (long)(off < 0 ? -off : off), target_mm);
    return true;
}

void desk_abort(void) { s_abort = true; }

// Wait for a one-shot code to actually leave. It goes out on the PANEL's next
// poll, not on a timer of ours, so the job must not be handed back before
// then: flap_job_done() returns the bus to the panel, and a one-shot still
// waiting when that happens is a recall that is never sent.
static bool wait_sent(uint32_t timeout_ms)
{
    TickType_t t0 = xTaskGetTickCount();
    while (wire_burst_left()) {
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms)) {
            wire_set_pending(KEY_IDLE);
            printf("[flap] the panel went quiet — the recall never reached the bus\n");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// ---- the flap itself ------------------------------------------------------
//
// THIS is where the stepper goes. Everything else in this firmware exists to
// get the desk stopped at the right height with the bus in hand; the move
// itself is a dwell until the motor is wired.
// The flap is EXPANDED while the desk is below the flap height and COLLAPSED
// above it. So a desk on its way up collapses it, one on its way down expands
// it. The move goes through limits.c like any 'mot go': range-checked, fast
// to within lim_approach_deg of the end, then crept onto it under the encoder,
// with the guard watching.
//
// Two calls: flap_start() and flap_finish(). Normally flap_start() runs once
// the desk is still at the flap height. With mot_early_start set it runs
// earlier — from desk_move_to()'s on-stop hook, the moment the desk is told to
// stop — so the coast and the flap's travel overlap. Only flap_finish()
// decides whether the flap got there.
static volatile bool s_early = FLAP_START_WITH_STOP;    // mot_early_start
void flap_set_early_start(bool on) { s_early = on; }
bool flap_early_start(void)        { return s_early; }

// desk_resume_pct: re-send the recall once the flap has covered this share of
// its move, measured by the encoder. 100, the default: only once it has
// settled on its end. Below that the desk sets off while the flap finishes —
// checked through the fast part and once more at its end; a share that the
// fast part does not reach (it stops lim_approach_deg short) means the end
// of the creep, the same as 100.
static volatile uint8_t s_resume_pct = 100;
void    flap_set_resume_pct(uint8_t pct) { s_resume_pct = pct; }
uint8_t flap_resume_pct(void)            { return s_resume_pct; }

// A recall must reach a desk that has FINISHED moving. One that arrives while
// the board is still in the slow crawl that ends a move up is announced and
// then dropped — the desk stays where it is (2026-10-03). The flap may start
// sooner; the recall waits for this much stillness.
#define RESUME_STILL_MS     1200
#define RESUME_CHECK_MS     4000    // the desk should be on its way by then
#define RESUME_MOVED_MM     5

static bool wait_still(uint32_t still_ms, uint32_t timeout_ms)
{
    TickType_t t0 = xTaskGetTickCount();
    while (desk_still_ms() < still_ms) {
        if (s_abort || xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms))
            return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

// Has the flap covered desk_resume_pct of its move? Then send the desk on —
// once. `can` is false for a manual crossing, which has no recall to send.
//
// The recall is only QUEUED here: the bus sends it on the panel's next poll,
// and the flap carries on meanwhile. Waiting for it to go out (up to a second)
// is what used to hold the creep back until the desk had started. Whether it
// went out is checked once the flap is done.
static void maybe_resume(bool can, uint8_t key, uint16_t dest, bool *resumed)
{
    if (!can || *resumed || s_abort || s_resume_pct >= 100 || !FLAP_DRIVES_MOTOR)
        return;
    if (desk_still_ms() < RESUME_STILL_MS)
        return;                         // the desk is still finishing: later
    double p = limits_progress();
    if (p >= 0.0 && p * 100.0 >= (double)s_resume_pct) {
        printf("[flap] the flap is %.0f%% of the way — the desk goes on (desk_resume_pct %u)\n",
               p * 100.0, s_resume_pct);
        printf("[flap] resuming — re-sending the recall for %u mm\n", dest);
        trace_add(TR_JOB, TRJ_RESUME, key);
        wire_send_once(key);
        *resumed = true;
    }
}

static bool       s_collapse;           // where this job's flap is going
static bool       s_flap_started;
static bool       s_flap_start_ok;
static TickType_t s_flap_t0;
static int32_t    s_flap_pos0;          // step counter at the start, for the stall check

static void flap_start(void)
{
    if (s_flap_started) return;
    s_flap_started = true;
    s_flap_t0      = xTaskGetTickCount();
    s_flap_pos0    = stepper_pos();
    printf("[flap] moving the flap to %s\n", s_collapse ? "COLLAPSED" : "EXPANDED");
    trace_add(TR_JOB, TRJ_FLAP_START, s_collapse);
    s_flap_start_ok = !FLAP_DRIVES_MOTOR || limits_goto_end(s_collapse);
}

// The fast part: wait for the open-loop run to end. True if it ran to its end
// (which says the pulses went out, not that the flap followed them).
static int32_t s_fast_steps;

static bool flap_fast(bool can, uint8_t key, uint16_t dest, bool *resumed)
{
    bool ok = s_flap_started && s_flap_start_ok;

    if (ok && !FLAP_DRIVES_MOTOR) {             // bench: the dwell, from the start
        TickType_t left = pdMS_TO_TICKS(FLAP_MOVE_MS);
        TickType_t gone = xTaskGetTickCount() - s_flap_t0;
        if (gone < left) vTaskDelay(left - gone);
        return true;
    }
    while (ok && stepper_busy()) {
        if (s_abort) {
            stepper_stop();
            printf("[flap] stopped by 'stop'\n");
            ok = false;
        } else if (xTaskGetTickCount() - s_flap_t0 > pdMS_TO_TICKS(FLAP_MOVE_TIMEOUT_MS)) {
            stepper_stop_hard();
            printf("[flap] the flap move TIMED OUT after %u s\n", FLAP_MOVE_TIMEOUT_MS / 1000);
            ok = false;
        } else {
            maybe_resume(can, key, dest, resumed);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_fast_steps = stepper_pos() - s_flap_pos0;
    if (FLAP_DRIVES_MOTOR) {
        double p = limits_progress();
        trace_add(TR_JOB, TRJ_FAST_END, p < 0.0 ? -1 : (int32_t)(p * 100.0 + 0.5));
    }
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(100));         // let the load come to rest
        maybe_resume(can, key, dest, resumed);
    }
    else
        trace_add(TR_JOB, TRJ_FLAP_END, 0);
    return ok;
}

// The creep onto the end, under the encoder. True only if the encoder
// confirms the flap is there.
static bool flap_settle(void)
{
    const char *to = s_collapse ? "COLLAPSED" : "EXPANDED";
    if (!FLAP_DRIVES_MOTOR) {
        trace_add(TR_JOB, TRJ_FLAP_END, 1);
        return true;
    }
    double  err;
    int32_t crept;
    int     r = limits_settle(&err, &crept);
    // The creep should only cover the last lim_approach_deg — a few hundred
    // steps. Far more means the fast part's steps did not turn the flap: the
    // motor stalled and skipped them. The creep still gets the flap there
    // (slowly), so it is a warning, not a failure.
    int32_t fast = s_fast_steps < 0 ? -s_fast_steps : s_fast_steps;
    if (crept > 1000 && crept * 10 > fast)
        printf("[flap] STALL: the motor skipped most of the fast part (%ld steps crept).\n"
               "       Check the driver's supply first; else lower the speed:\n"
               "       'eeprom set mot_speed_sps <n>' (now %lu).\n",
               (long)crept, (unsigned long)stepper_speed());
    bool ok = (r == 0);
    if (ok)
        printf("[flap] %s (%+.2f deg from the end)\n", to, err);
    else
        printf("[flap] the flap did NOT reach %s — %+.2f deg off after %ld steps of creep\n",
               to, err, (long)crept);
    trace_add(TR_JOB, TRJ_FLAP_END, ok);
    return ok;
}

// Finish the desk's move by RE-ISSUING the recall, not by driving to the
// announced height: the board then lands on its own stored preset exactly,
// with none of our error on top.
//
// ONE FRAME, and that is the whole of it. A recall is a one-shot code: the
// panel puts it on the wire for a single frame and is idle again on the next
// poll (protocol doc §4). Held for two or three frames instead, the second one
// reaches a board that has just accepted the first, and it cancels it — the
// desk sets off, travels about a centimetre and stops there, nowhere near the
// preset. Whether it survives depends on where the second frame lands in the
// board's ~1 s of start-up latency, which is why it worked some of the time.
static void resume_desk(uint8_t key, uint16_t dest)
{
    wait_still(RESUME_STILL_MS, 3000);
    printf("[flap] resuming — re-sending the recall for %u mm\n", dest);
    trace_add(TR_JOB, TRJ_RESUME, key);
    wire_send_once(key);
    trace_add(TR_JOB, TRJ_SENT, wait_sent(WIRE_ONESHOT_TIMEOUT_MS));
}

// The desk did not get to the flap height: whatever the flap started, stop it.
static void flap_cancel(void)
{
    if (s_flap_started && FLAP_DRIVES_MOTOR)
        stepper_stop();
}

// The recall went out; did the desk set off? It should be a few millimetres
// on its way towards the destination within RESUME_CHECK_MS. If not, the board
// dropped the recall — send it once more, to a desk that is still, rather than
// leave it parked at the flap height.
static void confirm_resume(uint8_t key, uint16_t dest)
{
    int32_t from = height_now();
    if (from < 0 || !dest)
        return;
    int8_t     dir = dest > from ? 1 : -1;
    TickType_t t0  = xTaskGetTickCount();
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(RESUME_CHECK_MS)) {
        int32_t now = height_now();
        if (now >= 0 && (now - from) * dir >= RESUME_MOVED_MM)
            return;                     // on its way
        if (s_abort)
            return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("[flap] the desk did not set off — the recall was dropped. Sending it again.\n");
    trace_add(TR_JOB, TRJ_RETRY, key);
    resume_desk(key, dest);
}

void flap_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t  key;
        uint16_t dest;
        if (!flap_job_take(&key, &dest)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_abort = false;

        if (!key) {                     // from 'go': just the move
            trace_add(TR_JOB, TRJ_GO, dest);
            desk_move_to(dest, NULL);
            flap_job_done();
            trace_add(TR_JOB, s_abort ? TRJ_ABORTED : TRJ_DONE, 0);
            continue;
        }
        if (key == FLAP_JOB_SWAP) trace_add(TR_JOB, TRJ_SWAP, dest > flap_height());
        else                      trace_add(TR_JOB, TRJ_TAKE, dest);

        bool swap = (key == FLAP_JOB_SWAP);      // a manual crossing: no recall
        if (swap)
            printf("[flap] manual crossing: the flap, then back to the panel\n");
        else
            printf("[flap] taking over: stop at %u, flap, then on to %u mm\n",
                   flap_height(), dest);

        // Which way the desk is going decides the flap's end. The destination
        // says so; without one (a recall caught late with no learned height),
        // whichever end the flap is nearer, it goes to the other.
        s_collapse      = dest ? dest > flap_height() : limits_nearer_end() == 0;
        s_flap_started  = false;
        s_flap_start_ok = false;

        // flap.c may hand the job over while it is still stopping the desk:
        // its stop code has gone out, so the flap starts now, and the desk is
        // waited for. If it could not stop the desk, the job is off.
        if (flap_desk_stopping()) {
            if (s_early) flap_start();
            while (flap_desk_stopping() && !s_abort)
                vTaskDelay(pdMS_TO_TICKS(20));
            if (!flap_job_active()) {
                flap_cancel();
                printf("[flap] the desk could not be stopped — flap move cancelled\n");
                flap_job_done();
                trace_add(TR_JOB, TRJ_ABORTED, 0);
                continue;
            }
        }

        // Already at the flap height, on the side it is heading for and within
        // the safe gap: the flap moves right here. Driving the desk back to
        // the mark first would be a step the wrong way for nothing.
        int32_t cur0 = height_now();
        int32_t off0 = cur0 - (int32_t)flap_height();
        bool    here = cur0 >= 0 &&
                       (dest > flap_height() ? (off0 >= 0 && off0 <= FLAP_SAFE_GAP_MM)
                                             : (off0 <= 0 && off0 >= -FLAP_SAFE_GAP_MM));
        if (here)
            printf("[desk] at %ld mm, within the safe gap of %u — the flap moves here\n",
                   (long)cur0, flap_height());
        bool there = here || desk_move_to(flap_height(), s_early ? flap_start : NULL);
        trace_add(TR_JOB, TRJ_AT_FLAP, there);
        if (!there || s_abort)
            flap_cancel();
        else
            flap_start();               // the desk is still; no-op if already started
        // desk_resume_pct below 100: the desk may set off while the flap is
        // still on its way, once the encoder says it has covered that share.
        bool resumed = false;
        bool ran = there && !s_abort && flap_fast(!swap, key, dest, &resumed);

        bool flapped = ran && !s_abort && flap_settle();
        if (there && !s_abort && !flapped) {
            led_refused();
            if (resumed)
                // Too late to hold the desk: it is already on its way.
                printf("[flap] FLAP MOVE FAILED — the desk had ALREADY resumed\n"
                       "       (desk_resume_pct %u). 'lim' says where the flap is.\n", s_resume_pct);
            else
                // The flap is somewhere unknown, in the desk's path. Staying
                // put is the only safe answer; the panel has the desk back.
                printf("[flap] FLAP MOVE FAILED — the desk stays at %u mm%s.\n"
                       "       'lim' says where the flap is.\n", flap_height(),
                       swap ? "; pressing again retries" : " and the recall is not sent");
        }
        if (flapped && !resumed && !s_abort && !swap)
            resume_desk(key, dest);
        if (resumed)                    // queued early: make sure it went out
            trace_add(TR_JOB, TRJ_SENT, wait_sent(WIRE_ONESHOT_TIMEOUT_MS));
        if ((resumed || flapped) && !swap && !s_abort)
            confirm_resume(key, dest);
        flap_job_done();
        trace_add(TR_JOB, s_abort ? TRJ_ABORTED : TRJ_DONE, 0);
    }
}
