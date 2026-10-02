//
// console — see console.h.
//
#include "console.h"
#include "bus.h"
#include "wire.h"
#include "flap.h"
#include "settings.h"
#include "stepper.h"
#include "tmc2209.h"
#include "encoder.h"
#include "mt6835.h"
#include "mode.h"
#include "limits.h"
#include "params.h"
#include "trace.h"
#include "board_config.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX        64
#define GREET_REPEATS   6
#define GREET_PERIOD_MS 5000

void desk_abort(void);                  // flap_task.c

static void cm(uint16_t mm, char *out, size_t n)
{
    snprintf(out, n, "%u.%u cm", mm / 10, mm % 10);
}

// Every line is "  <command>  <what it does>", two or more spaces between, and
// docs/commands.md has a row for each. tools/check_commands.sh compares the two
// on every build, so a command added here without its row fails the build.
//
// A handful of words at the top; everything else lives under a group word
// (desk, lim, mot, tmc, enc), and every stored value is set with 'eeprom set'.
static void help(void)
{
    printf(
        "\n"
        "  status              everything at a glance: self-check, desk, motor\n"
        "  calibrate           store the flap's two ends, step by step — always available\n"
        "  done                (calibrate) store the position the flap is at now\n"
        "  abort               (calibrate) stop calibrating; the desk stays locked\n"
        "  reset               clear the flap calibration: motor and DESK locked\n"
        "  stop                stop whatever the firmware is driving, now\n"
        "  dev                 working or dev mode, and the boot self-check\n"
        "  dev start|stop      unlock the development commands, or lock them\n"
        "  eeprom              every stored parameter and its value\n"
        "  eeprom <name>       one parameter\n"
        "  eeprom set <name> <value>   change one; written to flash once idle\n"
        "  ?                   this\n"
        "\n"
        "  desk                height, bus, flap stop, ceiling, presets, coast\n"
        "  desk ceiling [on|off]   refuse UP past %u mm (session only)\n"
        "  lim                 the flap's travel range: stored ends, where the shaft is\n"
        "  debug               is the trace running\n"
        "  debug start|stop    trace every desk decision as it happens, or stop\n"
        "\n"
        "  --- development only: 'dev start' first ---\n"
        "  go <cm>             drive the desk directly, bypassing the panel\n"
        "\n"
        "  lim expanded        store where the flap is now as its EXPANDED end\n"
        "  lim collapsed       store it as its COLLAPSED end (same session)\n"
        "  lim play [<deg>]    measure the gearbox backlash (before the ends)\n"
        "  lim span [<n>]      re-measure the steps between the ends, or set them\n"
        "  lim sync            re-seed the step counter from the encoder\n"
        "  lim free            toggle: ignore the range until reboot — careful\n"
        "\n"
        "  mot                 the flap motor: what it is doing and how it is set\n"
        "  mot on|off          energise the coils, or release them\n"
        "  mot accel <sps2>    ramp rate, microsteps/s^2\n"
        "  mot jog <steps>     small move, max one motor turn — works with no range\n"
        "  mot go expanded|collapsed|<n>   to an end, or n steps from expanded\n"
        "  mot move <steps>    signed microsteps, inside the range\n"
        "  mot rev <revs>      signed revolutions OF THE FLAP SHAFT (%g:1 box)\n"
        "  mot run fwd|back    to the end of travel that way\n"
        "  mot stop            ramp down\n"
        "  mot halt            cut the pulses now — loses position\n"
        "  mot zero            call this position zero\n"
        "\n"
        "  tmc                 how the driver is set up, and what it can report\n"
        "  tmc init            re-establish the link after fixing the wiring\n"
        "  tmc wire            loopback test of our own two pins — no driver needed\n"
        "  tmc scan            ask all four UART addresses which one answers\n"
        "  tmc current <run> [hold]   RMS milliamps per phase\n"
        "  tmc micro <n>       1..256 microsteps\n"
        "  tmc chop stealth|spread    quiet, or more torque at speed\n"
        "  tmc reg <hex> [hex]        read or write one register\n"
        "\n"
        "  enc                 the MT6835: is it there, where is the shaft\n"
        "  enc watch           print the angle as you turn it by hand ('z' zeroes)\n"
        "  enc zero            call this shaft position zero\n"
        "  enc dir [0|1]       which way the angle counts (a register, not a pin)\n"
        "  enc gear [<n>]      reducer between motor and magnet, N:1\n"
        "  enc reg <hex> [hex]        read or write one sensor register\n",
        DESK_CEILING_MM, (double)GEAR_RATIO);
}

static void desk_status(void)
{
    char buf[16];
    uint16_t mm, stand, sit, cu, cd;
    uint32_t age, bok, pok;

    if (desk_height_mm(&mm, &age)) {
        cm(mm, buf, sizeof buf);
        printf("height    %u mm (%s)%s\n", mm, buf,
               age > DESK_HEIGHT_STALE_MS ? "   [STALE — the board has gone quiet]" : "");
    } else {
        printf("height    never seen — is GP%d on the panel pin marked TX?\n",
               PIN_FROM_BOARD);
    }

    bus_counts(&bok, &pok);
    printf("bus       board %lu frames | panel %lu frames | sent %lu\n",
           (unsigned long)bok, (unsigned long)pok, (unsigned long)wire_frames());
    if (!wire_live())
        printf("          NOT TRANSMITTING — the panel has not been heard, so GP%d\n"
               "          is still high-Z and the desk cannot move. Check ground.\n",
               PIN_TO_BOARD);

    printf("doing     %s\n", flap_state_str());

    cm(flap_height(), buf, sizeof buf);
    printf("flap      %-3s at %u mm (%s) | %lu unsafe crossings refused\n",
           flap_enabled() ? "ON" : "off", flap_height(), buf,
           (unsigned long)flap_refused_crossings());

    cm(flap_ceiling_mm(), buf, sizeof buf);
    printf("ceiling   %-3s at %u mm (%s) | %lu UPs refused\n",
           flap_ceiling_on() ? "ON" : "off", flap_ceiling_mm(), buf,
           (unsigned long)flap_blocked_ups());

    printf("presets   stand ");
    if (flap_preset(0x01, &stand)) printf("%u mm", stand); else printf("not known yet");
    printf(" | sit ");
    if (flap_preset(0x02, &sit))   printf("%u mm", sit);   else printf("not known yet");
    printf("\n");

    flap_coast(&cu, &cd);
    printf("coast     up %u mm | down %u mm\n", cu, cd);
}

