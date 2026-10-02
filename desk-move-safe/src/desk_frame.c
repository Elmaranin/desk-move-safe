//
// desk_frame — see desk_frame.h for the frame layout.
//
#include "desk_frame.h"

void desk_parser_init(desk_parser_t *p, uint8_t sync0, uint8_t sync1)
{
    p->len    = 0;
    p->sync0  = sync0;
    p->sync1  = sync1;
    p->ok     = 0;
    p->bad    = 0;
    p->resync = 0;
}

desk_feed_t desk_parser_feed(desk_parser_t *p, uint8_t byte)
{
    if (p->len == 0) {
        if (byte != p->sync0) {
            p->resync++;
            return DESK_FEED_NONE;
        }
        p->buf[p->len++] = byte;
        return DESK_FEED_NONE;
    }
    if (p->len == 1) {
        if (byte != p->sync1) {
            // Not our sync pair. The byte we just read could itself open a
            // new frame, so keep it as a candidate sync0 instead of dropping
            // both — otherwise a 55 55 AA sequence never locks.
            p->resync++;
            p->len = (byte == p->sync0) ? 1 : 0;
            return DESK_FEED_NONE;
        }
        p->buf[p->len++] = byte;
        return DESK_FEED_NONE;
    }

    p->buf[p->len++] = byte;
    if (p->len < DESK_FRAME_LEN)
        return DESK_FEED_NONE;
    p->len = 0;

    // len field first, checksum second: sum of the first six bytes, 16-bit
    // big-endian in bytes 6..7 (protocol doc §2).
    uint16_t sum = 0;
    for (int i = 0; i < 6; i++)
        sum += p->buf[i];

    if (p->buf[2] != DESK_FRAME_LEN ||
        (sum >> 8)   != p->buf[6]   ||
        (sum & 0xFF) != p->buf[7]) {
        p->bad++;
        return DESK_FEED_BAD;
    }
    p->ok++;
    return DESK_FEED_FRAME;
}

// The motion codes are two consecutive pairs — edge then held, down then up:
//
//   0x05 down edge   0x06 down held
//   0x07 up edge     0x08 up held
//
// Both edge codes were confirmed by hand on this desk: a single Down tap gives
// 0x05, a single Up tap gives 0x07. That closes out the protocol doc's old
// "0x07 up transient, exact meaning not pinned down" — it is just the up half
// of the pair.
const char *desk_key_name(uint8_t code)
{
    switch (code) {
        case 0x00: return "idle";
        case 0x01: return "recall stand";
        case 0x02: return "recall sit";
        case 0x03: return "save stand";
        case 0x04: return "save sit";
        case 0x05: return "down edge";
        case 0x06: return "down (held)";
        case 0x07: return "up edge";
        case 0x08: return "up (held)";
        case 0x0E: return "panel wake";
        default:   return "unknown";
    }
}
