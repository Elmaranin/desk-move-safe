//
// settings — see settings.h.
//
#include "settings.h"
#include "nvs.h"
#include "flap.h"
#include "stepper.h"
#include "board_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

// Bump on any layout change: nvs rejects a record of the wrong length, so an
// older one reads as "nothing stored" rather than as garbage.
#define SETTINGS_VERSION    1

static settings_t          s_live;
static volatile bool       s_dirty;
static volatile TickType_t s_dirty_at;
static bool                s_stored;

settings_t *settings(void) { return &s_live; }

static void gather(void)
{
    uint16_t mm;
    s_live.desk.preset_stand_mm = flap_preset(0x01, &mm) ? mm : 0;
    s_live.desk.preset_sit_mm   = flap_preset(0x02, &mm) ? mm : 0;
    flap_coast(&s_live.desk.coast_up_mm, &s_live.desk.coast_down_mm);
    s_live.desk.flap_height_mm = flap_height();
    s_live.desk.flap_on        = flap_enabled() ? 1 : 0;
}

static void apply(void)
{
    if (s_live.desk.preset_stand_mm) flap_set_preset(0x01, s_live.desk.preset_stand_mm);
    if (s_live.desk.preset_sit_mm)   flap_set_preset(0x02, s_live.desk.preset_sit_mm);
    flap_set_coast(s_live.desk.coast_up_mm, s_live.desk.coast_down_mm);
    flap_set_height(s_live.desk.flap_height_mm);
    flap_set_enabled(s_live.desk.flap_on != 0);
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
#endif
    s_stored = false;               // board_config.h defaults stand
}

void settings_mark_dirty(void)
{
    s_dirty = true;
    s_dirty_at = xTaskGetTickCount();
}

bool settings_stored(void) { return s_stored; }

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
    if (flap_busy() || stepper_busy())
        return;

    gather();
    s_live.version = SETTINGS_VERSION;
    s_dirty = false;
#if SETTINGS_ENABLE_FLASH
    if (nvs_write(&s_live, sizeof s_live)) {
        s_stored = true;
        printf("[settings] saved: stand %u, sit %u, coast %u/%u, flap %u (%s)\n",
               s_live.desk.preset_stand_mm, s_live.desk.preset_sit_mm,
               s_live.desk.coast_up_mm, s_live.desk.coast_down_mm,
               s_live.desk.flap_height_mm, s_live.desk.flap_on ? "on" : "off");
    }
#endif
}

void settings_forget(void)
{
#if SETTINGS_ENABLE_FLASH
    nvs_erase();
#endif
    memset(&s_live, 0, sizeof s_live);
    s_live.version = SETTINGS_VERSION;
    s_stored = false;
    s_dirty  = false;
}
