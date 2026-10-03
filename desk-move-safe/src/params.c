//
// params — see params.h.
//
#include "params.h"
#include "settings.h"
#include "flap.h"
#include "limits.h"
#include "encoder.h"
#include "stepper.h"
#include "mode.h"
#include "board_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- getters and setters --------------------------------------------------
// Desk and panel values live in flap.c while the firmware runs (settings.c
// gathers them at write time), so they are read and written there. The limits
// live in the record itself.

static double preset(uint8_t key) { return flap_preset_stored(key); }
static double get_stand(void)     { return preset(0x01); }
static double get_sit(void)       { return preset(0x02); }

static double coast(bool up)      { uint16_t u, d; flap_coast(&u, &d); return up ? u : d; }
static double get_coast_up(void)  { return coast(true); }
static double get_coast_dn(void)  { return coast(false); }
static double get_flap_mm(void)   { return flap_height(); }
static double get_flap_on(void)   { return flap_enabled() ? 1 : 0; }

static double get_version(void)   { return settings()->version; }
static double get_flags(void)     { return settings()->lim.flags; }
static double get_expanded_raw(void)   { return settings()->lim.expanded_raw; }
static double get_collapsed_raw(void)   { return settings()->lim.collapsed_raw; }
static double get_enc_dir(void)   { return settings()->lim.enc_dir; }
static double get_mot_dir(void)   { return settings()->lim.mot_dir; }
static double get_backlash(void)  { return settings()->lim.backlash; }
static double get_approach(void)  { return settings()->lim.approach_deg; }

static bool set_preset(uint8_t key, double v)
{
    if (!flap_set_preset(key, (uint16_t)v)) {
        printf("rejected: 0 (forget, re-learn on the next press) or %u..%u mm\n",
               DESK_MIN_MM, DESK_MAX_MM);
        return false;
    }
    settings_mark_dirty();
    return true;
}
static bool set_stand(double v) { return set_preset(0x01, v); }
static bool set_sit(double v)   { return set_preset(0x02, v); }

static bool set_coast(bool up, double v)
{
    uint16_t u, d;
    flap_coast(&u, &d);
    if (!flap_set_coast(up ? (uint16_t)v : u, up ? d : (uint16_t)v)) {
        printf("rejected: coast is how far the desk runs on after being told to\n"
               "stop — it need not be exact; within %u mm of the mark is close enough\n",
               DESK_NEAR_MM);
        return false;
    }
    settings_mark_dirty();
    return true;
}
static bool set_coast_up(double v) { return set_coast(true, v); }
static bool set_coast_dn(double v) { return set_coast(false, v); }

static bool set_flap_mm(double v)
{
    const unsigned lo = DESK_MIN_MM + DESK_NEAR_MM, hi = DESK_CEILING_MM - DESK_NEAR_MM;
    if (v < lo || v > hi) {
        printf("rejected: %u..%u mm — inside the travel, below the ceiling\n", lo, hi);
        return false;
    }
    flap_set_height((uint16_t)v);
    settings_mark_dirty();
    return true;
}

static bool set_flap_on(double v)
{
    if (v != 0 && v != 1) { printf("rejected: 0 or 1\n"); return false; }
    flap_set_enabled(v != 0);
    settings_mark_dirty();
    return true;
}

static double get_speed(void) { return stepper_speed(); }

// The rig measured the motor stable to 15000 microsteps/s unloaded at 1/8;
// below 100 a move of the flap's travel takes minutes.
static bool set_speed(double v)
{
    if (v < 100 || v > 15000) {
        printf("rejected: 100..15000 microsteps/s — the motor was measured stable to\n"
               "15000 unloaded; the flap's load wants a margin under that\n");
        return false;
    }
    if (stepper_busy()) { printf("not while it is moving\n"); return false; }
    stepper_set_speed((uint32_t)v);
    settings_mark_dirty();
    return true;
}

static double get_early(void) { return flap_early_start() ? 1 : 0; }
static bool set_early(double v)
{
    if (v != 0 && v != 1) { printf("rejected: 0 or 1\n"); return false; }
    flap_set_early_start(v != 0);
    settings_mark_dirty();
    return true;
}

static double get_resume(void) { return flap_resume_pct(); }
static bool set_resume(double v)
{
    if (v < 1 || v > 100) {
        printf("rejected: 1..100 — the share of the flap's move, by the encoder, after\n"
               "which the desk goes on. 100: once the flap has settled on its end.\n");
        return false;
    }
    flap_set_resume_pct((uint8_t)v);
    settings_mark_dirty();
    return true;
}

static bool set_backlash(double v) { return limits_set_play((int32_t)v); }
static bool set_approach(double v) { return limits_set_approach(v); }

// ---- the table ------------------------------------------------------------

