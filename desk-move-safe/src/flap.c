//
// flap — see flap.h.
//
#include "flap.h"
#include "bus.h"
#include "wire.h"
#include "mode.h"
#include "settings.h"
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

static void learn(uint8_t key, uint16_t dest)
{
    int i = slot(key);
    if (i < 0) return;
    if (!s_preset_known[i] || s_preset_mm[i] != dest) {
        s_preset_mm[i] = dest;
        s_preset_known[i] = true;
        settings_mark_dirty();
        printf("[flap] learned preset 0x%02X = %u mm\n", key, dest);
    }
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

uint8_t flap_decide(uint8_t panel_code)
{
    int32_t mm  = height();
    int8_t  dir = desk_direction();

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
            if (s_ceil_on && is_up(ours) && mm >= 0 &&
                mm >= (int32_t)DESK_CEILING_MM - (int32_t)s_coast_up) {
                s_blocked++;
                return KEY_IDLE;        // not consumed: a held code stays held
            }
            return wire_next_key();
        }

    case ST_PLANNING: {
        // Waiting for the board to announce where the recall is going.
        uint16_t dest;
        if (!desk_target_mm(&dest) || dest == s_plan_from) {
            if (xTaskGetTickCount() - s_plan_started > pdMS_TO_TICKS(2500))
                s_state = ST_PASS;      // no announcement; nothing to plan on
            return panel_code;
        }
        learn(s_job_key, dest);

        uint16_t lo = s_plan_from < dest ? s_plan_from : dest;
        uint16_t hi = s_plan_from < dest ? dest : s_plan_from;
        if (s_height <= lo || s_height >= hi) {
            s_state = ST_PASS;
            return panel_code;          // does not cross; leave it alone
        }

        // It crosses. Stop it now, while the desk has barely started.
        s_job_dest    = dest;
        s_job_running = true;
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
                s_job_ready = true;
                s_state     = ST_MOVING;
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
            if (s_preset_known[i]) {
                s_preset_known[i] = false;
                s_preset_mm[i]    = 0;
                settings_mark_dirty();
                printf("[flap] a preset was re-saved — its height will be re-learned\n");
            }
        }

        // The ceiling applies to whatever we are about to send, from any
        // source, and is the one rule that is never skipped.
        if (s_ceil_on && is_up(panel_code) && mm >= 0 &&
            mm >= (int32_t)DESK_CEILING_MM - (int32_t)s_coast_up) {
            s_blocked++;
            return KEY_IDLE;
        }

        // The ceiling above this line always applies; everything below it is
        // the flap, and the flap only runs when it is fit to. A board whose
        // encoder is missing, or which is being driven by hand in dev mode,
        // must not stop a move it cannot finish — see mode.h.
        if (!s_on || s_hands_off || s_job_running || !mode_flap_may_run())
            return panel_code;

        if (is_recall(panel_code) && mm >= 0) {
            uint16_t known;
            if (flap_preset(panel_code, &known)) {
                // Destination already known, so the board never needs to hear
                // it. Nothing starts moving; nothing has to be stopped.
                uint16_t lo = (uint16_t)mm < known ? (uint16_t)mm : known;
                uint16_t hi = (uint16_t)mm < known ? known : (uint16_t)mm;
                if (s_height > lo && s_height < hi) {
                    s_job_key     = panel_code;
                    s_job_dest    = known;
                    s_job_running = true;
                    s_job_ready   = true;
                    s_armed       = false;
                    s_state       = ST_MOVING;
                    printf("[flap] recall to %u mm crosses %u — taking over before\n"
                           "       the board hears it\n", known, s_height);
                    return KEY_IDLE;
                }
                return panel_code;      // known and harmless
            }
            // First time for this preset: forward it, watch for the
            // announcement, and learn.
            s_job_key      = panel_code;
            s_plan_from    = (uint16_t)mm;
            s_plan_started = xTaskGetTickCount();
            s_state        = ST_PLANNING;
            return panel_code;
        }

        if (s_armed && approaching(dir, mm)) {
            printf("[flap] desk approaching %u mm — stopping\n", s_height);
            begin_stop(dir, mm, !is_motion(panel_code));
            if (is_motion(panel_code))
                return KEY_IDLE;        // a held move stops on idle
            return stop_code(dir);
        }
        return panel_code;              // the panel is in charge
    }
}

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
