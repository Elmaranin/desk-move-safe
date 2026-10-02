//
// flap — see flap.h.
//
#include "flap.h"
#include "bus.h"
#include "wire.h"
#include "mode.h"
#include "settings.h"
#include "trace.h"
#include "led.h"
#include "limits.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

#define KEY_IDLE    0x00
#define KEY_STAND   0x01
#define KEY_SIT     0x02
#define KEY_SAVE_ST 0x03
#define KEY_SAVE_SI 0x04
#define KEY_DOWN_E  0x05
#define KEY_DOWN    0x06
#define KEY_UP_E    0x07
#define KEY_UP      0x08

static bool is_up(uint8_t k)     { return k == KEY_UP   || k == KEY_UP_E; }
static bool is_down(uint8_t k)   { return k == KEY_DOWN || k == KEY_DOWN_E; }
static bool is_motion(uint8_t k) { return is_up(k) || is_down(k); }
static bool is_recall(uint8_t k) { return k == KEY_STAND || k == KEY_SIT; }
static int  slot(uint8_t k)      { return k == KEY_STAND ? 0 : k == KEY_SIT ? 1 : -1; }

typedef enum {
    ST_PASS = 0,        // the panel is in charge
    ST_PLANNING,        // a recall seen; waiting for its announced destination
    ST_STOPPING,        // bringing the desk to a halt at the flap height
    ST_MOVING,          // the flap task owns it from here
} state_t;

static volatile state_t   s_state;
static volatile bool      s_on       = FLAP_ON;
static volatile uint16_t  s_height   = FLAP_HEIGHT_MM;
static volatile bool      s_ceil_on  = DESK_CEILING_ON;
static volatile uint16_t  s_coast_up = DESK_COAST_UP_MM;
static volatile uint16_t  s_coast_dn = DESK_COAST_DOWN_MM;
static volatile uint32_t  s_blocked;
static volatile bool      s_armed = true;

static uint16_t s_preset_mm[2]    = { PRESET_STAND_MM, PRESET_SIT_MM };
static bool     s_preset_known[2] = { PRESET_STAND_MM != 0, PRESET_SIT_MM != 0 };

// Stopping
static uint8_t    s_stop_key;
static uint32_t   s_stop_frames;
static uint8_t    s_stop_tries;
static int8_t     s_fired_dir;
static TickType_t s_phase_start;
static uint16_t   s_last_mm;
static TickType_t s_last_change;

// The planned move, handed to the flap task
static volatile bool s_job_ready;
static volatile bool s_job_running;
static uint8_t       s_job_key;         // the recall to re-issue when done
static uint16_t      s_job_dest;
static uint16_t      s_plan_from;
static uint32_t      s_plan_gen;        // announcements before the recall
static TickType_t    s_plan_started;

// Hands off while a resumed move finishes
static volatile bool       s_hands_off;
static TickType_t          s_hands_off_from;
static TickType_t          s_hands_off_until;

// ---- helpers --------------------------------------------------------------

static int32_t height(void)
{
    uint16_t mm; uint32_t age;
    if (!desk_height_mm(&mm, &age) || age > DESK_HEIGHT_STALE_MS)
        return -1;
    return mm;
}

// Has the desk stopped?
//
// A FRESH reading is required, not merely an unchanging one. The board reports
// height only in answer to a poll, so the moment we stop sending it stops
// sending — and a frozen reading looks exactly like a stationary desk.
static bool settled(int32_t mm)
{
    uint32_t age;
    if (mm < 0 || !desk_height_mm(NULL, &age) || age > FLAP_FRESH_MS)
        return false;
    TickType_t now = xTaskGetTickCount();
    if ((uint16_t)mm != s_last_mm) {
        s_last_mm = (uint16_t)mm;
        s_last_change = now;
        return false;
    }
    return (now - s_last_change) >= pdMS_TO_TICKS(FLAP_SETTLE_MS);
}

// The interrupt that stops a move the board is driving: an edge code — a TAP.
//
// A tap, not a hold, because a held code IS a move command. And AWAY from the
// flap height, because a tap is worth DESK_NUDGE_MM: tapping the way the desk
// is already going asks for another centimetre toward the height we are trying
// to stop short of.
static uint8_t stop_code(int8_t dir)
{
    s_stop_key    = (dir > 0) ? KEY_DOWN_E : KEY_UP_E;
    s_stop_frames = 1;
    s_stop_frames--;
    return s_stop_key;
}

