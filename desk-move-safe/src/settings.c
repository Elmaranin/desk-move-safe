//
// settings — see settings.h.
//
#include "settings.h"
#include "nvs.h"
#include "flap.h"
#include "stepper.h"
#include <stddef.h>
#include "board_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

// Bump on any layout change: nvs rejects a record of the wrong length, so an
// older one reads as "nothing stored" rather than as garbage.
#define SETTINGS_VERSION    2

// Version 1 was everything before `mot`. Read as such, it keeps its values and
// the new fields take their defaults — a firmware update must not throw away
// the flap calibration, which would lock the desk until 'init' is run again.
#define SETTINGS_V1_SIZE    offsetof(settings_t, mot)

static settings_t          s_live;
static volatile bool       s_dirty;
static volatile TickType_t s_dirty_at;
static bool                s_stored;

settings_t *settings(void) { return &s_live; }

static void gather(void)
{
    // The stored value, trusted or not: a preset re-saved on the panel keeps
    // its old height in flash until a recall shows a different one.
    s_live.panel.stand_mm = flap_preset_stored(0x01);
    s_live.panel.sit_mm   = flap_preset_stored(0x02);
    flap_coast(&s_live.desk.coast_up_mm, &s_live.desk.coast_down_mm);
    s_live.desk.flap_mm = flap_height();
    s_live.desk.flap_on = flap_enabled() ? 1 : 0;
    s_live.mot.speed_sps = stepper_speed();
}

static void apply(void)
{
    if (s_live.panel.stand_mm) flap_set_preset(0x01, s_live.panel.stand_mm);
    if (s_live.panel.sit_mm)   flap_set_preset(0x02, s_live.panel.sit_mm);
    flap_set_coast(s_live.desk.coast_up_mm, s_live.desk.coast_down_mm);
    flap_set_height(s_live.desk.flap_mm);
    flap_set_enabled(s_live.desk.flap_on != 0);
    if (s_live.mot.speed_sps) stepper_set_speed(s_live.mot.speed_sps);
}

void settings_load(void)
{
    memset(&s_live, 0, sizeof s_live);
    s_live.version = SETTINGS_VERSION;
#if SETTINGS_ENABLE_FLASH
    settings_t s;
    if (nvs_read(&s, sizeof s) && s.version == SETTINGS_VERSION) {
        s_live   = s;
        s_stored = true;
        apply();
        return;
    }
    if (nvs_read(&s, SETTINGS_V1_SIZE) && s.version == 1) {
        memcpy(&s_live, &s, SETTINGS_V1_SIZE);  // mot stays zero: defaults
        s_live.version = SETTINGS_VERSION;
        s_stored = true;
        apply();
        settings_mark_dirty();                  // rewritten in the new layout
        printf("[settings] record from the previous layout — kept, new fields default\n");
        return;
    }
#endif
    s_stored = false;               // board_config.h defaults stand
}

void settings_mark_dirty(void)
{
    s_dirty = true;
    s_dirty_at = xTaskGetTickCount();
}

bool settings_stored(void) { return s_stored; }
bool settings_dirty(void)  { return s_dirty; }

void settings_flush(void)
{
    if (!s_dirty)
        return;
    // Let the person finish typing first. The write blanks interrupts for up
    // to ~100 ms, and opening that window right after a command is how a
    // BOOTSEL request arriving at the same moment gets lost. It also collapses
    // a burst of tuning commands into one write, which the sector's erase
    // budget appreciates.
    if (xTaskGetTickCount() - s_dirty_at < pdMS_TO_TICKS(SETTINGS_QUIET_MS))
        return;
    // Never mid-move, and that means EITHER move. flap_busy() is the desk's
    // state machine; stepper_busy() is the motor — and the motor is the one
    // that cannot survive this. The write blanks interrupts for tens of
    // milliseconds, which at 5000 microsteps/s is hundreds of missed steps:
    // the pulse train stops dead, the rotor carries on, and when the alarm
    // finally runs again it resumes at full speed against a rotor that is no
    // longer where the step count says. That is a stall, and it looks exactly
    // like a motor that was asked to go too fast.
    //
    // And "the desk" means the desk, not our state machine: a recall passed
    // through, or the move resumed after a flap job, is the board driving it
    // while flap_busy() says idle. A write then cost frames mid-move.
    if (!flap_desk_still() || stepper_busy())
        return;

    gather();
    s_live.version = SETTINGS_VERSION;
    s_dirty = false;
#if SETTINGS_ENABLE_FLASH
    if (nvs_write(&s_live, sizeof s_live)) {
        s_stored = true;
        printf("[settings] saved: stand %u, sit %u, coast %u/%u, flap %u (%s), speed %lu, limits %s\n",
               s_live.panel.stand_mm, s_live.panel.sit_mm,
               s_live.desk.coast_up_mm, s_live.desk.coast_down_mm,
               s_live.desk.flap_mm, s_live.desk.flap_on ? "on" : "off",
               (unsigned long)s_live.mot.speed_sps,
               (s_live.lim.flags & 3u) == 3u ? "stored" : s_live.lim.flags ? "one end" : "none");
    }
#endif
}