static void status(void)
{
    mode_report();
    desk_status();
    printf("settings  %s\n", settings_stored() ? "restored from flash — 'eeprom' lists it"
                                               : "defaults (nothing stored yet)");

    if (!TMC_UART_ENABLED) {
        printf("motor     %s at %ld microsteps | %u uSteps strapped,"
               " current from VREF\n",
               stepper_busy() ? "MOVING" : (stepper_enabled() ? "holding" : "released"),
               (long)stepper_pos(), TMC_MICROSTEPS);
    } else if (!tmc2209_link_ok()) {
        printf("motor     NO LINK to the driver — 'tmc' for what to check\n");
    } else {
        uint16_t run_ma, hold_ma;
        tmc2209_current(&run_ma, &hold_ma);
        printf("motor     %s at %ld microsteps | %s | %u uSteps, %u mA\n",
               stepper_busy() ? "MOVING" : (stepper_enabled() ? "holding" : "released"),
               (long)stepper_pos(), tmc2209_stealth() ? "StealthChop" : "SpreadCycle",
               tmc2209_microsteps(), run_ma);
    }
}

static bool parse_cm(const char *s, uint16_t *mm)
{
    char *end;
    double v = strtod(s, &end);
    if (*end || v <= 0.0) return false;
    *mm = (uint16_t)(v * 10.0 + 0.5);
    return true;
}

// ---- the MT6835 ----------------------------------------------------------

static void enc_status(void)
{
    if (!encoder_available()) {
        printf("encoder   NOT ANSWERING.\n"
               "          A missing sensor reads as 0xFF on every byte, and the\n"
               "          CRC of that is not 0xFF — so this is a definite no,\n"
               "          not a maybe. Check CS=GP%d SCK=GP%d MOSI=GP%d\n"
               "          MISO=GP%d, 3V3 and a shared ground. The task retries\n"
               "          every second, so fixing a lead needs no reboot.\n"
               "          %lu frames have failed their CRC so far.\n",
               MT6835_PIN_CS, MT6835_PIN_SCK, MT6835_PIN_MOSI, MT6835_PIN_MISO,
               (unsigned long)encoder_errors());
        return;
    }

    uint32_t raw = encoder_raw();
    printf("encoder   MT6835 answering | field %s\n",
           mt6835_status_str(encoder_status()));
    printf("angle     raw %lu of %lu  (%.3f deg within one turn)\n",
           (unsigned long)raw, (unsigned long)ENCODER_CPR,
           (double)raw * 360.0 / (double)ENCODER_CPR);
    printf("position  %+.3f deg | %+.4f rev of the measured shaft, since 'enc zero'\n",
           encoder_degrees(), encoder_revs());
    printf("gearing   %.4g:1 — tolerance %.3f deg of output\n",
           encoder_gear(), encoder_tolerance_deg());
    uint32_t weak = encoder_weak(), total = encoder_samples();
    printf("health    %lu CRC failures | %lu of %lu samples weak (%.2f%%)"
           " | %lu late | %lu jumps\n",
           (unsigned long)encoder_errors(), (unsigned long)weak,
           (unsigned long)total, total ? 100.0 * weak / total : 0.0,
           (unsigned long)encoder_late(), (unsigned long)encoder_jumps());

    // Each of these means something different, so say which — and for the
    // magnet, say whether it is happening NOW. A cumulative count cannot tell
    // a marginal magnet from one that was marginal while it was being
    // positioned, and those want opposite reactions.
    if (encoder_errors())
        printf("          CRC failures are WIRING — a lead, a missing ground, or\n"
               "          a clock too fast for flying leads. Not magnets.\n");
    if (weak) {
        uint32_t age = encoder_weak_age_ms();
        if (age > 10000)
            printf("          weak field: NOT HAPPENING NOW — the last one was\n"
                   "          %lu s ago, so those were almost certainly while the\n"
                   "          magnet was being positioned. The count is since\n"
                   "          boot; it only matters if it is still climbing.\n",
                   (unsigned long)(age / 1000));
        else
            printf("          WEAK FIELD NOW (last one %lu ms ago). The magnet is\n"
                   "          too far from the package, too small, or not\n"
                   "          diametrically magnetised. 0.5-3 mm above the chip\n"
                   "          and centred on the shaft axis.\n",
                   (unsigned long)age);
    }
    if (encoder_jumps())
        printf("          jumps: a sample saw more than a quarter turn of\n"
               "          movement. That is a WARNING LINE, not an error — the\n"
               "          unwrap is still correct up to half a turn. A handful,\n"
               "          alongside a similar 'late' count, is one delayed sample\n"
               "          covering a longer interval, not lost position. Only a\n"
               "          count that climbs while the shaft moves normally means\n"
               "          the sampling cannot keep up.\n");
}