// Is the desk heading FOR the flap height, and close enough that it must be
// stopped now to stop there at all? Both halves matter: without "approaching",
// "below the mark" is true of the whole bottom of the range.
static bool approaching(int8_t dir, int32_t mm)
{
    if (mm < 0 || dir == 0) return false;
    int32_t h = (int32_t)s_height;
    if (dir > 0) return mm < h && mm >= h - (int32_t)s_coast_up;
    return mm > h && mm <= h + (int32_t)s_coast_dn;
}

// ---- taps -----------------------------------------------------------------
// A tap (one edge frame) is a fixed DESK_NUDGE_MM step that ends by itself —
// and it can only be stopped by tapping the other way, which is a step back.
// So a tap is judged by where it WILL end, not by how close it is getting:
// one that stays short of the flap height is left alone, and one that would
// end past it is not sent at all. Stopping a tap that was never going to reach
// the flap is what once turned "tap up at 742" into 752 and straight back to
// 742.
#define TAP_WINDOW_MS   4000            // the step's ~1 s latency plus its travel

static uint8_t    s_drive_key;          // the recall driving the desk now, 0 = none
static uint32_t   s_drive_gen;          // announcements before it
static int32_t    s_tap_from = -1;      // height the last tap started from
static int8_t     s_tap_dir;
static TickType_t s_tap_at;

static int8_t edge_dir(uint8_t k) { return k == KEY_UP_E ? 1 : k == KEY_DOWN_E ? -1 : 0; }

// Track the panel's taps: an edge starts one, anything that moves the desk
// otherwise (a held code, a recall) ends it.
static void track_tap(uint8_t panel_code, int32_t mm)
{
    if (edge_dir(panel_code)) {
        s_tap_from = mm;
        s_tap_dir  = edge_dir(panel_code);
        s_tap_at   = xTaskGetTickCount();
    } else if (is_motion(panel_code) || is_recall(panel_code)) {
        s_tap_from = -1;
    } else if (s_tap_from >= 0 &&
               xTaskGetTickCount() - s_tap_at > pdMS_TO_TICKS(TAP_WINDOW_MS)) {
        s_tap_from = -1;
    }
}

// Would a step of DESK_NUDGE_MM from `from` in `dir` end past the flap height?
static bool tap_crosses(int8_t dir, int32_t from)
{
    int32_t h = (int32_t)s_height, end = from + dir * DESK_NUDGE_MM;
    return dir > 0 ? (from < h && end > h) : (from > h && end < h);
}

// The move in progress is a tap that ends short of the flap height.
static bool tap_stays_short(int8_t dir)
{
    return s_tap_from >= 0 && dir == s_tap_dir && !tap_crosses(s_tap_dir, s_tap_from);
}

// The presets are the only settings stored without being asked. s_preset_mm
// holds what is in flash (loaded once at boot); a recall's announcement is
// compared with it, and flash is written ONLY when the two differ. Seeing the
// same height again just confirms it — no write. s_preset_known is a RAM-only
// "may be trusted for a takeover" flag, cleared when the panel re-saves.
// Does the flap still have to move before the desk may pass the flap height in
// this direction? Above it the flap must be COLLAPSED, below it EXPANDED, so
// the answer is read off the flap itself (the encoder), not remembered: a flap
// already at the right end lets the desk through, and one that is not stops
// it — however the desk got here. Without a motor (bench), the old "armed"
// flag stands in.
static bool needs_swap(int8_t dir)
{
    if (!FLAP_DRIVES_MOTOR)
        return s_armed;
    return !limits_at_end(dir > 0);
}

// A manual move is crossing: hand flap_task a job that moves the flap and
// gives the bus back. See FLAP_JOB_SWAP.
static void start_swap(int8_t dir)
{
    s_job_key     = FLAP_JOB_SWAP;
    s_job_dest    = (uint16_t)(dir > 0 ? s_height + 1 : s_height - 1);
    s_job_running = true;
    s_job_ready   = true;
}

