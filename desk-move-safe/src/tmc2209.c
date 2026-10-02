//
// tmc2209 — see tmc2209.h.
//
#include "tmc2209.h"
#include "board_config.h"
#include "tmc_uart.pio.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdio.h>

#define SYNC            0x05u
#define REPLY_ADDR      0xFFu       // the chip answers as 0xFF, whatever its own address

// A whole datagram is 8 bytes; at 115200 baud that is 700 us, and the chip
// waits a few bit times before answering. Ten milliseconds is not a tight
// deadline, it is "the wire is disconnected".
#define BYTE_TIMEOUT_US 10000u

static PIO  s_pio;
static uint s_sm_tx, s_sm_rx;
static uint s_off_tx, s_off_rx;
static bool s_pio_ready;

static bool     s_link;
static uint8_t  s_version;
static uint32_t s_errors;
static uint8_t  s_ifcnt;            // our idea of the chip's write counter

// The write-only registers, shadowed: the chip will not read them back, so the
// only copy of what we asked for is ours.
static uint32_t s_ihold_irun;
static uint16_t s_run_ma, s_hold_ma;
static uint32_t s_gconf;
static uint32_t s_chopconf;
static uint16_t s_microsteps = TMC_MICROSTEPS;

// Which driver we are addressing. A constant would do if MS1/MS2 were reliably
// strapped, but they are the address bits and carriers disagree about how they
// idle — so it is a variable and tmc2209_scan() can find out.
static uint8_t  s_addr = TMC_UART_ADDRESS;
static bool     s_chopper_on;

static SemaphoreHandle_t s_lock;

// Why the last transaction failed, kept so the console can say something
// better than "no answer". The echo bytes are the interesting part: they say
// whether the fault is on our side of the wire or beyond it.
static tmc_link_t s_why = TMC_LINK_NO_ECHO;
static uint8_t    s_echo_want;
static int16_t    s_echo_got = -1;      // -1 = nothing came back at all

// ---- CRC ------------------------------------------------------------------
//
// The datasheet's own routine: x^8 + x^2 + x + 1, fed LSB first. Written out
// rather than table-driven so a change to the polynomial cannot leave a stale
// table behind it — the same reason wire.c generates the desk's checksum.
static uint8_t crc8(const uint8_t *data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc >> 7) ^ (b & 1))
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc = (uint8_t)(crc << 1);
            b >>= 1;
        }
    }
    return crc;
}

// ---- the wire -------------------------------------------------------------

static void tx_byte(uint8_t b)
{
    pio_sm_put_blocking(s_pio, s_sm_tx, (uint32_t)b);
}

static bool rx_byte(uint8_t *out, uint32_t timeout_us)
{
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
            return false;
        tight_loop_contents();
    }
    // Shifting right into a 32-bit register leaves the byte in the top octet.
    *out = (uint8_t)(s_pio->rxf[s_sm_rx] >> 24);
    return true;
}

static void rx_drain(void)
{
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx))
        (void)s_pio->rxf[s_sm_rx];
}

// Send a datagram and swallow its echo.
//
// The echo is not noise to be tolerated — it is the measurement. TX reaches
// the driver's pad through a resistor and RX sits on the pad, so our own bytes
// come back first, every time, whether or not a driver is attached. If they do
// not come back the fault is on OUR side of the wire (a pin not claimed, the
// resistor missing, the lead off), which is worth telling apart from a driver
// that simply never answers.
static tmc_link_t send(const uint8_t *frame, size_t n)
{
    rx_drain();
    for (size_t i = 0; i < n; i++)
        tx_byte(frame[i]);

    for (size_t i = 0; i < n; i++) {
        uint8_t echo;
        s_echo_want = frame[i];
        if (!rx_byte(&echo, BYTE_TIMEOUT_US)) {
            s_errors++;
            s_echo_got = -1;
            return (s_why = TMC_LINK_NO_ECHO);
        }
        if (echo != frame[i]) {
            s_errors++;
            s_echo_got = (int16_t)echo;
            return (s_why = TMC_LINK_ECHO_BAD);
        }
    }
    return TMC_LINK_OK;
}

