//
// wire — see wire.h.
//
#include "wire.h"
#include "bus.h"
#include "desk_frame.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/uart.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

static volatile bool     s_live;
static volatile bool     s_warned;
static volatile uint32_t s_frames;
static volatile uint8_t  s_pending;     // what the flap task wants sent
static volatile uint32_t s_burst;       // frames of it left; 0 = held until changed

void wire_send(uint8_t code)
{
    if (!s_live)
        return;

    // Checksum generated, never table-driven, so a new code cannot ship with
    // a stale one. 16-bit big-endian sum of the first six bytes.
    uint8_t f[DESK_FRAME_LEN] = { 0xAA, 0x55, 0x08, 0x01, 0x00, code, 0, 0 };
    uint16_t sum = 0;
    for (int i = 0; i < 6; i++)
        sum += f[i];
    f[6] = (uint8_t)(sum >> 8);
    f[7] = (uint8_t)(sum & 0xFF);

    uart_write_blocking(UART_KEYS, f, sizeof f);    // 32-byte FIFO: no wait
    s_frames++;
}

void wire_set_pending(uint8_t code)
{
    s_burst   = 0;                      // cleared first: a held code is not a burst
    s_pending = code;
}

void wire_send_once(uint8_t code)
{
    s_pending = code;
    s_burst   = 1;
}

uint32_t wire_burst_left(void)  { return s_burst; }
uint8_t  wire_pending_key(void) { return s_pending; }

uint8_t wire_next_key(void)
{
    uint8_t code = s_pending;
    if (s_burst && --s_burst == 0)
        s_pending = 0x00;               // spent: back to the idle poll
    return code;
}

bool     wire_live(void)   { return s_live; }
uint32_t wire_frames(void) { return s_frames; }

void wire_task(void *arg)
{
    (void)arg;

    // High-Z until the panel is heard, so a reset or a half-flashed board
    // never drives the board's RX line.
    gpio_init(PIN_TO_BOARD);
    gpio_set_dir(PIN_TO_BOARD, GPIO_IN);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DESK_POLL_MS));
        if (s_live)
            continue;

        uint32_t panel_ok;
        bus_counts(NULL, &panel_ok);
        if (panel_ok) {
            gpio_set_function(PIN_TO_BOARD, UART_FUNCSEL_NUM(UART_KEYS, PIN_TO_BOARD));
            s_live = true;
            printf("[bus] panel heard — GP%d claimed, the desk can move\n", PIN_TO_BOARD);
        } else if (!s_warned) {
            s_warned = true;
            printf("[bus] NOT transmitting: nothing received from the panel yet.\n"
                   "      GP%d stays high-Z until a valid frame arrives, because\n"
                   "      driving a line we cannot hear is how two outputs end up\n"
                   "      fighting on one wire.\n"
                   "      Check the ground first — one missing ground leaves both\n"
                   "      lines silent while panel and board still talk to each\n"
                   "      other perfectly. Then check GP%d is on the panel pin\n"
                   "      marked RX (its labels are SWAPPED; see docs/wiring.md).\n",
                   PIN_TO_BOARD, PIN_FROM_PANEL);
        }
    }
}