// THE CEILING. Would this UP carry the desk past DESK_CEILING_MM? A held UP
// runs on by the coast after it is let go, so it is refused a coast early. A
// tap is a fixed DESK_NUDGE_MM step that does not run on, so it is judged by
// where it ends — refusing taps a coast early stopped the desk at 78.2 cm
// under an 80 cm ceiling, with a whole tap's room left.
static bool ceiling_refuses(uint8_t code, int32_t mm)
{
    if (!s_ceil_on || !is_up(code) || mm < 0)
        return false;
    int32_t c = (int32_t)DESK_CEILING_MM;
    if (code == KEY_UP_E)
        return mm + DESK_NUDGE_MM > c;
    return mm >= c - (int32_t)s_coast_up;
}

static void ceiling_said(int32_t mm)
{
    static TickType_t said;
    led_refused();
    if (said && xTaskGetTickCount() - said < pdMS_TO_TICKS(3000))
        return;
    said = xTaskGetTickCount() ? xTaskGetTickCount() : 1;
    printf("[desk] UP refused at %ld mm — the ceiling is %u mm ('desk ceiling off' lifts it)\n",
           (long)mm, DESK_CEILING_MM);
}

static void learn(uint8_t key, uint16_t dest)
{
    int i = slot(key);
    if (i < 0 || dest < DESK_MIN_MM || dest > DESK_MAX_MM) return;
    if (s_preset_mm[i] != dest) {
        printf("[flap] preset %s: %u -> %u mm — stored\n",
               i ? "sit" : "stand", s_preset_mm[i], dest);
        s_preset_mm[i] = dest;
        settings_mark_dirty();
    } else if (!s_preset_known[i]) {
        printf("[flap] preset %s confirmed at %u mm — unchanged, nothing written\n",
               i ? "sit" : "stand", dest);
    }
    s_preset_known[i] = true;
}

static void begin_stop(int8_t dir, int32_t mm, bool self_driven)
{
    s_armed       = false;
    s_state       = ST_STOPPING;
    s_phase_start = xTaskGetTickCount();
    s_last_mm     = (uint16_t)(mm < 0 ? s_height : mm);
    s_last_change = s_phase_start;
    s_fired_dir   = dir;
    s_stop_tries  = self_driven ? 1 : 0;
}

// ---- the decision ---------------------------------------------------------