// Stream the position while the shaft is turned by hand, printing a line only
// when it has actually moved. A quiet shaft prints nothing, so anything that
// appears while nobody is touching it IS the noise floor — no need to read
// jitter out of a scrolling column of near-identical numbers.
//
// The console reads the encoder task's accumulated count, not the SPI bus, so
// polling at 50 Hz costs nothing and a hand-turned shaft reads smoothly.
#define WATCH_PERIOD_MS      20     // 50 Hz — plenty for a hand-turned shaft
#define WATCH_DEADBAND_DEG   0.05   // the sensor jitters a few hundredths at rest
#define WATCH_DEADBAND       ((int64_t)(WATCH_DEADBAND_DEG / 360.0 * ENCODER_CPR))
#define WATCH_IDLE_STATUS_MS 5000   // liveness line while nothing moves

static void enc_watch(void)
{
    if (!encoder_available()) { enc_status(); return; }

    printf("turn the shaft by hand. A line comes out only when it moves;\n"
           "a still shaft prints nothing (deadband %.2f deg), so a line that\n"
           "appears on its own is the noise floor. More than a few hundredths\n"
           "of a degree of that is a magnet mounted too far away or off axis.\n"
           "'z' makes this position zero (same as 'enc zero'); any other key\n"
           "but Enter stops.\n",
           WATCH_DEADBAND_DEG);
    if (stepper_enabled())
        printf("the driver is holding the shaft — 'mot off' first to turn it freely.\n");
    printf("\n");

    // Drain first. A terminal sending CRLF gives us two characters: the
    // console dispatches on the \r and leaves the \n in the buffer, where the
    // "any key stops" test would find it on the first pass. The delay covers
    // a \n that has not quite arrived yet.
    vTaskDelay(pdMS_TO_TICKS(60));
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();

    int64_t  last    = encoder_counts();        // position at the last line printed
    uint32_t gen     = encoder_zero_gen();
    uint32_t idle_ms = WATCH_IDLE_STATUS_MS;    // full status line immediately
    for (;;) {
        int key = getchar_timeout_us(0);
        if (key == 'z') {
            encoder_zero();
            printf("zeroed — the raw angle is unaffected\n");
        } else if (key != PICO_ERROR_TIMEOUT && key != '\r' && key != '\n') {
            printf("stopped.\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(WATCH_PERIOD_MS));
        idle_ms += WATCH_PERIOD_MS;

        // The encoder task applies a zero on its next sample, so the count
        // steps to 0 a couple of milliseconds after 'z'. That step is not
        // motion: restart the deadband from here and show the zeroed line.
        if (encoder_zero_gen() != gen) {
            gen     = encoder_zero_gen();
            last    = encoder_counts();
            idle_ms = WATCH_IDLE_STATUS_MS;
        }

        uint32_t raw = encoder_raw();
        int64_t  now = encoder_counts();        // continuous, so no wrap to fold
        int64_t  d   = now - last;
        double   pos = (double)now * 360.0 / (double)ENCODER_CPR;

        if (d >= WATCH_DEADBAND || d <= -WATCH_DEADBAND) {
            printf("  raw %7lu   %+10.3f deg   (moved %+.4f)   field %s\n",
                   (unsigned long)raw, pos,
                   (double)d * 360.0 / (double)ENCODER_CPR,
                   mt6835_status_str(encoder_status()));
            last    = now;
            idle_ms = 0;
        } else if (idle_ms >= WATCH_IDLE_STATUS_MS) {
            // Steady shaft: prove the reading is alive and *staying put*.
            printf("  raw %7lu   %+10.3f deg   | steady | field %s\n",
                   (unsigned long)raw, pos, mt6835_status_str(encoder_status()));
            idle_ms = 0;
        }
    }
}

static void enc(char *arg)
{
    if (!arg) { enc_status(); return; }
    char *v = strtok(NULL, " \t");

    if (!strcmp(arg, "watch") || !strcmp(arg, "w")) {
        enc_watch();

    } else if (!strcmp(arg, "zero")) {
        encoder_zero();
        printf("this shaft position is now zero. The RAW angle is unaffected —\n"
               "that is the absolute number the travel limits will be stored in.\n");

    } else if (!strcmp(arg, "dir")) {
        if (!encoder_available()) { enc_status(); return; }
        if (v) {
            if (!mt6835_set_rot_dir(*v != '0')) {
                printf("the sensor did not acknowledge the write\n");
                return;
            }
            printf("direction flipped. VOLATILE — it returns at the next power\n"
                   "cycle unless burnt to the sensor's EEPROM.\n");
        }
        printf("ROT_DIR %d. This is the fix when the count runs backwards\n"
               "relative to the motor — a register on this part, not a strapped\n"
               "pin, so no soldering iron is involved.\n", mt6835_rot_dir());

    } else if (!strcmp(arg, "gear")) {
        if (v) {
            double g = strtod(v, NULL);
            if (g <= 0.0) { printf("usage: enc gear <ratio>, e.g. 'enc gear 17.23'\n"); return; }
            encoder_set_gear(g);
            printf("session only — board_config.h's GEAR_RATIO returns at the\n"
                   "next reboot.\n");
        }
        printf("gear %.4g:1 | %.0f sensor counts per motor revolution\n",
               encoder_gear(), (double)ENCODER_CPR / encoder_gear());

    } else if (!strcmp(arg, "reg")) {
        if (!encoder_available()) { enc_status(); return; }
        if (!v) { printf("usage: enc reg <hex> [hex to write]\n"); return; }
        uint16_t reg = (uint16_t)strtoul(v, NULL, 16);
        char *w = strtok(NULL, " \t");
        if (w) {
            uint8_t val = (uint8_t)strtoul(w, NULL, 16);
            printf(mt6835_write_reg(reg, val)
                   ? "0x%03X <- %02X\n"
                   : "0x%03X <- %02X  NOT ACKNOWLEDGED\n", reg, val);
            return;
        }
        printf("0x%03X = %02X\n", reg, mt6835_read_reg(reg));

    } else {
        printf("unknown: 'enc %s'  ('?' for help)\n", arg);
    }
}

// ---- the flap motor -------------------------------------------------------

static void mot_status(void)
{
    char line[128];
    printf("motor   %s | position %ld microsteps | %lu of %lu microsteps/s\n",
           stepper_busy() ? "MOVING" : (stepper_enabled() ? "holding" : "released"),
           (long)stepper_pos(),
           (unsigned long)stepper_cur_sps(), (unsigned long)stepper_speed());
    printf("profile speed %lu microsteps/s | accel %lu microsteps/s^2 |"
           " %lu per motor rev\n",
           (unsigned long)stepper_speed(), (unsigned long)stepper_accel(),
           (unsigned long)stepper_steps_per_rev());
    printf("driver  %s\n", tmc2209_status_line(line, sizeof line));
}

// Wait for the motor to stop, any key stopping it early — the console is
// blocked meanwhile, so this is the only way to say "stop".
static bool wait_or_key(uint32_t timeout_ms)
{
    vTaskDelay(pdMS_TO_TICKS(60));                      // the \n after the \r
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    TickType_t t0 = xTaskGetTickCount();
    while (stepper_busy()) {
        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {
            stepper_stop();
            printf("stopping — key pressed\n");
            stepper_wait(5000);
            return false;
        }
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms)) {
            stepper_stop_hard();
            printf("move timed out\n");
            return false;
        }
        limits_poll_trip();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

// A targeted move is not over when the pulses stop: the shaft is asked where
// it is and crept onto the target. From the bench rig.
static void settle_after_move(void)
{
    if (!wait_or_key(120000))
        return;
    vTaskDelay(pdMS_TO_TICKS(100));             // let the load come to rest
    double  err;
    int32_t crept;
    int r = limits_settle(&err, &crept);
    if (r < 0)
        return;                                 // nothing to settle
    if (r == 0)
        printf("settled %+.3f deg from target, %ld steps crept\n", err, (long)crept);
    else
        printf("NOT SETTLED: crept %ld steps and the shaft is still %+.3f deg off — the\n"
               "  output is not following the motor here\n", (long)crept, err);
}

static void mot(char *arg)
{
    if (!arg) { mot_status(); return; }
    char *v = strtok(NULL, " \t");

    if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
        bool on = !strcmp(arg, "on");
        if (!stepper_enable(on)) {
            printf("the driver would not take it — 'tmc' for the link state\n");
            return;
        }
        printf("coils %s\n", on ? "energised" : "released — the flap turns by hand");

    } else if (!strcmp(arg, "speed")) {
        printf("speed %lu microsteps/s — stored; 'eeprom set mot_speed_sps <n>' changes it\n",
               (unsigned long)stepper_speed());

    } else if (!strcmp(arg, "accel")) {
        if (v) stepper_set_accel((uint32_t)strtoul(v, NULL, 10));
        printf("accel %lu microsteps/s^2 — the ramp takes %lu microsteps to reach"
               " the speed limit\n",
               (unsigned long)stepper_accel(),
               (unsigned long)((uint64_t)stepper_speed() * stepper_speed() /
                               (2u * stepper_accel())));

    } else if (!strcmp(arg, "move") || !strcmp(arg, "rev")) {
        if (!v) { printf("usage: mot %s <signed number>\n", arg); return; }
        double  n     = strtod(v, NULL);
        int32_t steps = !strcmp(arg, "move")
            ? (int32_t)n
            : (int32_t)(n * (double)stepper_steps_per_rev() * (double)GEAR_RATIO);
        if (steps == 0) { printf("that rounds to no steps at all\n"); return; }
        if (!limits_move(steps))
            return;                     // limits.c has said why
        printf("moving %+ld microsteps (%.3f rev of the flap shaft)\n",
               (long)steps,
               (double)steps / (double)stepper_steps_per_rev() / (double)GEAR_RATIO);

    } else if (!strcmp(arg, "jog")) {
        if (!v) { printf("usage: mot jog <signed microsteps>, at most one motor turn\n"); return; }
        int32_t steps = (int32_t)strtol(v, NULL, 10);
        if (!limits_jog(steps))
            return;
        if (limits_targeted())
            settle_after_move();
        else
            printf("jog %+ld microsteps\n", (long)steps);

    } else if (!strcmp(arg, "go")) {
        bool ok;
        if (v && !strcmp(v, "expanded"))      ok = limits_goto_end(false);
        else if (v && !strcmp(v, "collapsed")) ok = limits_goto_end(true);
        else if (v)                      ok = limits_goto((int32_t)strtol(v, NULL, 10));
        else {
            printf("usage: mot go expanded | mot go collapsed | mot go <steps from expanded, 0..%ld>\n",
                   (long)limits_span_steps());
            return;
        }
        if (ok)
            settle_after_move();

    } else if (!strcmp(arg, "run")) {
        bool fwd = !v || !strcmp(v, "fwd") || !strcmp(v, "f");
        if (limits_mode() == LIM_FREE) {
            if (!limits_run(fwd)) return;
            printf("running %s — 'mot stop' to ramp down\n", fwd ? "forward" : "back");
        } else if (limits_run(fwd)) {
            settle_after_move();        // bounded: a move to that end
        }

    } else if (!strcmp(arg, "stop")) {
        stepper_stop();
        printf("ramping down\n");

    } else if (!strcmp(arg, "halt")) {
        stepper_stop_hard();
        printf("pulses cut — the position counter no longer means anything\n");

    } else if (!strcmp(arg, "zero")) {
        stepper_zero();
        printf("position zeroed\n");

    } else {
        printf("unknown: 'mot %s'  ('?' for help)\n", arg);
    }
}

// ---- the driver's registers ----------------------------------------------

static void tmc_status(void)
{
    char line[128];
    if (!TMC_UART_ENABLED) {
        printf("driver    TMC2209 in STANDALONE mode — no serial link.\n"
               "wiring    STEP GP%d | DIR GP%d | EN GP%d (active low)\n"
               "stepping  %u microsteps, from the MS1/MS2 strapping. The driver\n"
               "          is never asked, so this number is a promise about the\n"
               "          hardware: get it wrong and every revolution figure is\n"
               "          off by that ratio and nothing complains.\n"
               "current   the VREF trimmer. Not visible from here, not settable\n"
               "          from here. On a 0.11 ohm carrier I_rms is about\n"
               "          0.71 * VREF.\n"
               "faults    none reported. Over-temperature, a shorted phase and\n"
               "          an open coil all look identical to a working motor in\n"
               "          this mode — that is what the UART would buy.\n"
               "\n"
               "          TMC_UART_ENABLED in board_config.h turns the link on;\n"
               "          ../docs/flap-motor.md has the wiring it needs.\n",
               PIN_STEP, PIN_DIR, PIN_EN, TMC_MICROSTEPS);
        return;
    }
    if (!tmc2209_link_ok()) {
        tmc2209_probe();                // fresh, so the diagnosis is current
        printf("driver    NO LINK.\n"
               "          %s\n"
               "\n"
               "          'tmc wire' tests our two pins alone. 'tmc init' retries\n"
               "          the link. Nothing can be enabled without it: the chopper\n"
               "          is the enable now, so a silent driver is a motor that\n"
               "          refuses to move rather than one running blind.\n",
               tmc2209_why());
        return;
    }

    uint16_t run_ma, hold_ma;
    uint32_t gconf = 0, chop = 0;
    tmc2209_current(&run_ma, &hold_ma);
    tmc2209_read(TMC_GCONF, &gconf);
    tmc2209_read(TMC_CHOPCONF, &chop);

    printf("driver    TMC2209 version 0x%02X on GP%d/GP%d at address %d\n",
           tmc2209_version(), PIN_TMC_TX, PIN_TMC_RX, tmc2209_address());
    printf("wiring    STEP on GP%d | direction by %s | enable by %s\n",
           PIN_STEP,
           TMC_USE_DIR_PIN ? "the DIR pin" : "GCONF.shaft",
           TMC_USE_EN_PIN  ? "the EN pin"  : "CHOPCONF.TOFF");
    printf("current   run %u mA | hold %u mA (after %.1f s of standstill)\n",
           run_ma, hold_ma, TMC_TPOWERDOWN_TICKS * 0.021);
    printf("stepping  %u microsteps, interpolated to 1/256 | %s\n",
           tmc2209_microsteps(), tmc2209_stealth() ? "StealthChop" : "SpreadCycle");
    printf("reports   %s\n", tmc2209_status_line(line, sizeof line));
    printf("registers GCONF %08lx | CHOPCONF %08lx | %lu bad datagrams\n",
           (unsigned long)gconf, (unsigned long)chop,
           (unsigned long)tmc2209_errors());
}

static void tmc(char *arg)
{
    if (!arg) { tmc_status(); return; }
    char *v = strtok(NULL, " \t");

    if (!TMC_UART_ENABLED) {
        printf("'tmc %s' needs the serial link, and this build is standalone.\n"
               "Set TMC_UART_ENABLED to 1 in board_config.h and wire GP%d and\n"
               "GP%d to PDN_UART per ../docs/flap-motor.md — which includes the\n"
               "solder jumper on the carrier, the one that ships unbridged.\n"
               "Until then: microstepping is the MS1/MS2 strapping and current\n"
               "is the VREF trimmer. 'tmc' on its own says what that means.\n",
               arg, PIN_TMC_TX, PIN_TMC_RX);
        return;
    }

    if (!strcmp(arg, "init")) {
        if (tmc2209_init())
            printf("link up\n");

    } else if (!strcmp(arg, "scan")) {
        int found = tmc2209_scan();
        if (found < 0) {
            printf("no driver answered on any of the four addresses.\n"
                   "The address is not the problem — something more basic is.\n"
                   "'tmc' for what, and 'tmc wire' to rule out our own pins.\n");
            return;
        }
        printf("address %d answered. Now using it.\n", found);
        if (found != TMC_UART_ADDRESS)
            printf("board_config.h says %d — set TMC_UART_ADDRESS to %d to make\n"
                   "this survive a reboot, or ground MS1/MS2 to move the driver\n"
                   "to address 0 instead.\n", TMC_UART_ADDRESS, found);
        tmc2209_init();

    } else if (!strcmp(arg, "wire")) {
        // The one test that needs no driver: our transmit pin reaches our
        // receive pin through the resistor, on this board. If the pattern does
        // not come back, nothing beyond these two pins can be the cause.
        uint8_t  sent[6];
        int16_t  got[6];
        int n = tmc2209_loopback(sent, got, 6);
        if (!n) { printf("the PIO UART is not running\n"); return; }

        int bad = 0;
        printf("loopback GP%d -> 1k -> GP%d, no driver needed\n",
               PIN_TMC_TX, PIN_TMC_RX);
        for (int i = 0; i < n; i++) {
            if (got[i] < 0) {
                printf("  sent 0x%02X   nothing came back\n", sent[i]);
                bad++;
            } else {
                printf("  sent 0x%02X   read 0x%02X%s\n", sent[i],
                       (unsigned)got[i], got[i] == sent[i] ? "" : "   MISMATCH");
                if (got[i] != sent[i]) bad++;
            }
        }
        if (!bad) {
            printf("\nOur side is good: both pins, the resistor and the PIO all\n"
                   "work. Whatever is wrong is beyond the junction — the wire to\n"
                   "PDN_UART, VM, VDD, the ground, or MS1/MS2.\n");
        } else if (bad == n) {
            printf("\nNothing at all came back. The loop does not close: the 1k\n"
                   "is missing or bridges the wrong pads, GP%d is not on the\n"
                   "junction, or the node is held by something — a PDN pad\n"
                   "strapped to GND for standalone mode, or an unpowered driver\n"
                   "clamping it. Unplug the driver and run this again.\n",
                   PIN_TMC_RX);
        } else {
            printf("\nSome bytes survived and some did not, which is a marginal\n"
                   "connection or a contended line rather than a missing one.\n");
        }

    } else if (!strcmp(arg, "current")) {
        if (v) {
            char *h = strtok(NULL, " \t");
            uint16_t run  = (uint16_t)strtoul(v, NULL, 10);
            uint16_t hold = h ? (uint16_t)strtoul(h, NULL, 10) : (uint16_t)(run / 2);
            if (!tmc2209_set_current(run, hold)) {
                printf("rejected: run current must be <= %u mA and hold <= run.\n"
                       "The driver resolves it to one of 32 levels, so ask for what\n"
                       "you want and read back what you got.\n", TMC_MAX_CURRENT_MA);
                return;
            }
        }
        uint16_t run_ma, hold_ma;
        tmc2209_current(&run_ma, &hold_ma);
        printf("run %u mA | hold %u mA\n", run_ma, hold_ma);

    } else if (!strcmp(arg, "micro")) {
        if (v) {
            if (stepper_busy()) {
                printf("not while it is moving — a step would change meaning"
                       " mid-ramp\n");
                return;
            }
            if (!tmc2209_set_microsteps((uint16_t)strtoul(v, NULL, 10))) {
                printf("usage: tmc micro 1|2|4|8|16|32|64|128|256\n");
                return;
            }
            printf("speed and accel are in MICROSTEPS, so they now mean a"
                   " different rate\n");
        }
        printf("%u microsteps | %lu per motor revolution\n",
               tmc2209_microsteps(), (unsigned long)stepper_steps_per_rev());

    } else if (!strcmp(arg, "chop")) {
        if (v && !tmc2209_set_stealth(!strcmp(v, "stealth"))) {
            printf("the driver would not take it\n");
            return;
        }
        printf("%s\n", tmc2209_stealth()
               ? "StealthChop — quiet, less torque at speed"
               : "SpreadCycle — louder, holds torque up the range");

    } else if (!strcmp(arg, "reg")) {
        if (!v) { printf("usage: tmc reg <hex> [hex to write]\n"); return; }
        uint8_t reg = (uint8_t)strtoul(v, NULL, 16);
        char *w = strtok(NULL, " \t");
        if (w) {
            uint32_t val = (uint32_t)strtoul(w, NULL, 16);
            printf(tmc2209_write(reg, val)
                   ? "0x%02X <- %08lx\n"
                   : "0x%02X <- %08lx  REFUSED (IFCNT did not advance)\n",
                   reg, (unsigned long)val);
            return;
        }
        uint32_t val;
        if (!tmc2209_read(reg, &val)) {
            printf("0x%02X did not answer — not every register is readable\n", reg);
            return;
        }
        printf("0x%02X = %08lx\n", reg, (unsigned long)val);

    } else {
        printf("unknown: 'tmc %s'  ('?' for help)\n", arg);
    }
}

// Working mode is not a reduced console, it is the console for a board doing
// its job: everything an installer needs — the heights, the presets, the coast
// figures, 'stop' — stays. What is behind the gate is everything that drives
// something directly or pokes at a register, because none of that has any
// business happening because of a mistyped line on a desk in use.
// ---- calibrate: guided calibration ----------------------------------------
//
// Always available, working mode included: a board without its calibration
// has a locked desk, and whoever is in front of it must be able to fix that
// without knowing about dev mode. It clears the calibration at once (what
// 'reset' clears), then asks for the flap at min, then at max, and stores each
// on 'done'. Everything else the range needs — the span in sensor counts and
// in motor steps — limits.c works out from those two marks.
//
// While it runs, 'mot' and 'enc' are unlocked so the flap can be positioned.
// 'mot jog' moves it, one motor turn at most per command since there is no
// range yet to check a bigger move against, and an empty line repeats the last
// jog. Turning the flap by hand does NOT work: the steps between the ends are
// counted from the motor's own moves, and a hand-turned end has none.

typedef enum { CAL_OFF, CAL_EXPANDED, CAL_COLLAPSED } cal_t;
static cal_t s_cal;
static char   s_repeat[LINE_MAX];       // the last 'mot jog' typed during calibration

static const char *prompt(void)
{
    return s_cal == CAL_EXPANDED ? "calibrate expanded> " : s_cal == CAL_COLLAPSED ? "calibrate collapsed> " : "> ";
}

static void cal_ask(void)
{
    printf("\nCALIBRATE %s — move the flap to its %s with the motor:\n"
           "  mot jog <steps>   signed, at most %lu per command; Enter repeats it\n"
           "  enc               where the shaft is\n"
           "  done              store this position\n"
           "  abort             stop calibrating (the desk stays locked)\n",
           s_cal == CAL_EXPANDED ? "1/2" : "2/2",
           s_cal == CAL_EXPANDED ? "EXPANDED position"
                              : "COLLAPSED position",
           (unsigned long)stepper_steps_per_rev());
}

static void cal_start(void)
{
    if (flap_busy()) {
        printf("not now — the desk is mid-sequence (%s). 'stop' first.\n",
               flap_state_str());
        return;
    }
    if (stepper_busy()) { printf("the motor is moving — 'stop' first\n"); return; }
    if (!encoder_available()) {
        printf("calibrate needs the MT6835 and it is not answering. Nothing was\n"
               "cleared. 'dev start' then 'enc' says what to check.\n");
        return;
    }
    if (!limits_clear())                // prints what it cleared and kept
        return;
    s_repeat[0] = '\0';
    s_cal = CAL_EXPANDED;
    stepper_enable(true);               // hold the flap where the jogs leave it
    cal_ask();
}

static void cal_done(void)
{
    if (stepper_busy()) { printf("still moving — wait for it, then 'done'\n"); return; }
    if (s_cal == CAL_EXPANDED) {
        if (!limits_mark(false))
            return;                     // limits.c has said why; still at min
        s_cal = CAL_COLLAPSED;
        cal_ask();
        return;
    }
    limits_mark(true);
    if (!limits_calibrated()) {
        // limits.c has kept only one end; the clean retry is from the top.
        limits_clear();
        s_cal = CAL_EXPANDED;
        printf("that did not make a usable range — starting again from expanded.\n");
        cal_ask();
        return;
    }
    s_cal = CAL_OFF;
    printf("\nCALIBRATION COMPLETE — the desk is unlocked. Stored:\n");
    params_print("lim_expanded_raw");
    params_print("lim_collapsed_raw");
    params_print("lim_enc_span");
    params_print("lim_span_steps");
    printf("written to flash in a couple of seconds.\n");
}

static void cal_abort(void)
{
    s_cal = CAL_OFF;
    stepper_stop();
    printf("calibration aborted — the flap is NOT calibrated, so the DESK stays locked.\n"
           "'calibrate' starts again.\n");
}

static bool unlocked(const char *cmd)
{
    if (mode_dev())
        return true;
    if (s_cal && (!strcmp(cmd, "mot") || !strcmp(cmd, "enc")))
        return true;                    // positioning the flap for calibration
    printf("'%s' is a development command and this board is in working mode.\n"
           "'dev start' unlocks it. It relocks at the next reboot — there is no\n"
           "stored mode, so a production board always comes up working.\n", cmd);
    return false;
}

static void dev(char *arg)
{
    if (!arg) { mode_report(); return; }
    if (!strcmp(arg, "start") || !strcmp(arg, "on"))
        mode_set_dev(true);
    else if (!strcmp(arg, "stop") || !strcmp(arg, "off"))
        mode_set_dev(false);
    else
        printf("usage: dev start | dev stop | dev\n");
}

// 'lim' alone is a report and works in working mode; everything that moves the
// motor or changes the stored range is development. Its two typed-in values,
// backlash and approach, are parameters: 'eeprom set lim_backlash|lim_approach_deg'.
static void lim(char *arg)
{
    if (!arg) { limits_report(); return; }
    if (!unlocked("lim")) return;
    char *v = strtok(NULL, " \t");

    if (!strcmp(arg, "expanded"))        limits_mark(false);
    else if (!strcmp(arg, "collapsed"))   limits_mark(true);
    else if (!strcmp(arg, "sync"))  limits_sync(true);
    else if (!strcmp(arg, "free"))  limits_set_free(limits_mode() != LIM_FREE);
    else if (!strcmp(arg, "span")) {
        if (!v) limits_store_span();
        else    limits_set_span((int32_t)strtol(v, NULL, 10));
    } else if (!strcmp(arg, "play")) {
        limits_measure_play(v ? strtod(v, NULL) : 1.0);
    } else {
        printf("usage: lim [expanded | collapsed | play [deg] | span [n] | sync | free]\n");
    }
}

static void desk(char *arg)
{
    if (!arg) { desk_status(); return; }
    char *v = strtok(NULL, " \t");
    char buf[16];

    if (!strcmp(arg, "ceiling")) {
        if (v && !strcmp(v, "on"))       flap_set_ceiling(true);
        else if (v && !strcmp(v, "off")) flap_set_ceiling(false);
        else if (v) { printf("usage: desk ceiling [on|off]\n"); return; }
        cm(flap_ceiling_mm(), buf, sizeof buf);
        printf("ceiling %s at %u mm (%s) — UP refused from %u up. %lu refused so far.\n",
               flap_ceiling_on() ? "ON" : "off", flap_ceiling_mm(), buf,
               flap_ceiling_mm() - DESK_COAST_UP_MM, (unsigned long)flap_blocked_ups());
    } else {
        printf("unknown: 'desk %s'  ('?' for help)\n", arg);
    }
}

static void eeprom(char *arg)
{
    if (!arg) { params_print_all(); return; }
    if (!strcmp(arg, "set")) {
        char *name  = strtok(NULL, " \t");
        char *value = strtok(NULL, " \t");
        if (!name || !value) { printf("usage: eeprom set <name> <value>\n"); return; }
        params_set(name, value);
        return;
    }
    if (!params_print(arg))
        printf("no parameter '%s' — 'eeprom' lists them\n", arg);
}

static void dispatch(char *line)
{
    // Remember a jog typed during calibration before strtok cuts the line up.
    if (s_cal && !strncmp(line, "mot jog ", 8))
        snprintf(s_repeat, sizeof s_repeat, "%s", line);

    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    char *arg = strtok(NULL, " \t");

    if (!strcmp(cmd, "?")) {
        help();

    } else if (!strcmp(cmd, "calibrate")) {
        cal_start();

    } else if (!strcmp(cmd, "done")) {
        if (s_cal) cal_done();
        else        printf("'done' answers 'calibrate' — nothing is waiting for it\n");

    } else if (!strcmp(cmd, "abort")) {
        if (s_cal) cal_abort();
        else        printf("'abort' leaves 'calibrate' — which is not running\n");

    } else if (!strcmp(cmd, "status")) {
        status();

    } else if (!strcmp(cmd, "stop")) {
        desk_abort();
        stepper_stop();
        printf("stopping\n");

    } else if (!strcmp(cmd, "dev")) {
        dev(arg);

    } else if (!strcmp(cmd, "eeprom")) {
        eeprom(arg);

    } else if (!strcmp(cmd, "desk")) {
        desk(arg);

    } else if (!strcmp(cmd, "debug")) {
        if (!arg)                       trace_report();
        else if (!strcmp(arg, "start")) {
            trace_set(true);
            printf("debug trace ON — every change is printed as [dbg <seconds>].\n"
                   "Reproduce the problem, then 'debug stop'. Nothing is stored.\n");
        } else if (!strcmp(arg, "stop")) {
            trace_drain();
            trace_set(false);
            printf("debug trace off\n");
        } else printf("usage: debug [start|stop]\n");

    } else if (!strcmp(cmd, "lim")) {
        lim(arg);

    } else if (!strcmp(cmd, "go")) {
        if (!unlocked(cmd)) return;
        uint16_t mm;
        if (!arg || !parse_cm(arg, &mm)) { printf("usage: go <cm>, e.g. 'go 73.5'\n"); return; }
        if (!mode_desk_may_move()) {
            printf("refused: DESK LOCKED — %s. 'lim' for how.\n", mode_desk_blocked_by());
            return;
        }
        // 'go' drives the desk and nothing else: it does not move the flap.
        uint16_t now_mm; uint32_t age;
        if (FLAP_DRIVES_MOTOR && desk_height_mm(&now_mm, &age) &&
            mm > flap_height() && mm > now_mm && !limits_at_end(true)) {
            printf("refused: going up to %u mm, above the flap height (%u), with the\n"
                   "flap not collapsed — 'mot go collapsed' first.\n", mm, flap_height());
            return;
        }
        if (!flap_go(mm)) { printf("busy: %s\n", flap_state_str()); return; }

    } else if (!strcmp(cmd, "reset")) {
        // Always available, like 'calibrate': it only ever makes the board
        // more cautious (the desk locks), and whoever is at the desk must be
        // able to start over without knowing about dev mode.
        // Only what the lock depends on. Presets, coast, the flap height,
        // backlash and approach are kept — 'eeprom set' changes those.
        if (limits_clear())
            printf("'calibrate' calibrates it again.\n");

    } else if (!strcmp(cmd, "mot")) {
        if (!unlocked(cmd)) return;
        mot(arg);

    } else if (!strcmp(cmd, "tmc")) {
        if (!unlocked(cmd)) return;
        tmc(arg);

    } else if (!strcmp(cmd, "enc")) {
        if (!unlocked(cmd)) return;
        enc(arg);

    } else {
        printf("unknown: '%s'  ('?' for help)\n", cmd);
    }
}

void console_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];
    size_t len = 0;
    bool spoken_to = false;
    TickType_t last_greet = 0;
    unsigned greets = 0;
    int prev = 0;

    for (;;) {
        // Repeat the banner briefly: flashing re-enumerates USB, so a terminal
        // attaching after boot would otherwise see a blank screen.
        TickType_t now = xTaskGetTickCount();
        if (!spoken_to && greets < GREET_REPEATS &&
            (last_greet == 0 || now - last_greet >= pdMS_TO_TICKS(GREET_PERIOD_MS))) {
            printf("\n=== desk-move-safe ===\n");
            status();
            if (!limits_calibrated())
                printf("The flap is not calibrated — type 'calibrate' to do it.\n");
            printf("'?' for commands.\n%s", prompt());
            last_greet = now ? now : 1;
            greets++;
            len = 0;
        }

        settings_flush();       // writes only when idle and settled
        limits_poll_trip();     // a guard trip, said once
        trace_drain();          // 'debug start' events, if any

        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        spoken_to = true;

        // CRLF is one Enter, not two: otherwise the \n after a line would be
        // an empty line, and during calibration an empty line repeats the last jog.
        bool lf_of_crlf = (c == '\n' && prev == '\r');
        prev = c;
        if (lf_of_crlf)
            continue;

        if (c == '\r' || c == '\n') {
            printf("\n");
            line[len] = '\0';
            if (len) {
                dispatch(line);
            } else if (s_cal && s_repeat[0]) {
                snprintf(line, sizeof line, "%s", s_repeat);
                printf("%s\n", line);
                dispatch(line);
            }
            len = 0;
            printf("%s", prompt());
        } else if (c == '\b' || c == 0x7f) {
            if (len) { len--; printf("\b \b"); }
        } else if (c >= 0x20 && c < 0x7f && len < LINE_MAX - 1) {
            line[len++] = (char)c;
            putchar(c);
        }
    }
}
