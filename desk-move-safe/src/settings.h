#ifndef SETTINGS_H
#define SETTINGS_H
//
// settings — the numbers that are learned or tuned at the desk and would
// otherwise be lost at every reset.
//
// Kept in the last flash sector through nvs. Loaded once at boot and pushed
// into flap.c; saved once they have been left alone for a moment. A flash
// write runs with interrupts off for tens of milliseconds, so the bus task
// only ever MARKS them dirty and the console task does the write — never
// mid-move, and never straight after a keystroke.
//
// ONE RECORD, ONE OWNER. nvs keeps exactly one record and this module is the
// only thing that may write it. limits.c keeps the flap's range in the `lim`
// section below and marks the settings dirty like everything else — it must
// NOT call nvs_write() for a record of its own, or the two would take turns
// erasing each other.
//
// SECTION = PREFIX. Each section is named after the console group it belongs
// to, and params.c shows every field as <section>_<field>: panel_stand_mm,
// desk_coast_up_mm, lim_min_raw. The layout is unchanged from version 1 — the
// panel/desk split moved no bytes — so a stored record still reads.
//
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t stand_mm;          // the panel's preset heights; 0 = not known yet
    uint16_t sit_mm;
} settings_panel_t;

typedef struct {
    uint16_t coast_up_mm;       // run-on after being told to stop
    uint16_t coast_down_mm;
    uint16_t flap_mm;           // where the desk is stopped for the flap
    uint16_t flap_on;
} settings_desk_t;

typedef struct {
    // The bench rig's limits.c record, field for field. All zero until
    // 'lim min' and 'lim max' are stored.
    uint32_t flags;
    uint32_t expanded_raw, collapsed_raw;
    int32_t  enc_span, span_steps;
    uint32_t steps_per_rev;
    int32_t  backlash;
    float    approach_deg;
} settings_lim_t;

typedef struct {
    uint32_t speed_sps;         // cruise rate, microsteps/s; 0 = board_config.h
    uint32_t early_start;       // version 3: 1 = the flap starts as the desk is
                                // told to stop, 0 = once it has stopped
} settings_mot_t;

typedef struct {
    uint32_t         version;
    settings_panel_t panel;
    settings_desk_t  desk;
    settings_lim_t   lim;
    settings_mot_t   mot;       // version 2. New fields go at the END, so
                                // an older record is this one, truncated.
    uint32_t         desk_early_resume; // version 4. A desk_* value, but here:
                                        // the desk section cannot grow in place.
} settings_t;

settings_t *settings(void);     // owners read and write their own section

void settings_load(void);       // at boot, before the bus runs
void settings_mark_dirty(void); // from anywhere
void settings_flush(void);      // console task: writes if dirty and idle
bool settings_stored(void);     // was there a valid record at boot?
bool settings_dirty(void);      // changed since the last write

#endif // SETTINGS_H