static uint8_t decide(uint8_t panel_code)
{
    int32_t mm  = height();
    int8_t  dir = desk_direction();

    track_tap(panel_code, mm);

    // End the hands-off window once the resumed move is over.
    if (s_hands_off) {
        uint32_t quiet = (uint32_t)((xTaskGetTickCount() - s_last_change) * portTICK_PERIOD_MS);
        if (mm >= 0 && (uint16_t)mm != s_last_mm) {
            s_last_mm = (uint16_t)mm;
            s_last_change = xTaskGetTickCount();
        } else if ((quiet >= PLAN_RESUME_QUIET_MS &&
                    xTaskGetTickCount() - s_hands_off_from >= pdMS_TO_TICKS(PLAN_RESUME_MIN_MS)) ||
                   xTaskGetTickCount() > s_hands_off_until) {
            s_hands_off = false;
            s_armed     = false;
        }
    }

    if (!s_armed && !s_hands_off && mm >= 0) {
        int32_t d = mm - (int32_t)s_height;
        if ((d < 0 ? -d : d) > FLAP_REARM_MM)
            s_armed = true;
    }

    switch (s_state) {

    case ST_MOVING:
        // The flap task owns the desk: the board hears whatever it has left
        // standing. The ceiling still applies — taking over is not permission
        // to exceed it.
        {
            uint8_t ours = wire_pending_key();
            if (ceiling_refuses(ours, mm)) {
                s_blocked++;
                ceiling_said(mm);
                return KEY_IDLE;        // not consumed: a held code stays held
            }
            return wire_next_key();
        }

    case ST_PLANNING: {
        // Waiting for the board to announce where the recall is going.
        uint16_t dest;
        if (desk_target_gen() == s_plan_gen || !desk_target_mm(&dest) ||
            dest == s_plan_from) {
            if (xTaskGetTickCount() - s_plan_started > pdMS_TO_TICKS(2500)) {
                s_state = ST_PASS;      // no announcement; nothing to plan on
                trace_add(TR_RECALL, s_job_key, TRR_PLAN_TIMEOUT);
            }
            return panel_code;
        }
        learn(s_job_key, dest);

        uint16_t lo = s_plan_from < dest ? s_plan_from : dest;
        uint16_t hi = s_plan_from < dest ? dest : s_plan_from;
        if (s_height <= lo || s_height >= hi) {
            s_state = ST_PASS;
            trace_add(TR_RECALL, s_job_key, TRR_PLAN_SAFE);
            return panel_code;          // does not cross; leave it alone
        }

        // It crosses. Stop it now, while the desk has barely started.
        trace_add(TR_RECALL, s_job_key, TRR_PLAN_CROSSES);
        s_job_dest    = dest;
        s_job_running = true;
        s_job_ready   = true;           // the flap starts with the stop
        begin_stop(dest > s_plan_from ? 1 : -1, mm, true);
        printf("[flap] recall heads for %u mm and crosses %u — stopping\n",
               dest, s_height);
        return stop_code(s_fired_dir);
    }

    case ST_STOPPING:
        // The panel is out of the picture until the sequence is done.
        //
        // Idle, not silence: idle is equally "no command" for a held move but
        // it is also a poll, so the board keeps answering and we keep seeing
        // the height. Going quiet blinds us exactly when we need to know
        // whether the desk is still moving.
        if (s_stop_frames) {
            s_stop_frames--;
            return s_stop_key;
        }
        if (settled(mm)) {
            if (s_job_running) {
                s_state = ST_MOVING;    // flap_task already has the job
            } else {
                s_state = ST_PASS;      // a held move: it has stopped, done
                printf("[flap] stopped at %ld mm\n", (long)mm);
            }
            return KEY_IDLE;
        }
        // Retry only while the desk is still moving BRISKLY. A tap that is
        // working shows as deceleration — the height starts repeating values
        // — and a second tap into a braking desk is another beep and another
        // ten millimetres.
        {
            uint32_t since = (uint32_t)((xTaskGetTickCount() - s_last_change) * portTICK_PERIOD_MS);
            if (since < FLAP_BRISK_MS &&
                xTaskGetTickCount() - s_phase_start >= pdMS_TO_TICKS(FLAP_STOP_PHASE_MS)) {
                s_phase_start = xTaskGetTickCount();
                if (++s_stop_tries <= FLAP_STOP_TRIES)
                    return stop_code(dir ? dir : s_fired_dir);
                printf("[flap] COULD NOT STOP the desk at %ld mm\n", (long)mm);
                s_state = ST_PASS;
                s_job_running = false;
            }
        }
        return KEY_IDLE;

    case ST_PASS:
    default:
        // A save code re-defines a preset: whatever height was cached for it
        // is wrong from this frame on. Forget it; the next press re-learns.
        if (panel_code == KEY_SAVE_ST || panel_code == KEY_SAVE_SI) {
            int i = (panel_code == KEY_SAVE_ST) ? 0 : 1;
            // The board's preset may have moved, so it is no longer trusted
            // for a takeover — but flash is left alone: the next recall
            // announces the height, and only a different one is written.
            if (s_preset_known[i]) {
                s_preset_known[i] = false;
                printf("[flap] preset %s re-saved on the panel — checked on its next press\n",
                       i ? "sit" : "stand");
            }
        }

        // The ceiling applies to whatever we are about to send, from any
        // source, and is the one rule that is never skipped.
        if (ceiling_refuses(panel_code, mm)) {
            s_blocked++;
            ceiling_said(mm);
            return KEY_IDLE;
        }

        // The ceiling above this line always applies; everything below it is
        // the flap, and the flap only runs when it is fit to. A board whose
        // encoder is missing, or which is being driven by hand in dev mode,
        // must not stop a move it cannot finish — see mode.h.
        if (!s_on || s_hands_off || s_job_running || !mode_flap_may_run()) {
            if (is_recall(panel_code))
                trace_add(TR_RECALL, panel_code,
                          !s_on ? TRR_FLAP_OFF : s_hands_off ? TRR_HANDS_OFF :
                          s_job_running ? TRR_JOB_RUNNING : TRR_NOT_FIT);
            return panel_code;
        }
        if (is_recall(panel_code) && mm < 0)
            trace_add(TR_RECALL, panel_code, TRR_NO_HEIGHT);

        if (is_recall(panel_code) && mm >= 0) {
            uint16_t known;
            if (flap_preset(panel_code, &known)) {
                // Destination already known, so the board never needs to hear
                // it. Nothing starts moving; nothing has to be stopped.
                uint16_t lo = (uint16_t)mm < known ? (uint16_t)mm : known;
                uint16_t hi = (uint16_t)mm < known ? known : (uint16_t)mm;
                int32_t h = (int32_t)s_height;
                bool at_and_up   = known > h && mm >= h && mm <= h + FLAP_SAFE_GAP_MM && needs_swap(1);
                bool at_and_down = known < h && mm <= h && mm >= h - FLAP_SAFE_GAP_MM && needs_swap(-1);
                if ((s_height > lo && s_height < hi) || at_and_up || at_and_down) {
                    s_job_key     = panel_code;
                    s_job_dest    = known;
                    s_job_running = true;
                    s_job_ready   = true;
                    s_armed       = false;
                    s_state       = ST_MOVING;
                    printf("[flap] recall to %u mm crosses %u — taking over before\n"
                           "       the board hears it\n", known, s_height);
                    trace_add(TR_RECALL, panel_code, TRR_TAKEOVER);
                    return KEY_IDLE;
                }
                trace_add(TR_RECALL, panel_code, TRR_SAFE);
                return panel_code;      // known and harmless
            }
            // First time for this preset: forward it, watch for the
            // announcement, and learn.
            s_job_key      = panel_code;
            s_plan_from    = (uint16_t)mm;
            s_plan_started = xTaskGetTickCount();
            s_plan_gen     = desk_target_gen();
            s_state        = ST_PLANNING;
            trace_add(TR_RECALL, panel_code, TRR_PLANNING);
            return panel_code;
        }

        // AT the flap height — within the safe gap past it — and asked to go
        // on with the flap not yet set for that side: move the flap here and
        // now. This is also the retry after a flap move that failed.
        {
            int8_t  kd = is_up(panel_code) ? 1 : is_down(panel_code) ? -1 : 0;
            int32_t h  = (int32_t)s_height;
            bool    at = kd > 0 ? (mm >= h && mm <= h + FLAP_SAFE_GAP_MM)
                                : (mm <= h && mm >= h - FLAP_SAFE_GAP_MM);
            if (kd && mm >= 0 && at && needs_swap(kd)) {
                s_armed    = false;
                s_tap_from = -1;
                start_swap(kd);
                s_state    = ST_MOVING;
                printf("[flap] at the flap height (%ld mm) with the flap not %s — moving it.\n"
                       "       Press again once it has moved.\n",
                       (long)mm, kd > 0 ? "collapsed" : "expanded");
                return KEY_IDLE;
            }
        }

        // A tap that would end past the flap height, with the flap still on
        // the wrong side for it: the tap is not sent (stopping one costs a
        // step back). The flap is moved instead, the desk standing where it
        // is, and the next tap finds the flap in place and goes through. If
        // the edge was the start of a HOLD, the held codes wait out the flap
        // move and then drive the desk on.
        if (edge_dir(panel_code) && mm >= 0 &&
            tap_crosses(edge_dir(panel_code), mm) && needs_swap(edge_dir(panel_code))) {
            s_armed    = false;
            s_tap_from = -1;
            start_swap(edge_dir(panel_code));
            s_state    = ST_MOVING;
            printf("[flap] a step from %ld mm would pass %u — moving the flap first.\n"
                   "       Press again once it has moved.\n", (long)mm, s_height);
            return KEY_IDLE;
        }

        if (needs_swap(dir) && approaching(dir, mm) && !tap_stays_short(dir)) {
            printf("[flap] desk approaching %u mm — stopping\n", s_height);
            begin_stop(dir, mm, !is_motion(panel_code));
            // A HELD key is driving it: stop, move the flap, hand the bus
            // back. Whoever is holding the key carries on from there.
            if (is_motion(panel_code)) {
                start_swap(dir);
                printf("[flap] a held move — the flap, then the panel has it back\n");
            }
            // Nobody is holding a key, so a RECALL is driving this move — one
            // that slipped past the takeover (hands-off, flap just switched
            // on, no height at the time). Stopping it and handing the bus back
            // would leave the desk parked at the flap height for good: make it
            // a flap job like any other, so the flap runs and the recall is
            // sent again to finish the move.
            if (!is_motion(panel_code) && s_drive_key) {
                uint16_t dest = 0;
                if (desk_target_gen() == s_drive_gen || !desk_target_mm(&dest))
                    flap_preset(s_drive_key, &dest);
                s_job_key     = s_drive_key;
                s_job_dest    = dest;
                s_job_running = true;
                s_job_ready   = true;   // the flap starts with the stop
                trace_add(TR_RECALL, s_drive_key, TRR_CAUGHT_LATE);
                printf("[flap] it is a recall to %u mm — the flap, then the recall again\n", dest);
            }
            if (is_motion(panel_code))
                return KEY_IDLE;        // a held move stops on idle
            return stop_code(dir);
        }
        return panel_code;              // the panel is in charge
    }
}

