#ifndef FLAP_H
#define FLAP_H
//
// flap — deciding what the main board is told.
//
// Every key frame the panel sends arrives here instead of at the board, and
// this decides what the board hears: normally the panel's own code, verbatim.
// Two things change that.
//
//   THE CEILING. An UP command that would carry the desk past the safe limit
//   is replaced with idle. Being in the path is what makes that a guarantee
//   rather than a request: it holds against a button held down, and against
//   any other part of this firmware getting it wrong.
//
//   THE FLAP HEIGHT. A move that would carry the desk past it is stopped
//   there, the flap runs, and only then does the move continue.
//
// HOW A PRESET IS HANDLED, AND WHY IT MATTERS. A recall announces its
// destination about a second BEFORE the desk moves, so the whole move is known
// in advance — there is no need to watch the desk approach and try to stop it
// in time. Once a preset's height has been seen, the recall is not forwarded
// at all: the board never starts, there is nothing to abort, and nothing can
// overshoot a height it has not begun travelling towards. The move is then
// driven here, and finished by re-issuing the recall so the board lands on its
// own stored height exactly.
//
// A HELD move has no announcement — nobody knows where the user will let go —
// so that one still has to be stopped as it approaches.
//
// Decisions are made per frame, ~10 times a second, from the bus task. Nothing
// here blocks or prints.
//
#include <stdbool.h>
#include <stdint.h>

void flap_task(void *arg);      // runs the stop-flap-continue sequence

// Decide what the board should be told, given what the panel just said.
// Returns a key code, or WIRE_NO_FRAME.
uint8_t flap_decide(uint8_t panel_code);

// Moves refused because mode_desk_may_move() said no. See mode.h.
uint32_t flap_refused_moves(void);

// Handed to flap_task: stop the desk at the flap height, run the flap, then
// finish the move by re-issuing this recall.
bool flap_job_take(uint8_t *preset_key, uint16_t *destination_mm);
void flap_job_done(void);

// Hand flap_task a plain move to `mm` — the console's 'go'. The job's key is
// 0: no flap, no recall afterwards. False if the bus is already taken.
bool flap_go(uint16_t mm);

const char *flap_state_str(void);
bool        flap_busy(void);

// The desk is at rest: nothing driving it, the height unchanged and no move
// key sent for a few seconds — whoever started the move, us or the board.
// The only time settings.c may write flash.
bool        flap_desk_still(void);

// ---- settings (all persisted) ---------------------------------------------

void     flap_set_height(uint16_t mm);      // where the desk is stopped
uint16_t flap_height(void);
bool     flap_set_enabled(bool on);
bool     flap_enabled(void);

void     flap_set_ceiling(bool on);
bool     flap_ceiling_on(void);
uint16_t flap_ceiling_mm(void);
uint32_t flap_blocked_ups(void);

void     flap_coast(uint16_t *up, uint16_t *down);
bool     flap_set_coast(uint16_t up, uint16_t down);

// What a preset is worth. Learned from the board's announcements; setting
// them by hand means even the first press is clean. 0 forgets one.
bool flap_preset(uint8_t key, uint16_t *mm);        // key: 0x01 stand, 0x02 sit
uint16_t flap_preset_stored(uint8_t key);           // as in flash, trusted or not
bool flap_set_preset(uint8_t key, uint16_t mm);

#endif // FLAP_H
