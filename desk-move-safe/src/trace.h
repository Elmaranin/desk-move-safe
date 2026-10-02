#ifndef TRACE_H
#define TRACE_H
//
// trace — 'debug start': a timestamped record of what the desk side decides.
//
// Built for the question "why did it stop there?". Everything that steers a
// move is logged when it CHANGES: the flap state machine, the key the panel
// sends and the key the board actually hears, the height, the destination the
// board announces, the hands-off / armed / locked flags, and each phase of a
// flap job. Unchanged frames are not logged, so a quiet desk prints nothing.
//
// The bus task cannot print: it has a hard deadline, and a printf to a USB
// host that has stopped reading can block for half a second. So events go into
// a ring buffer (a few integer stores) and the console task prints them. If the
// buffer fills faster than the console drains it, the overflow is counted and
// reported, never silently lost.
//
// Session only, off at every boot, and read-only: tracing changes nothing the
// firmware does, so it is available in working mode.
//
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TR_STATE,       // a = from, b = to (flap.c state_t)
    TR_PANEL,       // a = key the panel sent
    TR_SENT,        // a = key the board heard (what we forwarded or replaced)
    TR_HEIGHT,      // a = mm, b = direction (-1, 0, 1)
    TR_TARGET,      // a = mm the board announced, 0 = none
    TR_FLAG,        // a = which (TRF_*), b = new value
    TR_JOB,         // a = phase (TRJ_*), b = detail
    TR_RECALL,      // a = recall key, b = what was decided (TRR_*)
} tr_type_t;

// What became of a preset recall, and why.
enum {
    TRR_TAKEOVER,       // known destination crosses the flap: taken over
    TRR_SAFE,           // known destination does not cross: passed through
    TRR_PLANNING,       // destination unknown: passed, waiting for the board
    TRR_PLAN_CROSSES,   // ... announced, crosses: stopping it
    TRR_PLAN_SAFE,      // ... announced, does not cross
    TRR_PLAN_TIMEOUT,   // ... nothing announced in time
    TRR_FLAP_OFF,       // passed: desk_flap_on is 0
    TRR_HANDS_OFF,      // passed: hands-off after a flap move
    TRR_JOB_RUNNING,    // passed: a flap job already owns the desk
    TRR_NOT_FIT,        // passed: dev mode, or the desk/flap check failed
    TRR_NO_HEIGHT,      // passed: no fresh height to judge it by
    TRR_CAUGHT_LATE,    // a recall-driven move caught by the approach stop: flap job
};

enum { TRF_HANDS_OFF, TRF_ARMED, TRF_DESK_LOCKED, TRF_INTERCEPT };

enum {
    TRJ_TAKE,       // b = destination mm
    TRJ_GO,         // b = destination mm ('go')
    TRJ_AT_FLAP,    // b = 1 reached the flap height, 0 failed
    TRJ_FLAP_START,
    TRJ_FLAP_END,
    TRJ_RESUME,     // b = recall key re-sent
    TRJ_SENT,       // b = 1 left on the wire, 0 timed out
    TRJ_ABORTED,
    TRJ_DONE,
};

void trace_set(bool on);
bool trace_on(void);
void trace_add(tr_type_t type, int32_t a, int32_t b);  // any task; no-op when off
void trace_drain(void);                                 // console task only
void trace_report(void);                                // 'debug'

#endif // TRACE_H
