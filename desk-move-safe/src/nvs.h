#ifndef NVS_H
#define NVS_H
//
// nvs — one small record in the last sector of flash: the EEPROM this board
// doesn't have.
//
// The RP2350 has no non-volatile memory of its own apart from the program
// flash, so anything that must survive a power cycle goes there. This module
// keeps exactly one record — a header with a length and a CRC, then the
// caller's bytes — in the sector at the very end of flash, where the firmware
// image will never reach. A record is only ever read back if its CRC checks,
// so a fresh board, a torn write, or a record written by an older layout all
// read as "nothing stored" rather than as garbage.
//
// Writing erases the sector first (100 000 cycles rated), takes tens of
// milliseconds, and runs with interrupts off. On this rig that means the UART
// FIFOs (32 bytes, 33 ms at 9600) may overflow and a frame or two is lost to
// a resync — harmless, but do it from the console task, never from the
// sniffer, and never while a move is in progress.
//
#include <stdbool.h>
#include <stddef.h>

// True — and buf filled — if a record of exactly len bytes with a good CRC is
// stored. A length mismatch counts as "nothing stored", which is what a struct
// that grew a field should see.
bool nvs_read(void *buf, size_t len);

// Erase and rewrite. False if len is too big for the sector.
bool nvs_write(const void *buf, size_t len);

// Erase the sector; nvs_read() returns false from then on.
void nvs_erase(void);

#endif // NVS_H