static tmc_link_t read_status(uint8_t reg, uint32_t *value)
{
    uint8_t req[4] = { SYNC, s_addr, (uint8_t)(reg & 0x7F), 0 };
    req[3] = crc8(req, 3);

    tmc_link_t st = send(req, sizeof req);
    if (st != TMC_LINK_OK)
        return st;

    uint8_t rep[8];
    for (int i = 0; i < 8; i++) {
        if (!rx_byte(&rep[i], BYTE_TIMEOUT_US)) {
            s_errors++;
            // Nothing at all is a driver that never spoke. A few bytes and
            // then silence is a driver that did, which is a different fault.
            return (s_why = i ? TMC_LINK_SHORT_REPLY : TMC_LINK_NO_REPLY);
        }
    }
    if (rep[0] != SYNC || rep[1] != REPLY_ADDR || rep[2] != (reg & 0x7F) ||
        rep[7] != crc8(rep, 7)) {
        s_errors++;
        return (s_why = TMC_LINK_BAD_FRAME);
    }
    *value = ((uint32_t)rep[3] << 24) | ((uint32_t)rep[4] << 16) |
             ((uint32_t)rep[5] << 8)  |  (uint32_t)rep[6];
    return (s_why = TMC_LINK_OK);
}

static bool read_locked(uint8_t reg, uint32_t *value)
{
    return read_status(reg, value) == TMC_LINK_OK;
}

static bool write_locked(uint8_t reg, uint32_t value)
{
    uint8_t f[8] = {
        SYNC, s_addr, (uint8_t)(reg | 0x80),
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),  (uint8_t)value, 0
    };
    f[7] = crc8(f, 7);
    if (send(f, sizeof f) != TMC_LINK_OK)
        return false;

    // IFCNT advances only for a datagram the chip actually accepted, so this
    // is the difference between "we transmitted" and "it took". A write that
    // cannot be confirmed is reported as a failure even though the bytes left
    // the pin — an unverified current setting is not worth having.
    uint32_t ifcnt;
    if (!read_locked(TMC_IFCNT, &ifcnt))
        return false;
    uint8_t got = (uint8_t)ifcnt;
    bool ok = (got == (uint8_t)(s_ifcnt + 1));
    s_ifcnt = got;
    if (!ok)
        s_errors++;
    return ok;
}