// The desk gate, applied to whatever is about to reach the board — the panel's
// code or our own — so nothing upstream can get round it. A move is refused by
// sending idle in its place. Not while STOPPING: those frames are the stop, and
// refusing them would leave a move that is already running to finish.
static volatile uint32_t s_refused;

// Learning a preset is only watching: the board announces where a recall is
// going, and that height is the preset. It changes nothing on the bus, so it
// runs on every recall that reaches the board — whether or not the intercept is
// on. Tied to the intercept, as it once was, nothing was learned in dev mode,
// with the flap off, or in the hands-off window after a flap move.
static uint8_t    s_watch_key;          // 0 = not watching
static uint16_t   s_watch_from;
static uint32_t   s_watch_gen;          // announcements before the recall
static TickType_t s_watch_at;

static void watch_recall(uint8_t sent)
{
    if (is_recall(sent)) {
        s_drive_key = sent;             // the board is now driving to a preset
        s_drive_gen = desk_target_gen();
    } else if (is_motion(sent))
        s_drive_key = 0;                // a key took over; the recall is cancelled
    if (is_recall(sent)) {
        int32_t mm = height();
        if (mm >= 0) {
            s_watch_key  = sent;
            s_watch_from = (uint16_t)mm;
            s_watch_gen  = desk_target_gen();
            s_watch_at   = xTaskGetTickCount();
        }
        return;
    }
    if (!s_watch_key)
        return;
    uint16_t dest;
    if (desk_target_gen() != s_watch_gen && desk_target_mm(&dest) &&
        dest != s_watch_from) {
        learn(s_watch_key, dest);
        s_watch_key = 0;
    } else if (xTaskGetTickCount() - s_watch_at > pdMS_TO_TICKS(2500)) {
        s_watch_key = 0;                // no announcement: already there
    }
}

