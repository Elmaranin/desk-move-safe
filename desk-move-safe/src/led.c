//
// led — see led.h.
//
#include "led.h"
#include "board_config.h"
#include "ws2812.pio.h"

#include "hardware/pio.h"
#include "FreeRTOS.h"
#include "task.h"

#define BLINKS      3
#define ON_MS       150
#define OFF_MS      150

static uint              s_sm;
static bool              s_ok;
static volatile TickType_t s_refused_at;    // 0 = nothing to show

static void put(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ok) return;
    uint32_t grb = LED_ORDER_GRB ? ((uint32_t)g << 16) | ((uint32_t)r << 8) | b
                                 : ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    pio_sm_put_blocking(LED_PIO, s_sm, grb << 8);   // 24 bits, left-aligned
}

void led_init(void)
{
    int sm = pio_claim_unused_sm(LED_PIO, false);
    if (sm < 0 || !pio_can_add_program(LED_PIO, &ws2812_program))
        return;                                     // no LED; nothing else cares
    s_sm = (uint)sm;
    uint offset = pio_add_program(LED_PIO, &ws2812_program);
    ws2812_program_init(LED_PIO, s_sm, offset, PIN_LED);
    s_ok = true;
    put(0, 0, 0);
}

void led_refused(void)
{
    TickType_t t = xTaskGetTickCount();
    s_refused_at = t ? t : 1;
}

void led_task(void *arg)
{
    (void)arg;
    TickType_t shown = 0;
    for (;;) {
        TickType_t at = s_refused_at;
        if (at && at != shown) {
            // A burst per press; another refused press during it starts the
            // next burst straight after, so holding a button keeps it blinking.
            shown = at;
            for (int i = 0; i < BLINKS; i++) {
                put(LED_BRIGHTNESS, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(ON_MS));
                put(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(OFF_MS));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
