#ifndef DESK_FRAME_H
#define DESK_FRAME_H
//
// desk_frame — byte-stream parser for the CL103B-G's 8-byte UART frames.
//
//   [ sync0 sync1 ] [ len ] [ type ] [ payload x2 ] [ cksum_hi cksum_lo ]
//
// sync encodes direction (55 AA = board->panel, AA 55 = panel->board), len is
// always 0x08, and the checksum is the 16-bit big-endian sum of the first six
// bytes. One parser instance per tapped line, each locked to that line's sync
// pair — a parser fed the other line's bytes just resyncs forever and reports
// it in the counters, which is exactly the symptom of swapped taps.
//
#include <stdbool.h>
#include <stdint.h>

#define DESK_FRAME_LEN      8

#define DESK_TYPE_HEIGHT    0x02    // board->panel: payload = height, u16 BE mm
#define DESK_TYPE_KEY       0x01    // panel->board: payload[1] = key code

// What one fed byte completed. NONE is 0, so `if (feed(...))` still reads as
// "a frame happened" — but BAD is worth distinguishing: a frame that arrived
// whole and failed its checksum is the signature of a marginal tap, and the
// trace wants to show its bytes rather than only count it.
typedef enum {
    DESK_FEED_NONE = 0,     // byte consumed, frame still incomplete
    DESK_FEED_FRAME,        // p->buf holds a valid frame
    DESK_FEED_BAD,          // p->buf holds 8 bytes that failed len/checksum
} desk_feed_t;

typedef struct {
    uint8_t  buf[DESK_FRAME_LEN];   // the completed frame, valid after feed()
                                    // returns true, until the next feed()
    uint8_t  len;                   // bytes collected so far
    uint8_t  sync0, sync1;          // this line's expected sync pair
    uint32_t ok;                    // frames delivered
    uint32_t bad;                   // full frames dropped (len or checksum)
    uint32_t resync;                // bytes skipped hunting for sync
} desk_parser_t;

void desk_parser_init(desk_parser_t *p, uint8_t sync0, uint8_t sync1);

// Feed one received byte. p->buf is valid for both FRAME and BAD, until the
// next feed().
desk_feed_t desk_parser_feed(desk_parser_t *p, uint8_t byte);

// Payload accessors for a frame that feed() just delivered.
static inline uint8_t  desk_frame_type(const desk_parser_t *p) { return p->buf[3]; }
// The height field is NOT a plain u16 of millimetres. The top bits are flags.
//
// Captured while saving a preset, checksum valid:
//
//   55 AA 08 02 42 F5 02 40   -> 0x42F5 = 17141 "mm"
//                                0x42F5 & 0x3FFF = 757 mm, the actual height
//
// Bit 14 is set by the board around a save; the low bits carry the height
// unchanged. Bit 15 has not been seen, but no desk needs a height above
// 16 metres, so both top bits are treated as flags rather than magnitude.
//
// Reading the field raw is not a cosmetic bug: 17141 mm fed to a closed-loop
// move is a desk being told it is sixteen metres too high.
#define DESK_HEIGHT_MASK    0x3FFFu
#define DESK_HEIGHT_FLAGS   0xC000u

static inline uint16_t desk_frame_height_raw(const desk_parser_t *p)
{
    return (uint16_t)((p->buf[4] << 8) | p->buf[5]);
}

static inline uint16_t desk_frame_height_mm(const desk_parser_t *p)
{
    return desk_frame_height_raw(p) & DESK_HEIGHT_MASK;
}

// 0 in normal operation. Bit 0 here is the frame's bit 14.
static inline uint8_t desk_frame_height_flags(const desk_parser_t *p)
{
    return (uint8_t)((desk_frame_height_raw(p) & DESK_HEIGHT_FLAGS) >> 14);
}
static inline uint8_t  desk_frame_key(const desk_parser_t *p) { return p->buf[5]; }

// Human name for a panel key code ("idle", "up", "recall stand", ...).
const char *desk_key_name(uint8_t code);

#endif // DESK_FRAME_H
