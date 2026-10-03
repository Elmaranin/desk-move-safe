//
// bus — see bus.h.
//
// Polling, not interrupts: at 9600 baud a line delivers at most ~1 byte/ms and
// each UART has a 32-byte RX FIFO, so a 5 ms poll can never overrun.
//
#include "bus.h"
#include "desk_frame.h"
#include "flap.h"
#include "wire.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/uart.h"

#include "FreeRTOS.h"
#include "task.h"

static desk_parser_t s_board;       // 55 AA — height reports
static desk_parser_t s_panel;       // AA 55 — key reports

static volatile bool       s_have_height;
static volatile uint16_t   s_height_mm;
static volatile TickType_t s_height_tick;

static volatile bool       s_have_key;
static volatile uint8_t    s_key;
static volatile TickType_t s_key_tick;

static volatile int8_t   s_dir;
static volatile uint32_t s_dir_us;

static volatile uint16_t s_target_mm;
static volatile bool     s_have_target;
static volatile uint32_t s_target_gen;  // +1 per announcement

static uint16_t s_prev_mm;
static uint32_t s_prev_us;
static bool     s_have_prev;
static uint8_t  s_reject_run;

static void tap_init(uart_inst_t *u, uint pin_rx)
{
    uart_init(u, DESK_BAUD);
    uart_set_format(u, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(u, false, false);
    gpio_set_function(pin_rx, UART_FUNCSEL_NUM(u, pin_rx));
    // Idle UART is high; the pull-up stops an unplugged lead spewing frames.
    gpio_pull_up(pin_rx);
}

static void on_height(uint16_t mm, uint32_t now_us)
{
    // A jump too large to be mechanical is the board announcing where it
    // INTENDS to go, not reporting where the desk is. Hold the last real
    // height and keep the announced one: it is a preset's destination, a
    // second before the desk moves, and the longest look-ahead this bus gives.
    if (s_have_prev) {
        int32_t d = (int32_t)mm - (int32_t)s_prev_mm;
        if (d < 0) d = -d;
        if (d > DESK_MAX_STEP_MM) {
            if (++s_reject_run <= DESK_ANNOUNCE_MAX_FRAMES) {
                s_target_mm   = mm;
                s_have_target = true;
                s_target_gen++;
                return;                 // not a height
            }
            // Said so for too long to be an announcement: the stream and our
            // idea of it have genuinely diverged, so believe it.
        }
    }
    s_reject_run = 0;

    taskENTER_CRITICAL();
    s_height_mm   = mm;
    s_height_tick = xTaskGetTickCount();
    s_have_height = true;
    taskEXIT_CRITICAL();

    if (!s_have_prev) {
        s_prev_mm = mm; s_prev_us = now_us; s_have_prev = true;
        return;
    }
    if (mm == s_prev_mm)
        return;                         // holding station

    s_dir    = (mm > s_prev_mm) ? 1 : -1;
    s_dir_us = now_us;
    s_prev_mm = mm;
    s_prev_us = now_us;
}

static void drain_board(void)
{
    while (uart_is_readable(UART_HEIGHT)) {
        if (desk_parser_feed(&s_board, uart_getc(UART_HEIGHT)) != DESK_FEED_FRAME)
            continue;
        if (desk_frame_type(&s_board) != DESK_TYPE_HEIGHT)
            continue;
        uint16_t mm = desk_frame_height_mm(&s_board);
        if (mm >= DESK_HEIGHT_SANE_MIN_MM && mm <= DESK_HEIGHT_SANE_MAX_MM)
            on_height(mm, time_us_32());
        // Masked and still nonsense: the field carries something we do not
        // decode. Never acted on.
    }
}

static void drain_panel(void)
{
    while (uart_is_readable(UART_KEYS)) {
        if (desk_parser_feed(&s_panel, uart_getc(UART_KEYS)) != DESK_FEED_FRAME)
            continue;
        if (desk_frame_type(&s_panel) != DESK_TYPE_KEY)
            continue;

        uint8_t key = desk_frame_key(&s_panel);
        taskENTER_CRITICAL();
        s_key      = key;
        s_key_tick = xTaskGetTickCount();
        s_have_key = true;
        taskEXIT_CRITICAL();

        // The decision sits here, in the broken wire, at the moment a panel
        // frame completes. Nothing between the panel and the board is faster,
        // and this is the highest-priority task, so the desk is answered
        // within one frame time.
        uint8_t out = flap_decide(key);
        if (out != WIRE_NO_FRAME)
            wire_send(out);
    }
}

void bus_task(void *arg)
{
    (void)arg;
    desk_parser_init(&s_board, 0x55, 0xAA);
    desk_parser_init(&s_panel, 0xAA, 0x55);
    tap_init(UART_HEIGHT, PIN_FROM_BOARD);
    tap_init(UART_KEYS,   PIN_FROM_PANEL);

    for (;;) {
        drain_board();
        drain_panel();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---- getters --------------------------------------------------------------

static uint32_t age_of(TickType_t then)
{
    return (uint32_t)((xTaskGetTickCount() - then) * portTICK_PERIOD_MS);
}

bool desk_height_mm(uint16_t *mm, uint32_t *age_ms)
{
    taskENTER_CRITICAL();
    bool have = s_have_height; uint16_t v = s_height_mm; TickType_t t = s_height_tick;
    taskEXIT_CRITICAL();
    if (!have) return false;
    if (mm)     *mm = v;
    if (age_ms) *age_ms = age_of(t);
    return true;
}

bool desk_last_key(uint8_t *code, uint32_t *age_ms)
{
    taskENTER_CRITICAL();
    bool have = s_have_key; uint8_t v = s_key; TickType_t t = s_key_tick;
    taskEXIT_CRITICAL();
    if (!have) return false;
    if (code)   *code = v;
    if (age_ms) *age_ms = age_of(t);
    return true;
}

uint32_t desk_still_ms(void)
{
    taskENTER_CRITICAL();
    uint32_t at = s_dir_us;
    taskEXIT_CRITICAL();
    return (time_us_32() - at) / 1000u;
}

int8_t desk_direction(void)
{
    taskENTER_CRITICAL();
    int8_t d = s_dir; uint32_t at = s_dir_us;
    taskEXIT_CRITICAL();
    // Climbing is ~1 mm per frame, so a few quiet frames is normal mid-move.
    if (d == 0 || (time_us_32() - at) > DESK_DIR_STALE_MS * 1000u)
        return 0;
    return d;
}

uint32_t desk_target_gen(void) { return s_target_gen; }

bool desk_target_mm(uint16_t *mm)
{
    if (!s_have_target) return false;
    if (mm) *mm = s_target_mm;
    return true;
}

void bus_counts(uint32_t *board_ok, uint32_t *panel_ok)
{
    if (board_ok) *board_ok = s_board.ok;
    if (panel_ok) *panel_ok = s_panel.ok;
}