// ---- locking --------------------------------------------------------------
//
// A transaction is a couple of milliseconds of state machine FIFOs and cannot
// survive two callers interleaving. The console and the motion code both want
// it, so it is serialised — and never taken from an ISR: stepper.c's alarm
// handler touches pins only.
// Nothing may be taken before vTaskStartScheduler(): a mutex take asks for the
// current task so it can hand it the priority it inherits, and there is no
// current task yet. main() configures the driver from exactly that window, so
// this is not a corner — it is the first transaction the firmware ever runs.
static bool running(void)
{
    return s_lock && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

static bool lock(void)
{
    if (!running())
        return true;                // single-threaded: nothing to serialise
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void unlock(void)
{
    if (running())
        xSemaphoreGive(s_lock);
}

bool tmc2209_write(uint8_t reg, uint32_t value)
{
    if (!s_pio_ready || !lock()) return false;
    bool ok = write_locked(reg, value);
    unlock();
    return ok;
}

bool tmc2209_read(uint8_t reg, uint32_t *value)
{
    if (!s_pio_ready || !lock()) return false;
    bool ok = read_locked(reg, value);
    unlock();
    return ok;
}

// ---- current --------------------------------------------------------------
//
// I_rms = (CS + 1)/32 * V_fs / (R_sense + 0.02) / sqrt(2)
//
// V_fs is 0.325 V normally and 0.180 V with the vsense bit set, which is what
// makes small currents settable at all: 32 levels spread over the full scale
// are 50 mA apart, and a flap motor asked for 300 mA would land on whichever
// of them was nearest. Below half scale we drop to the low range and get the
// resolution back.
//
// The 0.02 ohm is the carrier's trace and connector resistance in series with
// the sense resistor. It is small and it is not optional: leaving it out
// overstates the current by about 15% on a 0.11 ohm carrier.
static uint8_t current_to_cs(uint16_t ma, bool *vsense)
{
    const double rs = TMC_RSENSE_OHMS + 0.02;
    double cs;

    *vsense = false;
    cs = (double)ma / 1000.0 * 1.41421356 * 32.0 * rs / 0.325 - 1.0;
    if (cs < 16.0) {                // less than half of the high range
        *vsense = true;
        cs = (double)ma / 1000.0 * 1.41421356 * 32.0 * rs / 0.180 - 1.0;
    }
    if (cs < 0.0)  cs = 0.0;
    if (cs > 31.0) cs = 31.0;
    return (uint8_t)(cs + 0.5);
}

static uint16_t cs_to_current(uint8_t cs, bool vsense)
{
    const double rs = TMC_RSENSE_OHMS + 0.02;
    double v = vsense ? 0.180 : 0.325;
    return (uint16_t)(((double)cs + 1.0) / 32.0 * v / rs / 1.41421356 * 1000.0 + 0.5);
}

bool tmc2209_set_current(uint16_t run_ma, uint16_t hold_ma)
{
    if (run_ma > TMC_MAX_CURRENT_MA || hold_ma > run_ma)
        return false;

    // One vsense bit for both, chosen by the larger: the ranges are a property
    // of the chopper, not of a register field, so run and hold cannot sit in
    // different ones.
    bool vsense;
    uint8_t irun  = current_to_cs(run_ma, &vsense);
    uint8_t ihold = (uint8_t)((uint32_t)hold_ma * (irun + 1) / (run_ma ? run_ma : 1));
    if (ihold > 31) ihold = 31;
    if (ihold)      ihold--;

    uint32_t chop = (s_chopconf & ~(1u << 17)) | (vsense ? (1u << 17) : 0);
    if (!tmc2209_write(TMC_CHOPCONF, chop))
        return false;
    s_chopconf = chop;

    s_ihold_irun = ((uint32_t)TMC_IHOLDDELAY << 16) |
                   ((uint32_t)irun << 8) | ihold;
    if (!tmc2209_write(TMC_IHOLD_IRUN, s_ihold_irun))
        return false;

    s_run_ma  = cs_to_current(irun, vsense);
    s_hold_ma = cs_to_current(ihold, vsense);
    return true;
}

void tmc2209_current(uint16_t *run_ma, uint16_t *hold_ma)
{
    if (run_ma)  *run_ma  = s_run_ma;
    if (hold_ma) *hold_ma = s_hold_ma;
}

// ---- microstepping --------------------------------------------------------

static bool mres_for(uint16_t microsteps, uint8_t *mres)
{
    switch (microsteps) {
        case 256: *mres = 0; return true;
        case 128: *mres = 1; return true;
        case 64:  *mres = 2; return true;
        case 32:  *mres = 3; return true;
        case 16:  *mres = 4; return true;
        case 8:   *mres = 5; return true;
        case 4:   *mres = 6; return true;
        case 2:   *mres = 7; return true;
        case 1:   *mres = 8; return true;
        default:  return false;
    }
}

bool tmc2209_set_microsteps(uint16_t microsteps)
{
    uint8_t mres;
    if (!mres_for(microsteps, &mres))
        return false;

    uint32_t chop = (s_chopconf & ~(0xFu << 24)) | ((uint32_t)mres << 24);
    if (!tmc2209_write(TMC_CHOPCONF, chop))
        return false;
    s_chopconf   = chop;
    s_microsteps = microsteps;
    return true;
}

uint16_t tmc2209_microsteps(void) { return s_microsteps; }

// ---- chopper mode ---------------------------------------------------------

bool tmc2209_set_stealth(bool on)
{
    uint32_t g = on ? (s_gconf & ~(1u << 2)) : (s_gconf | (1u << 2));
    if (!tmc2209_write(TMC_GCONF, g))
        return false;
    s_gconf = g;
    return true;
}

bool tmc2209_stealth(void) { return (s_gconf & (1u << 2)) == 0; }

bool tmc2209_set_shaft(bool reversed)
{
    uint32_t g = reversed ? (s_gconf | (1u << 3)) : (s_gconf & ~(1u << 3));
    if (g == s_gconf)
        return true;                // already that way: no datagram, no delay
    if (!tmc2209_write(TMC_GCONF, g))
        return false;
    s_gconf = g;
    return true;
}

bool tmc2209_shaft(void) { return (s_gconf & (1u << 3)) != 0; }

// TOFF is the chopper's off-time; zero stops it driving entirely. This is the
// driver's own enable, and it is the half of "enabled" that survives a reset
// as whatever the chip powered up with — which is why the EN pin exists.
bool tmc2209_set_chopper(bool on)
{
    // Standalone: the chopper is whatever the carrier's strapping made it, and
    // the EN pin is the enable. Succeeding here is correct — the caller has
    // already done the real work with a GPIO, and failing would stop every
    // move for the sake of a register that is not reachable.
    if (!TMC_UART_ENABLED) {
        s_chopper_on = on;
        return true;
    }

    uint32_t chop = (s_chopconf & ~0xFu) | (on ? TMC_TOFF : 0u);
    if (!tmc2209_write(TMC_CHOPCONF, chop))
        return false;
    s_chopconf   = chop;
    s_chopper_on = on;
    return true;
}

// ---- status ---------------------------------------------------------------

const char *tmc2209_status_line(char *buf, size_t n)
{
    uint32_t st;
    if (!TMC_UART_ENABLED) {
        snprintf(buf, n, "standalone STEP/DIR/EN — the driver cannot be asked "
                         "anything, so there is nothing to report");
        return buf;
    }
    if (!s_link) {
        snprintf(buf, n, "no link — the driver has never answered");
        return buf;
    }
    if (!tmc2209_read(TMC_DRV_STATUS, &st)) {
        snprintf(buf, n, "link lost — DRV_STATUS did not come back");
        return buf;
    }

    uint8_t cs = (uint8_t)((st >> 16) & 0x1F);
    snprintf(buf, n, "%s | current %u mA (CS %u)%s%s%s%s%s%s",
             (st & (1u << 31)) ? "standstill" : "moving",
             cs_to_current(cs, (s_chopconf >> 17) & 1), cs,
             (st & (1u << 0))  ? " | OVERTEMP SHUTDOWN"   : "",
             (st & (1u << 1))  ? " | over-temp warning"   : "",
             (st & (3u << 2))  ? " | SHORT TO GROUND"     : "",
             (st & (3u << 10)) ? " | SHORT"               : "",
             (st & (1u << 6))  ? " | open load A"         : "",
             (st & (1u << 7))  ? " | open load B"         : "");
    return buf;
}

uint32_t tmc2209_errors(void)  { return s_errors; }

// ---- diagnosis ------------------------------------------------------------

tmc_link_t tmc2209_probe(void)
{
    if (!s_pio_ready)
        return (s_why = TMC_LINK_NO_ECHO);
    if (!lock())
        return s_why;

    uint32_t ioin = 0;
    tmc_link_t st = read_status(TMC_IOIN, &ioin);
    unlock();

    if (st == TMC_LINK_OK)
        s_version = (uint8_t)(ioin >> 24);
    return st;
}

uint8_t tmc2209_address(void) { return s_addr; }

// Ask all four addresses who is there.
//
// MS1 and MS2 are the address bits — bit 0 and bit 1 — and whether they idle
// low depends on the carrier, not on the chip. Rather than reason about pull-
// ups, ask: three extra datagrams settle it, and a driver on the wrong address
// is indistinguishable from no driver at all until you do.
int tmc2209_scan(void)
{
    uint8_t saved = s_addr;

    for (uint8_t a = 0; a < 4; a++) {
        s_addr = a;
        if (tmc2209_probe() == TMC_LINK_OK)
            return (int)a;      // probe stored the version; s_addr stays here
    }
    s_addr = saved;
    return -1;
}

int tmc2209_loopback(uint8_t *sent, int16_t *got, int n)
{
    // Values chosen to exercise the framing rather than just the wire: 0x55
    // and 0xAA alternate every bit, 0x00 holds the line low for the whole
    // byte and 0xFF holds it high, which is what a stuck node looks like.
    static const uint8_t pattern[] = { 0x55, 0xAA, 0x00, 0xFF, 0x05, 0xF0 };
    if (!s_pio_ready)
        return 0;
    if (n > (int)sizeof pattern)
        n = (int)sizeof pattern;
    if (!lock())
        return 0;

    rx_drain();
    for (int i = 0; i < n; i++)
        tx_byte(pattern[i]);
    for (int i = 0; i < n; i++) {
        uint8_t b;
        sent[i] = pattern[i];
        got[i]  = rx_byte(&b, BYTE_TIMEOUT_US) ? (int16_t)b : (int16_t)-1;
    }
    unlock();
    return n;
}

const char *tmc2209_why(void)
{
    static char buf[1800];

    switch (s_why) {
    case TMC_LINK_OK:
        snprintf(buf, sizeof buf, "the link is up.");
        break;

    case TMC_LINK_NO_ECHO:
        snprintf(buf, sizeof buf,
            "OUR SIDE OF THE WIRE. We put 0x%02X on GP%d and GP%d heard\n"
            "          nothing — and that loop closes on this board, through the\n"
            "          resistor, with or without a driver attached. So this is\n"
            "          not the driver being silent. Either the two pins are not\n"
            "          actually joined, or something is holding the line:\n"
            "\n"
            "            - the 1k is missing, or it bridges the wrong two pads\n"
            "            - GP%d is not on the junction\n"
            "            - PDN_UART is strapped to GND on the carrier for\n"
            "              standalone mode. Many boards ship that way, and a\n"
            "              1k cannot pull against it. Look for a solder blob\n"
            "              or a jumper by the PDN pad and remove it.\n"
            "            - the driver is unpowered and clamping the node\n"
            "              through its protection diodes. Unplug it from the\n"
            "              wire and run 'tmc wire' again: if the echo comes\n"
            "              back with the driver disconnected, that is your\n"
            "              answer and the fix is VDD, not the UART.\n"
            "\n"
            "          'tmc wire' shows the pattern byte for byte.",
            s_echo_want, PIN_TMC_TX, PIN_TMC_RX, PIN_TMC_RX);
        break;

    case TMC_LINK_ECHO_BAD:
        snprintf(buf, sizeof buf,
            "SOMETHING ELSE IS DRIVING THE LINE. We sent 0x%02X and read\n"
            "          back 0x%02X. Our own byte should return unchanged, so\n"
            "          another output is fighting ours:\n"
            "\n"
            "            - the 1k between GP%d and the pad is missing, so our\n"
            "              transmitter and the driver's are shorted together\n"
            "            - the node is shared with something it should not be\n"
            "\n"
            "          'tmc wire' shows the pattern byte for byte.",
            s_echo_want, (unsigned)(s_echo_got & 0xFF), PIN_TMC_TX);
        break;

    case TMC_LINK_NO_REPLY:
        snprintf(buf, sizeof buf,
            "THE DRIVER IS SILENT — but our side is proven good. The echo\n"
            "          came back clean, so GP%d, GP%d, the resistor and the PIO\n"
            "          all work. What is left is the wire beyond them, or the\n"
            "          driver itself, in the order worth checking:\n"
            "\n"
            "            1. VM. THE MOTOR SUPPLY. The TMC2209 does not answer\n"
            "               on logic power alone — not slowly, not partially,\n"
            "               not at all. This is the usual one by a wide margin,\n"
            "               and it looks exactly like a broken wire.\n"
            "            2. VDD / VIO on the carrier, from this board's 3V3.\n"
            "               Separate from VM, and just as necessary.\n"
            "            3. THE JUMPER ON THE DRIVER. This is a hardware mod,\n"
            "               not a wiring choice, and it is why correct wiring\n"
            "               and correct code still give silence. Watterott\n"
            "               SilentStepSticks ship with the UART line NOT\n"
            "               connected to the header: a solder jumper has to be\n"
            "               bridged from the middle pad to the UART position\n"
            "               first. Other carriers do the same with a pad marked\n"
            "               UART or USART whose jumper is unpopulated. Look at\n"
            "               the board before believing any wire.\n"
            "            4. The wire from the GP%d/GP%d junction to that pad.\n"
            "               With it missing our echo still works perfectly —\n"
            "               which is exactly what we are seeing.\n"
            "            5. A shared ground between this board and the driver.\n"
            "            6. The address. We are addressing driver %d. MS1 is\n"
            "               bit 0 and MS2 is bit 1, so four are possible and a\n"
            "               driver on the wrong one is silent exactly like a\n"
            "               driver that is not there. Do not reason about it:\n"
            "               'tmc scan' asks all four.",
            PIN_TMC_TX, PIN_TMC_RX, PIN_TMC_TX, PIN_TMC_RX, TMC_UART_ADDRESS);
        break;

    case TMC_LINK_SHORT_REPLY:
        snprintf(buf, sizeof buf,
            "THE DRIVER STARTED ANSWERING AND STOPPED. It is there and it\n"
            "          is powered — this is a signal-integrity fault, not a\n"
            "          wiring one. Shorten the lead, or drop TMC_UART_BAUD.");
        break;

    case TMC_LINK_BAD_FRAME:
        snprintf(buf, sizeof buf,
            "THE DRIVER ANSWERED, BUT THE FRAME DID NOT CHECK OUT — wrong\n"
            "          sync byte, wrong address, or a bad CRC. The link is very\n"
            "          nearly working. Suspect the 1k first, then the lead\n"
            "          length, then TMC_UART_BAUD.");
        break;
    }
    return buf;
}
bool     tmc2209_link_ok(void) { return s_link; }
uint8_t  tmc2209_version(void) { return s_version; }

// ---- bring-up -------------------------------------------------------------

bool tmc2209_init(void)
{
    if (!TMC_UART_ENABLED) {
        // Nothing to bring up, and nothing to fail. Say so once so the boot
        // log is not silent about a whole subsystem being switched off.
        printf("[tmc] standalone STEP/DIR/EN on GP%d/GP%d/GP%d: %u uSteps"
               " (strapped), current from VREF\n",
               PIN_STEP, PIN_DIR, PIN_EN, TMC_MICROSTEPS);
        return true;
    }

    if (!s_pio_ready) {
        s_pio    = TMC_UART_PIO;
        s_off_tx = pio_add_program(s_pio, &tmc_uart_tx_program);
        s_off_rx = pio_add_program(s_pio, &tmc_uart_rx_program);
        s_sm_tx  = (uint)pio_claim_unused_sm(s_pio, true);
        s_sm_rx  = (uint)pio_claim_unused_sm(s_pio, true);
        tmc_uart_tx_program_init(s_pio, s_sm_tx, s_off_tx, PIN_TMC_TX, TMC_UART_BAUD);
        tmc_uart_rx_program_init(s_pio, s_sm_rx, s_off_rx, PIN_TMC_RX, TMC_UART_BAUD);
        s_lock = xSemaphoreCreateMutex();
        s_pio_ready = true;
    }

    s_link    = false;
    s_version = 0;

    // IOIN carries the silicon version in its top byte and nothing we wrote,
    // so it answers "is a TMC2209 on the other end of this wire" without
    // having configured anything first.
    for (int attempt = 0; attempt < 3 && !s_link; attempt++)
        s_link = (tmc2209_probe() == TMC_LINK_OK);

    if (!s_link) {
        printf("[tmc] %s\n", tmc2209_why());
        return false;
    }
    if (s_version != 0x21)
        printf("[tmc] version 0x%02X, expected 0x21 — this may not be a TMC2209\n",
               s_version);

    uint32_t ifcnt = 0;
    tmc2209_read(TMC_IFCNT, &ifcnt);
    s_ifcnt = (uint8_t)ifcnt;

    // GSTAT latches "I was reset" and the two fault flags; writing 1s clears
    // them, so anything still set afterwards happened after we looked.
    tmc2209_write(TMC_GSTAT, 0x07);

    //   pdn_disable      — PDN_UART is a serial line now, not a power-down input
    //   mstep_reg_select — microsteps come from MRES, not from the MS pins
    //   multistep_filt   — smooths an irregular STEP rate for StealthChop
    //   I_scale_analog=0 — ignore the VREF trimmer; current is IHOLD_IRUN alone
    s_gconf = (1u << 6) | (1u << 7) | (1u << 8);
    if (!tmc2209_write(TMC_GCONF, s_gconf)) {
        printf("[tmc] GCONF would not take — the link answers but will not accept\n"
               "      writes. Check the 1k resistor: without it our TX and the\n"
               "      driver's output fight and the CRC never survives.\n");
        s_link = false;
        return false;
    }

    // Chopper off to begin with. Two independent things have to agree before
    // current flows, and neither of them is set here.
    uint8_t mres;
    mres_for(TMC_MICROSTEPS, &mres);
    s_chopconf = ((uint32_t)mres << 24) | (1u << 28) |      // intpol: always to 1/256
                 ((uint32_t)TMC_TBL << 15) |
                 ((uint32_t)TMC_HEND << 7) | ((uint32_t)TMC_HSTRT << 4) | 0u;
    s_microsteps = TMC_MICROSTEPS;
    s_chopper_on = false;
    tmc2209_write(TMC_CHOPCONF, s_chopconf);

    tmc2209_write(TMC_TPOWERDOWN, TMC_TPOWERDOWN_TICKS);
    tmc2209_write(TMC_TPWMTHRS, 0);         // StealthChop at every speed
    tmc2209_write(TMC_VACTUAL, 0);          // motion comes from STEP, not from here
    tmc2209_set_stealth(true);
    tmc2209_set_current(TMC_RUN_CURRENT_MA, TMC_HOLD_CURRENT_MA);

    printf("[tmc] TMC2209 (version 0x%02X) on GP%d/GP%d: %u uSteps, run %u mA,\n"
           "      hold %u mA, StealthChop\n",
           s_version, PIN_TMC_TX, PIN_TMC_RX, s_microsteps, s_run_ma, s_hold_ma);
    return true;
}
