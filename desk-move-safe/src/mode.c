//
// mode — see mode.h.
//
#include "mode.h"
#include "settings.h"
#include "stepper.h"
#include "tmc2209.h"
#include "encoder.h"
#include "flap.h"
#include "limits.h"
#include "board_config.h"

#include <stdio.h>

// RAM only, and it has no initialiser on purpose: BSS is zeroed at reset, so
// "working mode" is what a reset produces without anything having to run.
static bool s_dev;

void mode_check(selfcheck_t *c)
{
    // THE DRIVER. Only a question if there is a link to ask it over. In
    // standalone STEP/DIR/EN the carrier either works or the motor does not
    // turn, and no wire reports which — so ABSENT is the honest answer, not OK.
    // Claiming OK here would be the firmware vouching for something it has no
    // way to know.
    if (TMC_UART_ENABLED)
        c->driver = tmc2209_link_ok() ? CHK_OK : CHK_FAIL;
    else
        c->driver = CHK_ABSENT;

    // THE ENCODER. A real question now: encoder_task answers the sensor on SPI
    // and keeps retrying, so this is "has it ever replied with a frame whose CRC
    // checked". Transiently FAIL for the first few milliseconds after a reset,
    // which locks the desk for those few milliseconds — harmless.
    c->encoder = encoder_available() ? CHK_OK : CHK_FAIL;

    // THE TRAVEL LIMITS. Both ends stored in the settings record.
    c->limits = limits_calibrated() ? CHK_OK : CHK_FAIL;
}

// The desk gate. The flap is mounted where the desk travels, and a flap left
// open is in the way: until the firmware can see the flap (encoder) and knows
// where its ends are (limits), it cannot know the desk is safe to move. So it
// refuses every move instead — panel included. A stuck desk is the price, and
// it is the cheaper failure. The driver is not part of this: in standalone mode
// it cannot be asked.
bool mode_desk_may_move(void)
{
    return *mode_desk_blocked_by() == '\0';
}

const char *mode_desk_blocked_by(void)
{
    selfcheck_t c;
    mode_check(&c);
    if (c.encoder == CHK_FAIL) return "the MT6835 does not answer — the flap cannot be seen";
    if (c.limits  == CHK_FAIL) return "the flap's expanded and collapsed ends are not stored";
    return "";
}

// What the flap has to be fit FOR depends on what it actually does.
//
// The flap move drives the stepper to an end (FLAP_DRIVES_MOTOR 1), so the driver,
// the encoder and the stored ends are all preconditions. With FLAP_DRIVES_MOTOR
// 0 the flap is a dwell that touches no motor, and only dev mode stops it.
bool mode_flap_may_run(void)
{
    if (s_dev)
        return false;
    if (!mode_desk_may_move())
        return false;           // a takeover would only stall against the lock
    if (!FLAP_DRIVES_MOTOR)
        return true;

    selfcheck_t c;
    mode_check(&c);
    return c.driver  != CHK_FAIL &&
           c.encoder != CHK_FAIL &&
           c.limits  != CHK_FAIL;
}

const char *mode_flap_blocked_by(void)
{
    if (s_dev)
        return "dev mode — the intercept is suspended while the motor is "
               "being driven by hand";
    if (!mode_desk_may_move())
        return mode_desk_blocked_by();
    if (!FLAP_DRIVES_MOTOR)
        return "";

    selfcheck_t c;
    mode_check(&c);
    if (c.driver  == CHK_FAIL) return "the stepper driver does not answer";
    if (c.encoder == CHK_FAIL) return "the MT6835 does not answer";
    if (c.limits  == CHK_FAIL) return "the flap's ends are not stored — 'calibrate' calibrates them";
    return "";
}

static const char *chk_str(chk_t c)
{
    switch (c) {
        case CHK_OK:     return "ok";
        case CHK_FAIL:   return "FAILED";
        case CHK_ABSENT: return "not in this build";
    }
    return "?";
}

void mode_report(void)
{
    selfcheck_t c;
    mode_check(&c);

    printf("mode      %s\n", s_dev ? "DEV — everything unlocked" : "working");
    printf("check     driver %s | encoder %s | limits %s\n",
           chk_str(c.driver), chk_str(c.encoder), chk_str(c.limits));

    if (!FLAP_DRIVES_MOTOR)
        printf("          the flap is still a dwell, so none of these gate it.\n"
               "          FLAP_DRIVES_MOTOR makes them requirements.\n");

    const char *desk = mode_desk_blocked_by();
    if (*desk)
        printf("          DESK LOCKED: %s.\n"
               "          Every move is refused, panel included (%lu so far).\n"
               "          'calibrate' calibrates the flap.\n",
               desk, (unsigned long)flap_refused_moves());

    const char *why = mode_flap_blocked_by();
    if (*why && why != desk)        // the same reason is not worth saying twice
        printf("          FLAP OFF: %s.\n", why);
}

bool mode_dev(void) { return s_dev; }

bool mode_set_dev(bool on)
{
    if (on == s_dev)
        return true;

    if (on) {
        // Not mid-sequence. flap_task may have the bus and be part-way through
        // stopping the desk; pulling the intercept out from under it would
        // leave the desk stopped at the flap height with nobody resuming it.
        if (flap_busy()) {
            printf("not now — the flap sequence is running (%s). 'stop' first.\n",
                   flap_state_str());
            return false;
        }
        s_dev = true;
        printf("\n*** DEV MODE ***\n"
               "The motor commands are unlocked. They go through the travel\n"
               "limits: with expanded and collapsed stored they cannot leave the range;\n"
               "without them only 'mot jog' moves, one motor turn at most.\n"
               "'lim free' lifts that for the session — then nothing does.\n"
               "\n"
               "The flap intercept is suspended while this lasts, so the desk is\n"
               "plain pass-through. 'dev stop', or any reboot, returns to working\n"
               "mode.\n");
        return true;
    }

    stepper_stop();
    stepper_enable(false);
    s_dev = false;
    printf("working mode — motor released, intercept live again.\n");
    mode_report();
    return true;
}
