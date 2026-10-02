//
// mode — see mode.h.
//
#include "mode.h"
#include "settings.h"
#include "stepper.h"
#include "tmc2209.h"
#include "encoder.h"
#include "flap.h"
#include "board_config.h"

#include <stdio.h>

// RAM only, and it has no initialiser on purpose: BSS is zeroed at reset, so
// "working mode" is what a reset produces without anything having to run.
static bool s_dev;

// The bits limits.c uses in settings()->flap.flags, kept here so the boot check
// can ask the question before the module that answers it exists. They come from
// the bench rig's record, which settings_flap_t copies field for field.
#define F_MIN   1u
#define F_MAX   2u

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
    // which is harmless — nothing gates on it until FLAP_DRIVES_MOTOR.
    c->encoder = encoder_available() ? CHK_OK : CHK_FAIL;

    // THE TRAVEL LIMITS. This one IS askable today: the record exists and the
    // flags are zero until both ends have been stored. Step 3 fills them in;
    // until then the answer is a truthful "not stored".
    uint32_t f = settings()->flap.flags;
    c->limits = ((f & (F_MIN | F_MAX)) == (F_MIN | F_MAX)) ? CHK_OK : CHK_FAIL;
}

// What the flap has to be fit FOR depends on what it actually does.
//
// While run_flap() is a two-second dwell it touches no motor, so neither the
// encoder nor the travel limits are preconditions for it — and refusing to stop
// the desk because of them would remove a feature that works today in exchange
// for nothing. FLAP_DRIVES_MOTOR turns them into hard requirements at the same
// moment they start to matter.
bool mode_flap_may_run(void)
{
    if (s_dev)
        return false;
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
    if (!FLAP_DRIVES_MOTOR)
        return "";

    selfcheck_t c;
    mode_check(&c);
    if (c.driver  == CHK_FAIL) return "the stepper driver does not answer";
    if (c.encoder == CHK_FAIL) return "the MT6835 does not answer";
    if (c.limits  == CHK_FAIL) return "min and max are not stored — 'lim min' "
                                      "and 'lim max' in dev mode";
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

    const char *why = mode_flap_blocked_by();
    if (*why)
        printf("          FLAP OFF: %s.\n"
               "          The desk still works — the panel drives it exactly as\n"
               "          it did before this board was fitted.\n", why);
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
               "The motor commands are unlocked and NOTHING bounds them: there\n"
               "are no travel limits in this build, so 'mot move' will drive the\n"
               "flap into its end stop and keep pulsing. Nothing detects that.\n"
               "\n"
               "The flap intercept is suspended while this lasts, so the desk is\n"
               "plain pass-through. 'dev stop', or any reset, returns to working\n"
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
