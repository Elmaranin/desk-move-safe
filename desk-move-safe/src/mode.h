#ifndef MODE_H
#define MODE_H
//
// mode — what this firmware is allowed to do, and whether it is fit to do it.
//
// WORKING MODE IS THE ONLY MODE A RESET CAN PRODUCE. This is a production
// board: it is bolted under a desk, it has no operator, and the thing it must
// do after a power cut is its job. So there is no stored mode, no jumper and no
// boot flag — dev mode is typed in by hand ('dev start') and is gone at the
// next reset. Deliberately NOT in settings.c: a mode that can be stored is a
// mode a board can be left in, and the board that gets left in it is always the
// one nobody is watching.
//
// THE SELF-CHECK. Coming up in working mode is not a claim that the hardware
// works, it is a question asked at boot and answerable again at any time:
//
//   driver    can the stepper driver be reached (only askable over UART)
//   encoder   does the MT6835 answer
//   limits    are min and max stored in flash
//
// A check can also be ABSENT, which is not a failure: it is a subsystem this
// build does not contain yet. That distinction is the point — "not built" and
// "broken" need different reactions, and collapsing them into one bool is how a
// firmware ends up refusing to work because of a feature nobody has written.
//
// NOT FIT MEANS PASS-THROUGH, NOT AN ERROR. If the flap cannot run, the desk
// must still be a desk. Stopping a move at the flap height and then failing to
// move the flap is strictly worse than never stopping it: the user gets an
// interrupted move and a beep in exchange for nothing. So a failed check
// switches the intercept OFF and says why, and the panel drives the desk
// exactly as it did before this board was fitted.
//
// DEV MODE ALSO SUSPENDS THE INTERCEPT, for the same reason from the other
// side: someone hand-driving the motor must not have the desk take the bus out
// from under them because a preset was pressed in the next room.
//
#include <stdbool.h>

// ABSENT is not a failure. See above.
typedef enum { CHK_OK = 0, CHK_FAIL, CHK_ABSENT } chk_t;

typedef struct {
    chk_t driver;
    chk_t encoder;
    chk_t limits;
} selfcheck_t;

// Run the checks. Reads flags and cached state only — no bus traffic, nothing
// that blocks — so flap_decide() can ask on every panel frame.
void mode_check(selfcheck_t *out);

// May the flap take the bus and run? Working mode, not dev, and no check that
// matters for what run_flap() actually does has FAILed.
bool mode_flap_may_run(void);

// One line saying why not, for the console and the boot log. Empty if it may.
const char *mode_flap_blocked_by(void);

// The self-check, one line per item.
void mode_report(void);

bool mode_dev(void);

// Entering is loud and refuses while the desk half is mid-sequence; leaving
// stops the motor and releases it. Returns false if it refused.
bool mode_set_dev(bool on);

#endif // MODE_H
