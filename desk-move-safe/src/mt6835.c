//
// mt6835 — SPI driver for the magnetic angle sensor. See mt6835.h.
//
#include "mt6835.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"

#include <stdio.h>
#include <string.h>

// The top nibble of every frame.
#define OP_READ     0x3
#define OP_WRITE    0x6
#define OP_PROG     0xC     // program EEPROM
#define OP_ZERO     0x5     // set zero to the current angle
#define OP_ANGLE    0xA     // burst read starting at ANGLE1

#define WRITE_ACK   0x55

static bool     s_present;
static uint32_t s_crc_errors;

static inline void cs(bool select)
{
    gpio_put(MT6835_PIN_CS, !select);       // active low
}

// CRC-8 over the 21 angle bits and 3 status bits, exactly as the chip computes
// it: polynomial x^8 + x^2 + x + 1 (0x07), init 0, no reflection, no final xor.
static uint8_t crc8(const uint8_t *b, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

// One CS-framed exchange. Full duplex: every byte clocked out brings one in,
// and the chip puts its reply in the byte positions after the command.
static void xfer(uint8_t *buf, size_t n)
{
    cs(true);
    spi_write_read_blocking(MT6835_SPI_PORT, buf, buf, n);
    cs(false);
    // The datasheet asks for CS to rest high between frames; at 1 MHz a
    // microsecond is several bit times, cheap insurance against back-to-back
    // frames merging.
    sleep_us(1);
}

// The 24-bit command frame: op | addr_hi, addr_lo, data. The reply byte is
// where the data went.
static uint8_t frame(uint8_t op, uint16_t reg, uint8_t data)
{
    uint8_t buf[3] = {
        (uint8_t)((op << 4) | ((reg >> 8) & 0x0F)),
        (uint8_t)(reg & 0xFF),
        data,
    };
    xfer(buf, sizeof buf);
    return buf[2];
}

uint8_t mt6835_read_reg(uint16_t reg)
{
    return frame(OP_READ, reg, 0x00);
}

bool mt6835_write_reg(uint16_t reg, uint8_t value)
{
    return frame(OP_WRITE, reg, value) == WRITE_ACK;
}

bool mt6835_angle(uint32_t *angle, uint8_t *status)
{
    // Command, address, then four bytes clocked out: ANGLE1..3 and the CRC.
    uint8_t buf[6] = { (uint8_t)(OP_ANGLE << 4), MT6835_REG_ANGLE1, 0, 0, 0, 0 };
    xfer(buf, sizeof buf);

    // MISO is pulled up, so a missing sensor delivers 0xFF 0xFF 0xFF 0xFF — and
    // the CRC of three 0xFF bytes is 0x0F, not 0xFF, so that case fails here
    // deterministically rather than being read as a valid angle.
    if (crc8(&buf[2], 3) != buf[5]) {
        s_crc_errors++;
        return false;
    }
    *angle  = ((uint32_t)buf[2] << 13) | ((uint32_t)buf[3] << 5) | (buf[4] >> 3);
    *status = buf[4] & 0x07;
    return true;
}

uint32_t mt6835_crc_errors(void) { return s_crc_errors; }

bool mt6835_init(void)
{
    gpio_init(MT6835_PIN_CS);
    gpio_set_dir(MT6835_PIN_CS, GPIO_OUT);
    cs(false);

    spi_init(MT6835_SPI_PORT, MT6835_SPI_BAUD);
    spi_set_format(MT6835_SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(MT6835_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(MT6835_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(MT6835_PIN_MISO, GPIO_FUNC_SPI);
    // See mt6835_angle(): this is what makes "no sensor" a CRC failure rather
    // than whatever the floating line happened to say.
    gpio_pull_up(MT6835_PIN_MISO);

    // A frame straight after power-up can catch the chip mid-boot; the second
    // attempt is the one that counts.
    uint32_t a; uint8_t st;
    s_present = mt6835_angle(&a, &st) || mt6835_angle(&a, &st);
    return s_present;
}

bool mt6835_present(void) { return s_present; }

const char *mt6835_status_str(uint8_t status)
{
    static char buf[40];
    if (!(status & 0x07))
        return "ok";
    buf[0] = '\0';
    if (status & MT6835_ST_WEAK)      strcat(buf, "WEAK FIELD ");
    if (status & MT6835_ST_OVERSPEED) strcat(buf, "OVERSPEED ");
    if (status & MT6835_ST_UNDERVOLT) strcat(buf, "UNDERVOLT ");
    buf[strlen(buf) - 1] = '\0';        // trailing space
    return buf;
}

// ---- decoded settings ----------------------------------------------------

uint16_t mt6835_abz_res(void)
{
    uint8_t hi = mt6835_read_reg(MT6835_REG_ABZ_RES1);
    uint8_t lo = mt6835_read_reg(MT6835_REG_ABZ_RES2);
    return (uint16_t)((hi << 6) | (lo >> 2));
}

bool mt6835_abz_enabled(void)
{
    return !(mt6835_read_reg(MT6835_REG_ABZ_RES2) & 0x02);     // ABZ_OFF
}

uint16_t mt6835_zero_pos(void)
{
    uint8_t hi = mt6835_read_reg(MT6835_REG_ZERO1);
    uint8_t lo = mt6835_read_reg(MT6835_REG_ZERO2);
    return (uint16_t)((hi << 4) | (lo >> 4));
}

bool    mt6835_rot_dir(void)    { return (mt6835_read_reg(MT6835_REG_DIR_HYST) >> 3) & 1; }
uint8_t mt6835_hysteresis(void) { return  mt6835_read_reg(MT6835_REG_DIR_HYST) & 0x07; }
uint8_t mt6835_bandwidth(void)  { return  mt6835_read_reg(MT6835_REG_BW) & 0x07; }
uint8_t mt6835_cal_status(void) { return  mt6835_read_reg(MT6835_REG_CAL_STATUS) >> 6; }

// Read-modify-write, so the bits sharing the byte are left as they were.
static bool update_reg(uint16_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v = mt6835_read_reg(reg);
    v = (uint8_t)((v & ~mask) | (value & mask));
    return mt6835_write_reg(reg, v);
}

bool mt6835_set_rot_dir(bool dir)
{
    return update_reg(MT6835_REG_DIR_HYST, 0x08, dir ? 0x08 : 0x00);
}

bool mt6835_set_hysteresis(uint8_t hyst)
{
    return update_reg(MT6835_REG_DIR_HYST, 0x07, hyst & 0x07);
}

bool mt6835_set_bandwidth(uint8_t bw)
{
    return update_reg(MT6835_REG_BW, 0x07, bw & 0x07);
}

bool mt6835_set_zero_here(void)
{
    return frame(OP_ZERO, 0x000, 0x00) == WRITE_ACK;
}

bool mt6835_program_eeprom(void)
{
    sleep_ms(1);                        // the datasheet's minimum gap before PROG
    return frame(OP_PROG, 0x000, 0x00) == WRITE_ACK;
}
