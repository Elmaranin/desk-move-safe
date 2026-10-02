//
// desk-move-safe — RP2350 firmware that keeps a desk from carrying a folding
// flap past the height it has to be moved at.
//
// It sits in the cut panel-TX wire between the desk's control panel and its
// CL103B-G main board, and forwards both directions. The panel drives the desk
// exactly as it did before. When a move would take the desk past the flap
// height, this takes the bus, stops it there, runs the flap, and lets the move
// finish.
//
//   bus_task      drains both UART taps, parses frames, decides   bus.c
//   wire_task     claims the transmit pin once the panel is heard wire.c
//   flap_task     stop / flap / continue                          flap_task.c
//   console_task  USB-CDC shell                                   console.c
//   encoder_task  MT6835 angle -> continuous shaft position       encoder.c
//
// The flap itself is a NEMA 17 on a TMC2209 in standalone STEP/DIR/EN mode:
// step pulses out of an alarm ISR (stepper.c), current from the carrier's
// trimmer, microstepping from its jumpers. The driver can be put on a serial
// link instead (tmc2209.c, off by default) — see docs/flap-motor.md for what
// that trade looks like.
//
// The decision itself is in flap.c, called from the bus task as each panel
// frame completes — nothing between the panel and the board is faster.
//
// Protocol and wiring: ../docs/CL103B-G_protocol.md, ../docs/wiring.md.
// The rig this was worked out on, with all the instrumentation: ../desk-lab.
//
#include <stdio.h>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board_config.h"
#include "bus.h"
#include "wire.h"
#include "flap.h"
#include "console.h"
#include "settings.h"
#include "stepper.h"
#include "tmc2209.h"
#include "encoder.h"
#include "limits.h"
#include "led.h"

TaskHandle_t g_task_bus, g_task_wire, g_task_flap, g_task_console, g_task_encoder;

// A blown stack or a failed assert used to be a silent hang. Both say what
// happened before stopping.
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    printf("\n\n*** STACK OVERFLOW in task '%s' ***\n", name ? name : "?");
    for (;;) { }
}

void vAssertCalled(const char *file, int line)
{
    taskDISABLE_INTERRUPTS();
    printf("\n\n*** FreeRTOS assert: %s:%d ***\n", file, line);
    for (;;) { }
}

static void wait_for_serial(uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms && !stdio_usb_connected(); t += 10)
        sleep_ms(10);
}

int main(void)
{
    stdio_init_all();

    // EN high and the coils off BEFORE anything that can block, so a board
    // that sat in BOOTSEL with floating pins does not come back energised and
    // then wait three seconds for a USB handshake while it does. With the
    // UART enabled instead of the EN pin this stops being tidiness and becomes
    // the only thing holding the driver off, so the ordering stays either way.
    //
    // Anything printed in here lands in a window no terminal is open for yet.
    // That is what the console's repeated banner is for, and 'tmc' and 'mot'
    // say all of it again on demand.
    stepper_init();
    bool motor = tmc2209_init();
    led_init();

    wait_for_serial(3000);

    // Learned presets and tuning from flash BEFORE the bus runs, so the first
    // press after a reset is already the clean, suppressed one.
    settings_load();
    printf("\n[boot] settings %s\n",
           settings_stored() ? "restored from flash"
                             : "from board_config.h (nothing stored)");
    limits_init();             // the flap range lives in the same record
    printf("[boot] flap motor %s\n",
           motor ? "ready" : "NOT available — 'tmc' for what to check");

    // Working mode, every time, and nothing here has to arrange that: the mode
    // lives in a static bool that reset zeroes, and it is never stored. The
    // self-check is evaluated on demand rather than latched at boot, so there
    // is nothing to run here either — the console's banner prints it a moment
    // from now, 'dev' repeats it, and flap_decide() consults it every frame.
    //
    // Nothing here blocks on the result. No encoder or no stored range locks
    // the DESK — flap_decide() refuses every move — because a flap left open is
    // in its path. See mode.h.

    // Bus highest: it is the only task with a hard deadline. A UART RX FIFO is
    // 32 bytes, which at 9600 baud is 33 ms of slack, and the console below it
    // can busy-wait for half a second inside a printf when the USB host stops
    // reading. It costs almost nothing to give priority to — it sleeps 5 ms
    // out of every 5 ms and moves a few bytes.
    //
    // Console next: it is the only way to say "stop". Then the wire task,
    // which only watches for the panel to appear, and the flap task, whose
    // moves are paced by the panel's own polling.
    xTaskCreate(bus_task,     "bus",     1024, NULL, 4, &g_task_bus);
    xTaskCreate(console_task, "console", 2048, NULL, 3, &g_task_console);
    xTaskCreate(wire_task,    "wire",     512, NULL, 3, &g_task_wire);
    xTaskCreate(flap_task,    "flap",    1536, NULL, 2, &g_task_flap);

    // Encoder alongside the console rather than above it: its SPI transaction
    // is 50 us and bounded, but a shell that cannot be heard is worse than a
    // sample that arrives late. It is below the bus task for the same reason
    // everything is — the bus is the only one with a hard deadline.
    xTaskCreate(encoder_task, "encoder", 1024, NULL, 3, &g_task_encoder);

    // Lowest: it only blinks.
    xTaskCreate(led_task,     "led",      256, NULL, 1, NULL);

    vTaskStartScheduler();

    printf("FATAL: scheduler did not start\n");
    for (;;) { }
}
