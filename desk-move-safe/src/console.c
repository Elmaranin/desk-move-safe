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

static void help(void)
{
    printf(
        "\n"
        "  status              height, what it is doing, every setting\n"
        "  flap [<cm>]         the height the desk is stopped at for the flap\n"
        "  flap on|off         whether it stops there at all\n"
        "  ceiling [on|off]    refuse UP past %u mm, whatever the panel asks\n"
        "  presets             the stand and sit heights\n"
        "  presets stand|sit <cm>   set one (0 = forget, re-learn it)\n"
        "  coast <up> <down>   how far the desk runs on after being told to stop\n"
        "  stop                stop now\n"
        "  dev                 working or dev mode, and the boot self-check\n"
        "  dev start|stop      unlock the development commands, or lock them\n"
        "\n"
        "  --- development only: 'dev start' first ---\n"
        "  go <cm>             drive the desk directly, bypassing the panel\n"
        "  forget              erase the stored settings\n"
        "  mot                 the flap motor: what it is doing and how it is set\n"
        "  mot on|off          energise the coils, or release them\n"
        "  mot speed <sps>     cruise rate, microsteps/s\n"
        "  mot accel <sps2>    ramp rate, microsteps/s^2\n"
        "  mot move <steps>    signed microsteps, full accel/cruise/decel\n"
        "  mot rev <revs>      signed revolutions OF THE FLAP SHAFT (%g:1 box)\n"
        "  mot run fwd|back    run until 'mot stop'\n"
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
        "  enc reg <hex> [hex]        read or write one sensor register\n"
        "  ?                   this\n",
        DESK_CEILING_MM, (double)GEAR_RATIO);
}

