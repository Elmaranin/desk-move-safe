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
static int32_t settle(uint32_t quiet_ms, uint32_t timeout_ms)
{
    TickType_t t0 = xTaskGetTickCount();
    int32_t last = height_now();
    TickType_t stable = t0;
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int32_t cur = height_now();
        if (cur != last) { last = cur; stable = xTaskGetTickCount(); }
        else if (xTaskGetTickCount() - stable >= pdMS_TO_TICKS(quiet_ms)) break;
    }
    return last;
}

// Drive, release early, let it coast, confirm it stopped. That is the whole
// algorithm — there is no correction afterwards. The only fine adjustment this
// desk offers is a 10 mm step, so correcting a 6 mm error means landing 4 mm
// out on the other side: a stop, a pause and a beep to gain two millimetres.
// Within DESK_NEAR_MM is close enough, and stopping SHORT is the safe side.
bool desk_move_to(uint16_t target_mm)
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
    int32_t landed = settle(1200, 8000);
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
static void run_flap(void)
{
    printf("[flap] at the height — running the flap\n");
    trace_add(TR_JOB, TRJ_FLAP_START, 0);
    vTaskDelay(pdMS_TO_TICKS(FLAP_MOVE_MS));
    trace_add(TR_JOB, TRJ_FLAP_END, 0);
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
            desk_move_to(dest);
            flap_job_done();
            trace_add(TR_JOB, s_abort ? TRJ_ABORTED : TRJ_DONE, 0);
            continue;
        }
        trace_add(TR_JOB, TRJ_TAKE, dest);

        printf("[flap] taking over: stop at %u, flap, then on to %u mm\n",
               flap_height(), dest);

        bool there = desk_move_to(flap_height());
        trace_add(TR_JOB, TRJ_AT_FLAP, there);
        if (there && !s_abort) {
            run_flap();
            if (!s_abort) {
                // Resume by RE-ISSUING the recall, not by driving to the
                // announced height: the board then lands on its own stored
                // preset exactly, with none of our error on top, and the user
                // gets the position they actually asked for.
                //
                // ONE FRAME, and that is the whole of it. A recall is a
                // one-shot code: the panel puts it on the wire for a single
                // frame and is idle again on the next poll (protocol doc §4).
                // Held for two or three frames instead, the second one reaches
                // a board that has just accepted the first, and it cancels it
                // — the desk sets off, travels about a centimetre and stops
                // there, nowhere near the preset. Whether it survives depends
                // on where the second frame lands in the board's ~1 s of
                // start-up latency, which is why it worked some of the time.
                printf("[flap] resuming — re-sending the recall for %u mm\n", dest);
                trace_add(TR_JOB, TRJ_RESUME, key);
                wire_send_once(key);
                trace_add(TR_JOB, TRJ_SENT, wait_sent(WIRE_ONESHOT_TIMEOUT_MS));
            }
        }
        flap_job_done();
        trace_add(TR_JOB, s_abort ? TRJ_ABORTED : TRJ_DONE, 0);
    }
}