// 'debug start': everything that steers a move, logged when it changes. Runs
// once per panel frame, after the decision, so it sees what the board was
// actually told. Each check is a compare; the event itself is a few stores.
static void trace_frame(uint8_t panel_code, uint8_t out)
{
    static uint8_t  last_panel = 0xFE, last_out = 0xFE;
    static int32_t  last_state = -1, last_mm = -2, last_target = -1;
    static int8_t   last_flags[4] = { -1, -1, -1, -1 };
    static uint32_t last_gen = 0xFFFFFFFFu;

    if (!trace_on()) {
        last_panel = last_out = 0xFE;   // a fresh 'debug start' logs a snapshot
        last_state = -1; last_mm = -2; last_target = -1; last_gen = 0xFFFFFFFFu;
        for (int i = 0; i < 4; i++) last_flags[i] = -1;
        return;
    }
    if (s_state != last_state) {
        trace_add(TR_STATE, last_state < 0 ? s_state : last_state, s_state);
        last_state = s_state;
    }
    if (panel_code != last_panel) { trace_add(TR_PANEL, panel_code, 0); last_panel = panel_code; }
    if (out != last_out)          { trace_add(TR_SENT,  out, 0);        last_out   = out; }

    int32_t mm = height();
    if (mm != last_mm) { trace_add(TR_HEIGHT, mm, desk_direction()); last_mm = mm; }

    uint16_t t;
    if (desk_target_gen() != last_gen) {    // each announcement, repeats included
        last_gen = desk_target_gen();
        int32_t target = desk_target_mm(&t) ? t : 0;
        if (target != last_target || last_target < 0) trace_add(TR_TARGET, target, 0);
        last_target = target;
    }

    int8_t flags[4] = { s_hands_off, s_armed, !mode_desk_may_move(),
                        s_on && !s_hands_off && !s_job_running && mode_flap_may_run() };
    for (int i = 0; i < 4; i++)
        if (flags[i] != last_flags[i]) {
            trace_add(TR_FLAG, i, flags[i]);
            last_flags[i] = flags[i];
        }
}

