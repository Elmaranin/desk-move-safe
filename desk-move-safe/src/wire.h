#ifndef WIRE_H
#define WIRE_H
//
// wire — what this board puts on the bus.
//
// The panel-TX wire is CUT: the panel's frames reach GP5 and nothing else, and
// the board hears only what leaves GP4. Every frame the board receives comes
// from here.
//
// GP4 stays high-impedance until a valid frame has been RECEIVED from the
// panel. Driving a line you cannot hear is the one mistake with a real cost —
// if the wire was not actually cut, the far end is still driving it, and two
// push-pull outputs on one wire is a short through both. Hearing the panel is
// the cheapest available evidence that our end of a broken wire is really
// ours. It is evidence, not proof: a tap wired in PARALLEL with an uncut wire
// hears frames too. What it catches every time is transmitting into a silent
// side — a lead on the wrong pin, a lead off, or no common ground.
//
// FAILS SAFE. A desk told nothing stops moving. If this firmware dies, no key
// frames reach the board and the desk stays where it is.
//
#include <stdbool.h>
#include <stdint.h>

// Returned by flap_decide() to mean "transmit nothing this frame". Not a key
// code — 0xFF is not one.
#define WIRE_NO_FRAME   0xFFu

void wire_task(void *arg);      // claims the pin once the panel is heard

// Send one key frame now. Called from the bus task as each panel frame
// completes, so the board is answered at the panel's own cadence.
void wire_send(uint8_t code);

// While the flap task owns the desk it leaves a command standing here, and
// flap_decide() sends it on the panel's own poll — so a move is clocked by the
// panel's polling rather than a timer of ours, and needs no second transmitter.
//
// A HELD code (up/down) stands until it is changed: that is what a held code
// is, and the desk stops the moment it stops arriving.
void    wire_set_pending(uint8_t code);

// A ONE-SHOT code — a recall or a save — goes out on exactly ONE frame and
// then reverts to idle by itself.
//
// One frame is not a tuning choice, it is what the code MEANS. A real panel
// press of a preset button puts 0x01 on the wire for a single frame and the
// next poll 105 ms later is already idle (protocol doc §4). Streaming it for
// two or three frames is not a tap but a button held down, and the board
// answers a second recall arriving mid-recall by cancelling the first: the
// desk sets off, travels about a centimetre and stops there for good.
//
// It is one frame because the desk counts frames, not milliseconds — so a
// dwell of "about two poll periods" cannot express it. Hence a count.
void     wire_send_once(uint8_t code);
uint32_t wire_burst_left(void);     // 0 once the one-shot has gone out

// What is standing, without consuming a one-shot frame.
uint8_t wire_pending_key(void);

// The code for the frame about to be sent, consuming one one-shot frame.
// Called only from flap_decide(), which is the one place that transmits while
// the flap task owns the desk.
uint8_t wire_next_key(void);

bool     wire_live(void);       // is GP4 actually driven yet?
uint32_t wire_frames(void);

#endif // WIRE_H
