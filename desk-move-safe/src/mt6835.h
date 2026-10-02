#ifndef MT6835_H
#define MT6835_H
//
// mt6835 — 21-bit magnetic rotary position sensor on SPI.
//
// A diametrically-magnetised magnet sits on the motor shaft, a millimetre or so
// above the chip; the chip reports its angle as 0..2097151 over one turn. That
// makes it an absolute encoder within a revolution, and — once the wrap-around
// is tracked, which is what encoder.h does on top of this — a way to find out
// whether the motor actually went where the STEP pulses told it to.
//
// This is the whole point of pairing it with the stepper: open-loop step/dir has
// no idea when it has lost steps. A stall looks identical to a successful move
// from the firmware's side. With the shaft measured the difference is a number.
//
// The protocol is a 24-bit SPI frame: a 4-bit opcode, a 12-bit register
// address, and one data byte in either direction. Angle reads use a burst
// opcode that clocks the three angle bytes and a CRC out in one transaction, so
// a sample is atomic — no register-by-register tearing. Mode 3 (clock idles
// high, data sampled on the rising edge), MSB first, CS driven by GPIO.
//
// The chip is rated to 16 MHz and 120 000 rpm. Neither matters here: the rotor
// on this rig is slow and the bus is short flying leads, so the clock is set
// well below the limit in board_config.h. Every call blocks for one
// transaction — about 50 us at 1 MHz — and none belongs in an ISR.
//
#include <stdbool.h>
#include <stdint.h>

#define MT6835_BITS     21
#define MT6835_CPR      (1u << MT6835_BITS)     // 2 097 152 counts per revolution

// Status bits, three of them, delivered with every angle sample. All zero is
// the healthy state.
//   OVERSPEED   the shaft turned faster than the chip can track — not
//               reachable on a stepper, so if it sets, suspect the wiring
//   WEAK        magnetic field too weak: magnet too far, too small, or not
//               diametrically magnetised. THE placement indicator on this part
//   UNDERVOLT   supply dipped below the chip's threshold
#define MT6835_ST_OVERSPEED  0x01
#define MT6835_ST_WEAK       0x02
#define MT6835_ST_UNDERVOLT  0x04

// Bring up the SPI port and confirm a sensor answers with a frame whose CRC
// checks. Safe to call when no sensor is fitted — it returns false, and the
// firmware runs open-loop.
bool mt6835_init(void);
bool mt6835_present(void);

// One burst read: 21-bit angle and the three status bits, CRC-verified. Returns
// false — and counts it — when the CRC does not match, which is what a
// disconnected MISO, a flaky lead, or a clock too fast for the wiring looks
// like. On false the outputs are untouched.
bool     mt6835_angle(uint32_t *angle, uint8_t *status);
uint32_t mt6835_crc_errors(void);

// Human-readable status: "ok", or the set bits joined, e.g. "WEAK FIELD".
// Static buffer; not reentrant.
const char *mt6835_status_str(uint8_t status);

// ---- registers -----------------------------------------------------------
// Raw access, for poking at the configuration from the console. Writes land in
// the chip's working registers and are lost at power-off unless followed by
// mt6835_program_eeprom(). A write answers with an ACK byte; false means it
// was not acknowledged.
uint8_t mt6835_read_reg(uint16_t reg);
bool    mt6835_write_reg(uint16_t reg, uint8_t value);

#define MT6835_REG_USER_ID      0x001
#define MT6835_REG_ANGLE1       0x003       // ANGLE[20:13]
#define MT6835_REG_ANGLE2       0x004       // ANGLE[12:5]
#define MT6835_REG_ANGLE3       0x005       // ANGLE[4:0] STATUS[2:0]
#define MT6835_REG_CRC          0x006
#define MT6835_REG_ABZ_RES1     0x007       // ABZ_RES[13:6]
#define MT6835_REG_ABZ_RES2     0x008       // ABZ_RES[5:0] ABZ_OFF AB_SWAP
#define MT6835_REG_ZERO1        0x009       // ZERO[11:4]
#define MT6835_REG_ZERO2        0x00A       // ZERO[3:0] Z_EDGE Z_PUL_WID[2:0]
#define MT6835_REG_UVW          0x00B       // Z_PHASE[1:0] UVW_MUX UVW_OFF UVW_RES[3:0]
#define MT6835_REG_PWM          0x00C       // NLC_EN PWM_FQ PWM_POL PWM_SEL[2:0]
#define MT6835_REG_DIR_HYST     0x00D       // ROT_DIR HYST[2:0]
#define MT6835_REG_AUTOCAL      0x00E       // GPIO_DS AUTOCAL_FREQ[2:0]
#define MT6835_REG_BW           0x011       // BW[2:0]
#define MT6835_REG_CAL_STATUS   0x113       // [7:6]: 0 none, 1 running, 2 failed, 3 ok

// Decoded views of the settings worth knowing about on a bench.
uint16_t mt6835_abz_res(void);          // ABZ output: this + 1 pulses per turn
bool     mt6835_abz_enabled(void);
uint16_t mt6835_zero_pos(void);         // 12-bit zero offset, 0..4095 over a turn
bool     mt6835_rot_dir(void);          // which way the angle increases
uint8_t  mt6835_hysteresis(void);       // 0..7, see the datasheet table
uint8_t  mt6835_bandwidth(void);        // 0..7, output filter; higher = faster
uint8_t  mt6835_cal_status(void);       // 0..3 as in MT6835_REG_CAL_STATUS

// Direction is a register bit on this part, not a strapped pin as on simpler
// sensors — so a count running backwards relative to the stepper is fixed from
// the console rather than with a soldering iron.
bool mt6835_set_rot_dir(bool dir);
bool mt6835_set_hysteresis(uint8_t hyst);
bool mt6835_set_bandwidth(uint8_t bw);

// Make the current shaft angle read as zero: the chip copies its present angle
// into the ZERO registers. Volatile until programmed.
bool mt6835_set_zero_here(void);

// Burn the working registers into EEPROM so they survive a power cycle. The
// chip needs about six seconds of uninterrupted power afterwards, and the new
// values take effect on the next power-up, not immediately. EEPROM endurance
// is finite; this is not something to do in a loop.
bool mt6835_program_eeprom(void);

#endif // MT6835_H