// When the desk last did anything: a move key or recall sent, or the height
// changing. settings.c will not write flash until it has been still for
// DESK_STILL_MS — flap_busy() alone misses the moves the BOARD drives (a
// recall passed through, the resumed move after a flap job).
#define DESK_STILL_MS   3000
static volatile TickType_t s_active_at;
static int32_t             s_active_mm = -1;

bool flap_desk_still(void)
{
    return s_state == ST_PASS && !s_job_running && desk_direction() == 0 &&
           xTaskGetTickCount() - s_active_at > pdMS_TO_TICKS(DESK_STILL_MS);
}

// THE CROSSING GUARD. The desk must never be above the flap height with the
// flap not collapsed — that is what breaks things. Normally the intercept sees
// to it: it stops the desk, moves the flap, and lets the desk on. But the
// intercept can be off (desk_flap_on 0), suspended (dev mode, the hands-off
// window) or simply not involved, and then nothing stood between the panel and
// a collision. So, like the ceiling, this is applied to whatever is about to
// reach the board: a key that would carry the desk up across the flap height
// while the flap is not collapsed is replaced with idle.
//
// Only UP is guarded. A collapsed flap below the flap height is out of the
// way, so going down is never refused here — and down is the way out when the
// desk is found above the flap height with the flap not collapsed, where every
// move further up is refused.
//
// Not while flap_task or the stop sequence owns the desk: those are the
// firmware bringing the desk TO the flap height in order to move the flap.
static volatile uint32_t s_cross_refused;

static bool crossing_refused(uint8_t out)
{
    if (!FLAP_DRIVES_MOTOR || s_state == ST_MOVING || s_state == ST_STOPPING)
        return false;
    int32_t mm = height(), h = (int32_t)s_height;
    if (mm < 0)
        return false;
    if (limits_at_end(true))            // collapsed: free to pass
        return false;
    if (mm >= h) {
        // Above the flap height with the flap not collapsed — however that
        // came about — and the intercept has not dealt with it (within the
        // safe gap it moves the flap; further up, or switched off, it cannot).
        // No higher: every UP is refused, and every recall not known to lead
        // down.
        if (is_up(out))
            return true;
        if (is_recall(out)) {
            int i = slot(out);
            return i < 0 || s_preset_mm[i] == 0 || (int32_t)s_preset_mm[i] > mm;
        }
        return false;
    }
    if (out == KEY_UP_E)                // a tap: only one that would end past it
        return tap_crosses(1, mm);
    if (is_up(out))                     // held: once it could no longer stop short
        return mm >= h - (int32_t)s_coast_up;
    if (is_recall(out) && s_state == ST_PASS) {
        // A recall the intercept is not handling. Known and headed above the
        // flap height: refused. Unknown: refused too — it cannot be judged.
        int i = slot(out);
        return i < 0 || s_preset_mm[i] == 0 || s_preset_mm[i] > h;
    }
    return false;
}

uint8_t flap_decide(uint8_t panel_code)
{
    uint8_t out = decide(panel_code);
    if (s_state != ST_STOPPING && (is_motion(out) || is_recall(out)) &&
        !mode_desk_may_move()) {
        s_refused++;
        led_refused();                  // the person at the panel sees why
        out = KEY_IDLE;
    }
    if (crossing_refused(out)) {
        static TickType_t said;
        if (!said || xTaskGetTickCount() - said > pdMS_TO_TICKS(3000)) {
            said = xTaskGetTickCount() ? xTaskGetTickCount() : 1;
            printf("[flap] REFUSED: going up %s %u mm with the flap not collapsed%s\n",
                   height() >= (int32_t)s_height ? "above" : "past", s_height,
                   s_on ? "" : " (desk_flap_on is 0, so the flap is not moved)");
        }
        s_cross_refused++;
        led_refused();
        out = KEY_IDLE;
    }
    watch_recall(out);                  // what the board actually hears
    int32_t mm_now = height();
    if (is_motion(out) || is_recall(out) || mm_now != s_active_mm) {
        s_active_at = xTaskGetTickCount();
        s_active_mm = mm_now;
    }
    trace_frame(panel_code, out);
    return out;
}