typedef struct {
    const char *name;
    const char *unit;
    int         decimals;
    bool        dev;            // 'eeprom set' needs dev mode
    double    (*get)(void);
    bool      (*set)(double);   // NULL: read-only here
    const char *how;            // read-only: what changes it
} param_t;

static const param_t P[] = {
    { "version",            "",          0, false, get_version,  NULL,         "the firmware's record layout" },

    { "panel_stand_mm",     "mm",        0, false, get_stand,    set_stand,    NULL },
    { "panel_sit_mm",       "mm",        0, false, get_sit,      set_sit,      NULL },

    { "desk_coast_up_mm",   "mm",        0, false, get_coast_up, set_coast_up, NULL },
    { "desk_coast_down_mm", "mm",        0, false, get_coast_dn, set_coast_dn, NULL },
    { "desk_flap_mm",       "mm",        0, false, get_flap_mm,  set_flap_mm,  NULL },
    { "desk_flap_on",       "0|1",       0, false, get_flap_on,  set_flap_on,  NULL },
    { "desk_resume_pct",    "%",         0, true,  get_resume,   set_resume,   NULL },

    { "lim_flags",          "bits",      0, true,  get_flags,    NULL,         "'lim expanded', 'lim collapsed', 'reset'" },
    { "lim_expanded_raw",        "raw",       0, true,  get_expanded_raw,  NULL,         "'lim expanded'; 'reset' clears it" },
    { "lim_collapsed_raw",        "raw",       0, true,  get_collapsed_raw,  NULL,         "'lim collapsed'; 'reset' clears it" },
    { "lim_enc_dir",        "+1|-1",     0, true,  get_enc_dir,  NULL,         "'calibrate'; 'reset' clears it" },
    { "lim_mot_dir",        "+1|-1",     0, true,  get_mot_dir,  NULL,         "'calibrate' (learned from the motor); 'reset' clears it" },
    { "lim_backlash",       "microsteps",0, true,  get_backlash, set_backlash, NULL },
    { "lim_approach_deg",   "deg",       1, true,  get_approach, set_approach, NULL },

    { "mot_speed_sps",      "steps/s",   0, true,  get_speed,    set_speed,    NULL },
    { "mot_early_start",    "0|1",       0, true,  get_early,    set_early,    NULL },
};
#define N_PARAMS (sizeof P / sizeof P[0])

static const param_t *find(const char *name)
{
    for (size_t i = 0; i < N_PARAMS; i++)
        if (!strcmp(P[i].name, name))
            return &P[i];
    return NULL;
}

static void line(const param_t *p)
{
    double v = p->get();
    printf("  %-20s %12.*f %-10s", p->name, p->decimals, v, p->unit);
    if (!strcmp(p->unit, "raw"))
        printf(" (%.3f deg)", v * 360.0 / ENCODER_CPR);
    else if (!strcmp(p->name, "lim_flags"))
        printf(" (%s, %s)", (unsigned)v & 1u ? "expanded stored" : "expanded missing",
               (unsigned)v & 2u ? "collapsed stored" : "collapsed missing");
    else if (strstr(p->name, "panel_") == p->name && v == 0)
        printf(" (not known yet)");
    if (!p->set)
        printf("  read-only");
    printf("\n");
}

void params_print_all(void)
{
    printf("record    %s | %u bytes in the last flash sector\n",
           !settings_stored() ? "NOTHING IN FLASH YET — defaults"
           : settings_dirty() ? "changed, written once idle"
                              : "matches flash",
           (unsigned)sizeof(settings_t));
    const char *section = "";
    for (size_t i = 0; i < N_PARAMS; i++) {
        // A blank line between sections: the prefix up to the first '_'.
        size_t n = strcspn(P[i].name, "_");
        if (strncmp(section, P[i].name, n) || section[n] != '_') {
            printf("\n");
            section = P[i].name;
        }
        line(&P[i]);
    }
    printf("\n'eeprom <name>' for one, 'eeprom set <name> <value>' to change one.\n");
}

bool params_print(const char *name)
{
    const param_t *p = find(name);
    if (!p) return false;
    line(p);
    return true;
}

void params_set(const char *name, const char *value)
{
    const param_t *p = find(name);
    if (!p) {
        printf("no parameter '%s' — 'eeprom' lists them\n", name);
        return;
    }
    if (!p->set) {
        printf("%s is read-only here — set by %s\n", p->name, p->how);
        return;
    }
    if (p->dev && !mode_dev()) {
        printf("%s is a development setting — 'dev start' first\n", p->name);
        return;
    }
    char  *end;
    double v = strtod(value, &end);
    if (end == value || *end || (p->decimals == 0 && v != (double)(long)v)) {
        printf("usage: eeprom set %s <%s>\n", p->name,
               p->decimals ? "number" : "whole number");
        return;
    }
    if (p->set(v))
        line(p);
}
