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
// only thing that may write it. When the stepper arrives, its stored values go
// in the `flap` section below and it marks the settings dirty like everything
// else — it must NOT call nvs_write() for a record of its own, or the two
// would take turns erasing each other.
//
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t preset_stand_mm;   // 0 = not known yet
    uint16_t preset_sit_mm;
    uint16_t coast_up_mm;
    uint16_t coast_down_mm;
    uint16_t flap_height_mm;
    uint16_t flap_on;
} settings_desk_t;

typedef struct {
    // The bench rig's limits.c record, field for field, so the port is a copy.
    // All zero until the stepper is wired.
    uint32_t flags;
    uint32_t min_raw, max_raw;
    int32_t  enc_span, span_steps;
    uint32_t steps_per_rev;
    int32_t  backlash;
    float    approach_deg;
} settings_flap_t;

typedef struct {
    uint32_t        version;
    settings_desk_t desk;
    settings_flap_t flap;
} settings_t;

settings_t *settings(void);     // owners read and write their own section

void settings_load(void);       // at boot, before the bus runs
void settings_mark_dirty(void); // from anywhere
void settings_flush(void);      // console task: writes if dirty and idle
bool settings_stored(void);     // was there a valid record at boot?
void settings_forget(void);     // erase, and fall back to board_config.h

#endif // SETTINGS_H
