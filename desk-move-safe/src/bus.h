#ifndef BUS_H
#define BUS_H
//
// bus — the two taps on the desk's UART, and the state they publish.
//
// Drains both lines, parses frames, and keeps the latest decoded values for
// the rest of the firmware. Nothing here prints: this task has a hard
// deadline (a 32-byte UART FIFO is 33 ms at 9600 baud) and printf to USB can
// block for half a second when the host stops reading.
//
#include <stdbool.h>
#include <stdint.h>

#define BUS_LINE_BOARD  0
#define BUS_LINE_PANEL  1

void bus_task(void *arg);

// Latest height from the main board. False if none has arrived. The flag bits
// are already masked off and an implausible value never gets this far.
bool desk_height_mm(uint16_t *mm, uint32_t *age_ms);

// Latest key code from the panel. False if none has arrived.
bool desk_last_key(uint8_t *code, uint32_t *age_ms);

// Which way the desk is travelling: +1 up, -1 down, 0 stopped.
//
// From the HEIGHT stream, not the key line. A preset recall puts one frame on
// the wire and then the board drives itself, so anything watching the keys is
// blind for the whole of an autonomous move.
int8_t desk_direction(void);

// How long the height has been unchanged, in ms.
uint32_t desk_still_ms(void);

// The height the board announced before an autonomous move — a preset's
// destination, about a second before the desk starts. It arrives as a height
// frame that jumps too far to be real, which is also how it is recognised.
bool desk_target_mm(uint16_t *mm);

// The announcement is KEPT after the move — it is the last one, not the
// current one. Each announcement bumps this number, so whoever waits for "the
// destination of the recall just sent" records it at the recall and accepts
// only an announcement with a different number. Reading desk_target_mm()
// without that check is how the old sit destination got learned as stand.
uint32_t desk_target_gen(void);

// Good frames seen on each line, for the status report.
void bus_counts(uint32_t *board_ok, uint32_t *panel_ok);

#endif // BUS_H
