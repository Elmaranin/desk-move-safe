#ifndef TMC2209_H
#define TMC2209_H
//
// tmc2209 — the stepper driver's register interface, over one wire.
//
// WHAT MOVED TO SOFTWARE. Everything the driver knows except the step clock:
//
//   run and hold current      was the VREF trimmer — now a number in mA
//   microstep resolution      was the MS1/MS2 jumpers (which in UART mode
//                             become the chip's address bits instead)
//   StealthChop / SpreadCycle was the pin strapping
//   standstill power-down     was PDN_UART's own pin function
//   direction                 was the DIR pin — now GCONF.shaft
//   enable                    was the EN pin — now CHOPCONF.TOFF
//   fault status              was nothing at all — there was no way to ask
//
// Four signal wires became two, and the two that remain are STEP and this one.
// Both pins can be put back one at a time with TMC_USE_DIR_PIN and
// TMC_USE_EN_PIN in board_config.h, which is also where the trade each one
// makes is written down.
//
// WHAT DID NOT MOVE, AND WHY. STEP. UART's only motion primitive is VACTUAL, a
// constant velocity with no ramp, and a ramp built out of VACTUAL writes means
// position is the integral of a velocity rather than a count of steps that
// definitely happened. Everything the travel limits will do — the step span,
// the backlash figure, creeping the last few degrees under the encoder — rests
// on that count being exact. STEP is a clock, not a parameter.
//
// THE LINK IS CHECKED, NOT ASSUMED. Reading IOIN gives the chip's version byte
// (0x21 for a TMC2209), which no amount of wishful wiring can fake, and every
// write is confirmed against IFCNT — a counter the chip increments only on a
// datagram whose CRC it accepted. A driver that cannot be talked to refuses to
// be enabled, so a broken wire is a motor that does not move rather than a
// motor running at whatever current the chip happened to power up with.
//
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Claim the PIO state machines, check the link, and write the working
// configuration. Returns false if the driver did not answer; the console says
// what to look at. Safe to call again to re-establish the link.
bool tmc2209_init(void);

bool     tmc2209_link_ok(void);
uint8_t  tmc2209_version(void);     // 0x21 = TMC2209. 0 = never answered.

// HOW a transaction failed, which is most of the diagnosis.
//
// The dividing line is the echo. Our transmit pin reaches the driver's pad
// through a resistor and our receive pin sits on that same node, so every byte
// we send comes back to us BEFORE any reply does — and it comes back whether
// or not a driver is attached, because the loop is closed on our own board.
//
//   the echo returns       everything on our side works; look at the wire to
//                          the driver, its power, and its address
//   the echo does not      the fault is on our side, or something is holding
//                          the line: the two pins are not actually joined, the
//                          resistor is missing, or the node is clamped
//
// That is the whole reason the echo is checked rather than discarded.
typedef enum {
    TMC_LINK_OK = 0,
    TMC_LINK_NO_ECHO,       // our own bytes never came back
    TMC_LINK_ECHO_BAD,      // they came back altered
    TMC_LINK_NO_REPLY,      // echo clean; the driver said nothing
    TMC_LINK_SHORT_REPLY,   // it began answering and stopped
    TMC_LINK_BAD_FRAME,     // it answered, but sync/address/CRC did not check
} tmc_link_t;

// One read of IOIN, reporting exactly how it went. Does not configure
// anything, so it is safe to call at any time.
tmc_link_t tmc2209_probe(void);

// Several lines explaining the last failure, and what to look at for THAT
// failure rather than for all of them at once.
const char *tmc2209_why(void);

// Ask all four addresses who is there, and stay on the one that answers.
// Returns the address, or -1 if none did. MS1 and MS2 are the address bits —
// bit 0 and bit 1 — and carriers disagree about how they idle, so this is
// faster and more certain than working it out from the schematic.
int     tmc2209_scan(void);
uint8_t tmc2209_address(void);

// Bare loopback: transmit a known pattern and report what came back, byte for
// byte. Needs no driver at all — the resistor closes the loop on our board —
// so it separates "our two pins and the PIO work" from everything else.
// Returns how many bytes were compared; `got` is -1 where nothing arrived.
int tmc2209_loopback(uint8_t *sent, int16_t *got, int n);

// Raw register access. Both confirm the CRC; a write is verified by IFCNT
// having advanced by exactly one.
bool tmc2209_write(uint8_t reg, uint32_t value);
bool tmc2209_read(uint8_t reg, uint32_t *value);

// ---- the settings that used to be hardware -------------------------------

// RMS current per phase, in milliamps. The driver resolves it to one of 32
// levels, so the value read back is the one actually in effect, not the one
// asked for. Hold current applies after TPOWERDOWN of standstill.
bool tmc2209_set_current(uint16_t run_ma, uint16_t hold_ma);
void tmc2209_current(uint16_t *run_ma, uint16_t *hold_ma);

// 1, 2, 4, 8, 16, 32, 64, 128 or 256. Rejected while the motor is moving: a
// step would change meaning in the middle of a ramp.
bool     tmc2209_set_microsteps(uint16_t microsteps);
uint16_t tmc2209_microsteps(void);

// StealthChop is quiet and has less torque at speed; SpreadCycle is the
// opposite. A flap on a desk should be heard as little as possible, so this
// boots StealthChop.
bool tmc2209_set_stealth(bool on);
bool tmc2209_stealth(void);

// The chopper on or off — the driver's own idea of enabled. With TMC_USE_EN_PIN
// this is one of two things that must agree before current flows; without it,
// this IS the enable. stepper.c drives it.
bool tmc2209_set_chopper(bool on);

// Which way the motor turns: GCONF.shaft, the register that replaces the DIR
// pin. Only legal at a standstill — the driver reverses on the next step edge,
// so calling this mid-move turns a move around. stepper.c is the only caller,
// and it checks. A redundant call costs nothing; the register is only written
// when the direction actually changes.
bool tmc2209_set_shaft(bool reversed);
bool tmc2209_shaft(void);

// ---- what the driver has to say ------------------------------------------

// One line from DRV_STATUS: temperature warnings, short and open-load
// detection, standstill, and the current the chip says it is using. Returns
// buf. Prints why it could not ask, rather than silence, if the link is down.
const char *tmc2209_status_line(char *buf, size_t n);

uint32_t tmc2209_errors(void);      // CRC failures + timeouts since boot

// Register numbers, for the console's raw read/write and for status decoding.
#define TMC_GCONF       0x00
#define TMC_GSTAT       0x01
#define TMC_IFCNT       0x02
#define TMC_IOIN        0x06
#define TMC_IHOLD_IRUN  0x10
#define TMC_TPOWERDOWN  0x11
#define TMC_TSTEP       0x12
#define TMC_TPWMTHRS    0x13
#define TMC_TCOOLTHRS   0x14
#define TMC_VACTUAL     0x22
#define TMC_SGTHRS      0x40
#define TMC_SG_RESULT   0x41
#define TMC_COOLCONF    0x42
#define TMC_MSCNT       0x6A
#define TMC_CHOPCONF    0x6C
#define TMC_DRV_STATUS  0x6F
#define TMC_PWMCONF     0x70

#endif // TMC2209_H