uint32_t flap_refused_moves(void) { return s_refused; }
uint32_t flap_refused_crossings(void) { return s_cross_refused; }

// ---- settings -------------------------------------------------------------

const char *flap_state_str(void)
{
    switch (s_state) {
        case ST_PASS:     return "pass-through";
        case ST_PLANNING: return "learning a preset";
        case ST_STOPPING: return "stopping the desk";
        case ST_MOVING:   return "moving / flap";
    }
    return "?";
}

bool flap_busy(void) { return s_state != ST_PASS; }
bool flap_desk_stopping(void) { return s_state == ST_STOPPING; }
bool flap_job_active(void)    { return s_job_running; }

void flap_set_height(uint16_t mm) { s_height = mm; s_armed = true; }
uint16_t flap_height(void)        { return s_height; }
bool flap_enabled(void)           { return s_on; }

bool flap_set_enabled(bool on)
{
    s_on = on;
    if (on) s_armed = true;
    return true;
}

void     flap_set_ceiling(bool on) { s_ceil_on = on; }
bool     flap_ceiling_on(void)     { return s_ceil_on; }
uint16_t flap_ceiling_mm(void)     { return DESK_CEILING_MM; }
uint32_t flap_blocked_ups(void)    { return s_blocked; }

void flap_coast(uint16_t *up, uint16_t *down)
{
    if (up) *up = s_coast_up;
    if (down) *down = s_coast_dn;
}

bool flap_set_coast(uint16_t up, uint16_t down)
{
    if (up > 200 || down > 200) return false;
    s_coast_up = up; s_coast_dn = down;
    return true;
}

bool flap_preset(uint8_t key, uint16_t *mm)
{
    int i = slot(key);
    if (i < 0 || !s_preset_known[i]) return false;
    if (mm) *mm = s_preset_mm[i];
    return true;
}

uint16_t flap_preset_stored(uint8_t key)
{
    int i = slot(key);
    return i < 0 ? 0 : s_preset_mm[i];
}

bool flap_set_preset(uint8_t key, uint16_t mm)
{
    int i = slot(key);
    if (i < 0) return false;
    if (mm && (mm < DESK_MIN_MM || mm > DESK_MAX_MM)) return false;
    s_preset_mm[i] = mm;
    s_preset_known[i] = (mm != 0);
    return true;
}

// ---- the job, run by flap_task --------------------------------------------

bool flap_job_take(uint8_t *key, uint16_t *dest)
{
    if (!s_job_ready) return false;
    s_job_ready = false;
    if (key)  *key  = s_job_key;
    if (dest) *dest = s_job_dest;
    return true;
}

bool flap_go(uint16_t mm)
{
    // The same hand-over a preset takeover makes, from the console instead of
    // the bus. Without it the bus stays in pass-through and forwards the
    // panel's idle, and whatever flap_task leaves pending is never sent.
    bool ok = false;
    taskENTER_CRITICAL();
    if (s_state == ST_PASS && !s_job_running) {
        s_job_key     = 0;              // nothing to re-issue: a plain move
        s_job_dest    = mm;
        s_job_running = true;
        s_job_ready   = true;
        s_state       = ST_MOVING;
        ok = true;
    }
    taskEXIT_CRITICAL();
    return ok;
}

void flap_job_done(void)
{
    if (!s_job_key) {
        // A 'go' crosses nothing on purpose, so there is no resumed move to
        // keep the intercept off for.
        s_job_running = false;
        s_state       = ST_PASS;
        return;
    }
    // Hands off until the resumed move is over: the desk has to cross the
    // flap height to reach its destination, which is the point.
    s_hands_off       = true;
    s_hands_off_from  = xTaskGetTickCount();
    s_hands_off_until = xTaskGetTickCount() + pdMS_TO_TICKS(PLAN_RESUME_MAX_MS);
    s_last_change     = xTaskGetTickCount();
    s_job_running     = false;
    s_armed           = false;
    s_state           = ST_PASS;
}