static void status(void)
{
    char buf[16];

    mode_report();
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
    printf("flap      %-3s at %u mm (%s)\n", flap_enabled() ? "ON" : "off",
           flap_height(), buf);

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
    printf("settings  %s\n", settings_stored() ? "restored from flash"
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
               "          every second, so fixing a lead needs no reset.\n"
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
                   "next reset.\n");
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
        if (v) stepper_set_speed((uint32_t)strtoul(v, NULL, 10));
        printf("speed %lu microsteps/s\n", (unsigned long)stepper_speed());

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
        if (!stepper_move(steps)) {
            printf("%s\n", stepper_busy() ? "already moving — 'mot stop' first"
                                          : "the driver would not enable");
            return;
        }
        printf("moving %+ld microsteps (%.3f rev of the flap shaft)\n",
               (long)steps,
               (double)steps / (double)stepper_steps_per_rev() / (double)GEAR_RATIO);

    } else if (!strcmp(arg, "run")) {
        bool fwd = !v || !strcmp(v, "fwd") || !strcmp(v, "f");
        if (!stepper_run(fwd)) {
            printf("%s\n", stepper_busy() ? "already moving — 'mot stop' first"
                                          : "the driver would not enable");
            return;
        }
        printf("running %s — 'mot stop' to ramp down\n", fwd ? "forward" : "back");

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
static bool unlocked(const char *cmd)
{
    if (mode_dev())
        return true;
    printf("'%s' is a development command and this board is in working mode.\n"
           "'dev start' unlocks it. It relocks at the next reset — there is no\n"
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

static void dispatch(char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    char *arg = strtok(NULL, " \t");
    char buf[16];

    if (!strcmp(cmd, "?") || !strcmp(cmd, "help")) {
        help();

    } else if (!strcmp(cmd, "status") || !strcmp(cmd, "p")) {
        status();

    } else if (!strcmp(cmd, "flap")) {
        if (arg && !strcmp(arg, "on"))       { flap_set_enabled(true);  settings_mark_dirty(); }
        else if (arg && !strcmp(arg, "off")) { flap_set_enabled(false); settings_mark_dirty(); }
        else if (arg) {
            uint16_t mm;
            if (!parse_cm(arg, &mm) ||
                mm < DESK_MIN_MM + DESK_NEAR_MM || mm > DESK_CEILING_MM - DESK_NEAR_MM) {
                printf("usage: flap <cm>, between %u.%u and %u.%u\n",
                       (DESK_MIN_MM + DESK_NEAR_MM) / 10, (DESK_MIN_MM + DESK_NEAR_MM) % 10,
                       (DESK_CEILING_MM - DESK_NEAR_MM) / 10, (DESK_CEILING_MM - DESK_NEAR_MM) % 10);
                return;
            }
            flap_set_height(mm);
            settings_mark_dirty();
        }
        cm(flap_height(), buf, sizeof buf);
        printf("flap %s at %u mm (%s)\n", flap_enabled() ? "ON" : "off",
               flap_height(), buf);

    } else if (!strcmp(cmd, "ceiling")) {
        if (arg && !strcmp(arg, "on"))       flap_set_ceiling(true);
        else if (arg && !strcmp(arg, "off")) flap_set_ceiling(false);
        cm(flap_ceiling_mm(), buf, sizeof buf);
        printf("ceiling %s at %u mm (%s) — UP refused from %u up. %lu refused so far.\n",
               flap_ceiling_on() ? "ON" : "off", flap_ceiling_mm(), buf,
               flap_ceiling_mm() - DESK_COAST_UP_MM, (unsigned long)flap_blocked_ups());

    } else if (!strcmp(cmd, "presets")) {
        if (arg) {
            uint8_t key = !strcmp(arg, "stand") ? 0x01 : !strcmp(arg, "sit") ? 0x02 : 0;
            char *v = strtok(NULL, " \t");
            uint16_t mm = 0;
            if (!key || !v || (strcmp(v, "0") && !parse_cm(v, &mm))) {
                printf("usage: presets stand <cm> | presets sit <cm> | presets sit 0\n"
                       "Knowing a preset's height lets its recall be caught before the\n"
                       "board hears it, so nothing starts moving and nothing has to be\n"
                       "stopped. They are learned on first use if you leave them unset.\n");
                return;
            }
            if (!flap_set_preset(key, mm)) {
                printf("rejected: must be 0, or between %u and %u mm\n",
                       DESK_MIN_MM, DESK_MAX_MM);
                return;
            }
            settings_mark_dirty();
        }
        uint16_t stand, sit;
        printf("presets stand ");
        if (flap_preset(0x01, &stand)) printf("%u mm", stand); else printf("not known yet");
        printf(" | sit ");
        if (flap_preset(0x02, &sit))   printf("%u mm", sit);   else printf("not known yet");
        printf("\n");

    } else if (!strcmp(cmd, "coast")) {
        if (arg) {
            char *d = strtok(NULL, " \t");
            if (!d || !flap_set_coast((uint16_t)strtoul(arg, NULL, 10),
                                      (uint16_t)strtoul(d, NULL, 10))) {
                printf("usage: coast <up_mm> <down_mm>, e.g. 'coast 18 19'\n"
                       "How far the desk runs on after being told to stop. Two figures\n"
                       "because it is not the same in both directions. They need not be\n"
                       "exact — anything inside %u mm of the mark is close enough.\n",
                       DESK_NEAR_MM);
                return;
            }
            settings_mark_dirty();
        }
        uint16_t cu, cd;
        flap_coast(&cu, &cd);
        printf("coast up %u mm | down %u mm\n", cu, cd);

    } else if (!strcmp(cmd, "dev")) {
        dev(arg);

    } else if (!strcmp(cmd, "go")) {
        if (!unlocked(cmd)) return;
        uint16_t mm;
        if (!arg || !parse_cm(arg, &mm)) { printf("usage: go <cm>, e.g. 'go 73.5'\n"); return; }
        if (!flap_go(mm)) { printf("busy: %s\n", flap_state_str()); return; }

    } else if (!strcmp(cmd, "stop")) {
        desk_abort();
        printf("stopping\n");

    } else if (!strcmp(cmd, "forget")) {
        if (!unlocked(cmd)) return;
        settings_forget();
        flap_set_preset(0x01, 0);
        flap_set_preset(0x02, 0);
        printf("stored settings erased — presets unknown again; the rest returns to\n"
               "board_config.h at the next boot\n");

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

    for (;;) {
        // Repeat the banner briefly: flashing re-enumerates USB, so a terminal
        // attaching after boot would otherwise see a blank screen.
        TickType_t now = xTaskGetTickCount();
        if (!spoken_to && greets < GREET_REPEATS &&
            (last_greet == 0 || now - last_greet >= pdMS_TO_TICKS(GREET_PERIOD_MS))) {
            printf("\n=== desk-move-safe ===\n");
            status();
            printf("'?' for commands.\n> ");
            last_greet = now ? now : 1;
            greets++;
            len = 0;
        }

        settings_flush();       // writes only when idle and settled

        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        spoken_to = true;

        if (c == '\r' || c == '\n') {
            printf("\n");
            line[len] = '\0';
            if (len) dispatch(line);
            len = 0;
            printf("> ");
        } else if (c == '\b' || c == 0x7f) {
            if (len) { len--; printf("\b \b"); }
        } else if (c >= 0x20 && c < 0x7f && len < LINE_MAX - 1) {
            line[len++] = (char)c;
            putchar(c);
        }
    }
}
