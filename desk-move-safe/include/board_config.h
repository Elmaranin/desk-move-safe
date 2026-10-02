#pragma once
//
// desk-move-safe — pin map and tuning.
//
// The firmware sits in the cut panel-TX wire between a Völundr X2DM-SE desk's
// control panel and its CL103B-G main board. It forwards both directions, and
// when a move would carry the desk past a chosen height it takes the bus,
// stops there, runs the flap, and lets the move finish.
//
// Protocol and wiring: ../docs/CL103B-G_protocol.md, ../docs/wiring.md.
//
// NAMING. A DESK_ prefix means a property of the desk or its bus — true
// whether or not this board exists. Things this firmware does carry their own
// name: PROXY_, FLAP_, SETTINGS_.
//
//   Board: RP2350A Supermini (Pico 2 compatible die; different break-out).
//

// ---- one UART per channel -------------------------------------------------
// The bus is two one-way channels and each gets a whole UART, receiving one
// end of its channel and transmitting the other:
//
//   HEIGHT   board --> panel     UART0   in GP1
//   KEYS     panel --> board     UART1   in GP5, out GP4
//
// GP0/GP1 are UART0 and nothing else; GP4/GP5 are UART1 and nothing else. And
// within a pair the direction is fixed too: GP0 and GP4 are TX (outputs),
// GP1 and GP5 are RX (inputs). No setting reverses either.
//
// ⚠️ The PANEL header's own RX/TX silkscreen is SWAPPED — the pin marked RX is
// the one the panel transmits on. See ../docs/wiring.md before cutting.
#define UART_HEIGHT         uart0
#define PIN_FROM_BOARD      1       // <- board TX: 55 AA height frames (tap)
#define UART_KEYS           uart1
#define PIN_FROM_PANEL      5       // <- panel TX: AA 55 key frames
#define PIN_TO_BOARD        4       // -> board RX: what the board is told

#define DESK_BAUD           9600
#define DESK_POLL_MS        104     // the panel's poll period

// ---- proxying -------------------------------------------------------------
// On at boot, and it must stay that way: the panel-TX wire is CUT, so a board
// that is not proxying is a desk that cannot move at all. Booting into
// pass-through means a reset costs half a second, not a dead desk.
//
// Proxying is transport, not control. The panel drives the desk exactly as it
// did before the cut.
#define PROXY_ON_BOOT       1

// ---- the height field -----------------------------------------------------
// NOT a plain u16: the top two bits are flags (bit 14 is raised around a
// preset save). 0x42F5 read raw is 17141 "mm"; masked it is 757, the real
// height. Feeding the raw value to a move is a desk told it is sixteen metres
// too high. See ../docs/CL103B-G_protocol.md §2.
#define DESK_HEIGHT_MASK        0x3FFFu
#define DESK_HEIGHT_SANE_MIN_MM 200     // rejected outright outside this
#define DESK_HEIGHT_SANE_MAX_MM 2500
#define DESK_HEIGHT_STALE_MS    2000    // older than this: act on nothing

// A jump larger than this did not happen mechanically — the desk manages
// 2-3 mm per frame descending and 1 ascending. It is the board announcing
// where a preset is HEADED, about a second before it sets off, and it must not
// be mistaken for a height. Kept, because it is the destination.
#define DESK_MAX_STEP_MM        10
#define DESK_ANNOUNCE_MAX_FRAMES 20     // ... unless it keeps saying so
#define DESK_DIR_STALE_MS       600     // no change for this long = stopped

// ---- travel ---------------------------------------------------------------
// The desk's own limits, P01 and P02 in its settings menu. Defaults there are
// 73 cm and 122 cm. A target outside this is refused rather than sent.
#define DESK_MIN_MM             730
#define DESK_MAX_MM             1220

// How far the desk travels after being told to stop, per direction — it is not
// the same both ways. Measured on this desk: up 15-19 mm, down 15-19 mm.
// They do not need to be exact; see DESK_NEAR_MM.
#define DESK_COAST_UP_MM        18
#define DESK_COAST_DOWN_MM      19

// Close enough. The flap does not care about the last few millimetres, and the
// only fine adjustment this desk offers is a 10 mm step — so chasing anything
// tighter costs a stop, a pause and a beep to gain nothing. Stopping SHORT is
// the safe side: the flap runs before the desk has crossed.
#define DESK_NEAR_MM            10

#define DESK_MOVE_TIMEOUT_MS    40000
#define DESK_STALL_MS           1500    // height stopped changing while driving

// ---- the ceiling ----------------------------------------------------------
// While this firmware sits in the panel-TX path it simply will not pass an UP
// command that would take the desk past this — whatever the panel asks,
// including a button held down. Enforced a coast early so the coast lands AT
// the limit rather than past it.
#define DESK_CEILING_MM         800
#define DESK_CEILING_ON         1

// ---- what the flap actually does ------------------------------------------
// run_flap() in flap_task.c is a two-second dwell today. While that is true the
// flap touches no motor, so the encoder and the stored travel limits are not
// preconditions for it — and refusing to stop the desk because they are missing
// would take away a feature that works, in exchange for nothing.
//
// Set this to 1 when run_flap() drives the stepper for real (step 4). From that
// moment the self-check also gates the flap itself. The desk does not wait for
// this: no encoder or no stored min/max already locks the desk — every move is
// refused — because a flap left open is in its path. See src/mode.h.
#define FLAP_DRIVES_MOTOR   0

// ---- the flap -------------------------------------------------------------
// The height the desk is stopped at so the flap can move. A preset recall that
// would cross it is caught BEFORE the board is told about it: the destination
// is already known, so nothing starts moving and there is nothing to abort.
#define FLAP_HEIGHT_MM          770
#define FLAP_ON                 1

// Once stopped for the flap, do not fire again until the desk has left this
// band — otherwise a desk resting on the mark re-triggers on every press.
#define FLAP_REARM_MM           25

// Held still this long counts as stopped. Two poll periods plus margin: the
// upward crawl is ~1 mm per frame, so anything shorter reads the gap between
// frames as arrival.
#define FLAP_SETTLE_MS          700
#define FLAP_FRESH_MS           350     // a height older than this is not evidence

// How long the flap takes. A dwell until the stepper is wired — flap.c is the
// one place that changes.
#define FLAP_MOVE_MS            2000

// Stopping a move the BOARD is driving needs a key press injected; idle does
// not touch one (protocol doc §4). One edge frame is enough — but it is also
// worth DESK_NUDGE_MM of travel, so it goes AWAY from the flap height.
#define DESK_NUDGE_MM           10      // one edge frame, measured
#define DESK_NUDGE_LATENCY_MS   1000    // before it acts
#define FLAP_STOP_PHASE_MS      5000    // > latency + the ~3 s abort coast
#define FLAP_STOP_TRIES         3
#define FLAP_BRISK_MS           250     // still changing this recently = not braking

// A one-shot code (a recall, a save) is sent in FRAMES, not milliseconds, and
// the count is always ONE — that is what the code means to the board, not a
// value to tune. See wire.h. This is only how long we wait for that one frame
// to find a panel poll to ride out on before giving up on it.
#define WIRE_ONESHOT_TIMEOUT_MS 1000

// After a stopped move resumes, stay out of the way until it is over: the desk
// has to cross the flap height to reach its destination, which is the point.
#define PLAN_RESUME_QUIET_MS    2000
#define PLAN_RESUME_MIN_MS      4000    // never sooner: the board takes ~1 s to start
#define PLAN_RESUME_MAX_MS      45000

// ---- preset heights -------------------------------------------------------
// Knowing where a preset goes is what lets its recall be suppressed before the
// board hears it — nothing starts, nothing is aborted, no beep. Learned from
// the board's own announcements and kept in flash; 0 = not known yet.
#define PRESET_STAND_MM         0
#define PRESET_SIT_MM           0

// ---- the flap motor -------------------------------------------------------
// A NEMA 17 through a reduction gearbox, on a TMC2209 carrier, in the driver's
// standalone STEP / DIR / EN mode — the same way the bench rig at
// ../../../test/nema_MT6835 runs it, so the port is a copy.
//
//   Carrier pin   Signal                      RP2350
//   -----------   -------------------------   ------------------
//   VDD           logic supply 3.3 V          3V3
//   GND           logic ground                the rig's ground node
//   DIR           direction level             GP6   <- PIN_DIR
//   STEP          one pulse = one microstep   GP7   <- PIN_STEP
//   EN            enable, ACTIVE LOW          GP8   <- PIN_EN
//   MS1, MS2      microstep select            strapped; see TMC_MICROSTEPS
//   VREF          current limit               the trimmer on the carrier
//   PDN_UART      —                           leave unconnected
//   VM / GND      motor supply 12-24 V        NOT from this board
//   1A 1B 2A 2B   motor coils                 NEMA 17
//
// ONE UNBROKEN RUN. GP7 is the last pad of the right edge and GP8 the first of
// the bottom, so GP6-GP11 are six pads in a row around the corner: DIR, STEP,
// EN, then the UART pair if it is ever wanted, then DIAG. The driver harness is
// one connector and the whole flap lives on one side of the board.
//
// WHY NOT THE UART. It was built and it works (src/tmc2209.h, src/tmc_uart.pio,
// and TMC_UART_ENABLED below turns it back on) but it does not pay for itself
// here. It replaces the DIR and EN pins with two of its own, so the wire count
// goes from three to two — and buys that one wire with a 1k resistor, a solder
// jumper on the carrier that ships unbridged, a motor supply that must be live
// before the chip will answer at all, and an address selected by the same
// MS1/MS2 pins that used to pick microstepping. One wire is not worth four new
// ways to be silently wrong.
//
// What it WOULD buy, if a later revision wants it: current and microstepping as
// numbers instead of a trimmer and jumpers, over-temperature and open-coil
// reporting, and StallGuard. Nothing in stepper.c cares either way — the ramp
// is the interval between STEP edges and that is all it has ever been.
//
// PIN CHOICE IS CONSTRAINED, on this module more than most. GP0, GP1, GP4 and
// GP5 are the desk bus. GP26-GP29 are SPI1 and are reserved for the MT6835 —
// the far end of the board, four pads in a row, the sensor cable as far from
// the driver as it can get. And on the RP2350A Supermini, GP16 drives the
// onboard WS2812 while GP17-GP25 are back-side inner pads rather than
// castellations — so the usable set is GP0-GP15 and GP26-GP29, and the bench
// rig's SPI0 group at GP16-GP19 simply does not exist here.
// See ../../docs/wiring.md.
//
// Non-negotiables, all of them learned the hard way on the rig:
//   * The motor supply ground and this board's ground must be tied together.
//   * >=100 uF electrolytic across VM/GND, physically at the driver.
//   * Never unplug the motor while VM is live — the inductive kick kills the
//     driver.
//   * SET VREF BEFORE THE FIRST RUN. In this mode it is the current limit, not
//     a fallback: on a 0.11 ohm carrier I_rms is roughly 0.71 * VREF. Check
//     the sense resistor while you are there — 0.10, 0.11 and 0.15 all ship on
//     boards sold under the same description.
#define PIN_DIR             6       // sampled by the driver on the STEP edge
#define PIN_STEP            7       // rising edge = one microstep
#define PIN_EN              8       // active LOW: 0 = coils energised, 1 = free

// The driver's stall output, or a limit switch. Unwired for now. Pulled UP and
// read active-low, like every input here — RP2350 erratum E9 makes internal
// pull-downs unusable.
#define PIN_TMC_DIAG        11

// ---- microstepping --------------------------------------------------------
// Strapped on the carrier, so this is a promise about the hardware rather than
// a setting. The TMC2209 table is NOT the A4988 one, and there is no full-step
// mode:
//
//   MS2  MS1   microsteps
//    0    0      1/8      <- both low or floating: the power-on default
//    0    1      1/2
//    1    0      1/4
//    1    1      1/16
//
// Get it wrong and nothing fails — every "revolutions" figure the firmware
// prints is simply off by that ratio, quietly, including the travel limits.
#define TMC_MICROSTEPS      8

// ---- the UART, off ---------------------------------------------------------
// 1 puts the driver on a serial link: current and microstepping become numbers,
// faults become readable, and DIR and EN can move into registers. It needs the
// wiring in ../docs/flap-motor.md, including the carrier's UART jumper.
//
// Direction and enable have nowhere to live but pins while this is 0, which the
// #error below enforces rather than leaving you with a motor that will not move.
#define TMC_UART_ENABLED    0
#define TMC_USE_DIR_PIN     1
#define TMC_USE_EN_PIN      1

// Only meaningful with TMC_UART_ENABLED. TX reaches PDN_UART through a 1k
// resistor and RX sits on the same node; MS1/MS2 become the address bits.
#define TMC_UART_ADDRESS    0
// GP9 and GP10 are adjacent, so the 1k that makes half duplex work bridges two
// neighbouring castellations and a single wire leaves for PDN_UART.
#define PIN_TMC_TX          9
#define PIN_TMC_RX          10
#define TMC_UART_PIO        pio0
#define TMC_UART_BAUD       115200  // the chip works it out from the sync byte

#if !TMC_UART_ENABLED && (!TMC_USE_DIR_PIN || !TMC_USE_EN_PIN)
#error "With TMC_UART_ENABLED 0 there are no registers to put direction or \
enable in, so TMC_USE_DIR_PIN and TMC_USE_EN_PIN must both be 1. Either wire \
the pins, or turn the UART back on."
#endif

// ---- MT6835 magnetic encoder — STEP 2, NOT YET USED -----------------------
// Reserved here so the pin map does not have to move again. SPI1 on GP26-GP29:
// four pads in a row at the TOP of the left edge, which is the furthest this
// module gets from the driver corner at GP6-GP11. The sensor cable and the
// motor cable leave from opposite ends of the board.
//
// The RP2350's SPI functions repeat every eight pins as RX, CSn, SCK, TX, so
// GP26-GP29 is a complete SPI1 group. Note the order that gives: SCK and MOSI
// come first and MISO third, which is only cosmetic.
//
// CS IS AN ORDINARY GPIO, driven by the firmware rather than by the SPI block,
// so it could be anywhere. GP29 is chosen because it is also SPI1's own CSn —
// and because GP29 is ADC3, which on many RP2350 modules carries a VSYS sense
// divider. CS is an OUTPUT, so a weak divider cannot affect it; a MISO there
// could be pulled, which is why MISO is on GP28.
#define MT6835_SPI_PORT     spi1
#define MT6835_PIN_SCK      26      // SPI1 SCK
#define MT6835_PIN_MOSI     27      // SPI1 TX
#define MT6835_PIN_MISO     28      // SPI1 RX
#define MT6835_PIN_CS       29      // SPI1 CSn, driven by hand
#define MT6835_SPI_BAUD     (1000 * 1000)

// Smallest position error worth believing, in degrees of the measured shaft.
// One count of a 21-bit sensor is 0.00017 deg, far below what a magnet glued to
// a shaft can actually deliver: noise on a real mounting is a few hundredths of
// a degree, and a lost-step check must not trip on that. Floor for
// encoder_tolerance_deg().
#define ENCODER_NOISE_FLOOR_DEG 0.05

// How far a move may end from its target before it counts as lost steps,
// in degrees of the MOTOR shaft. One full step at 1/8 microstepping is 1.8 deg,
// so this is a bit over half a full step — tight enough to catch a real slip,
// loose enough not to trip on sensor noise or a magnet slightly off axis.
#define POSITION_TOLERANCE_DEG  1.0

// ---- travel limits ---------------------------------------------------------
// Ported from the bench rig with its values. Once min and max are stored
// ('lim min' / 'lim max'), the encoder watches every move and hard-stops the
// motor this far past either end — degrees of the measured shaft. See limits.h.
#define LIMIT_GUARD_DEG     1.0

// How far the step counter may disagree with the encoder before a move
// re-seeds it from the absolute angle.
#define LIMIT_RESYNC_DEG    1.0

// After a targeted move ('mot go') the shaft is crept toward the target under
// the encoder until it is within this many degrees.
#define LIMIT_SETTLE_DEG    0.1

// The last stretch of a targeted move is crept under the encoder rather than
// run open-loop, so the load cannot carry the shaft past an end. The default
// for a new range; 'eeprom set lim_approach_deg <deg>' stores another.
#define LIMIT_APPROACH_DEG  4.0

// ---- motor and gearing ----------------------------------------------------
#define FULL_STEPS_REV      200     // standard 1.8 deg NEMA 17

// Motor revolutions per output revolution. MEASURED, not read off the label:
// the rig's box is sold as 50:1 and measures 48.435:1, and this one measures
// 17.23:1, because 42 mm planetary boxes are built from 3.7:1 and 5.18:1
// stages whose products are never round. The encoder step will re-measure it.
#define GEAR_RATIO          17.23

// ---- current — UART ONLY ---------------------------------------------------
// None of this reaches the driver while TMC_UART_ENABLED is 0. In standalone
// mode the current limit is the VREF trimmer and nothing in software can see
// or change it; these values are here for when the link goes in.
//
// The sense resistor on the carrier, in ohms. READ IT OFF THE BOARD — this
// number is the whole scale factor for every current below, so a wrong one
// silently runs the motor at the wrong torque and nothing complains.
//
// 0.11 is the figure usually quoted for BigTreeTech / FYSETC carriers, but
// 0.10 and 0.15 are both out there on boards sold under the same description,
// and people who have measured theirs report 0.10 where the datasheet-derived
// tables say 0.11. That is a 10% error in every current setting. If the motor
// runs hotter or weaker than the numbers suggest, this is the first thing to
// doubt.
#define TMC_RSENSE_OHMS     0.11
#define TMC_MAX_CURRENT_MA  1500    // refused above this, whatever is asked

// Run current is what the motor pulls while stepping; hold is what is left
// after TMC_TPOWERDOWN_TICKS of standstill. A flap that must not drift wants a
// real hold current; one that can be pushed by hand wants none.
#define TMC_RUN_CURRENT_MA  800
#define TMC_HOLD_CURRENT_MA 300
#define TMC_IHOLDDELAY      6       // how gradually it drops to hold current
#define TMC_TPOWERDOWN_TICKS 20     // ~0.4 s of standstill before it does

// ---- chopper — UART ONLY ---------------------------------------------------
// The datasheet's own defaults. TOFF is also the driver's enable: zero stops
// it driving at all, which is how tmc2209_set_chopper() works when the link is
// on. With it off, the EN pin is the only enable there is.
#define TMC_TOFF            3
#define TMC_TBL             2
#define TMC_HSTRT           5
#define TMC_HEND            0

// ---- motion profile -------------------------------------------------------
// Both in MICROSTEPS, so they scale with TMC_MICROSTEPS — which means they only
// mean what they say if MS1/MS2 are strapped to match it.
//
// Measured on the rig: unloaded, this motor is stable to 15000 microsteps/s at
// 1/8. Through the 17.23:1 box, 5000 microsteps/s is 65 deg of output per
// second — a third of that ceiling, with the margin left for the load and for
// a hard stop not to carry the flap past its end.
#define DEFAULT_SPEED_SPS   5000u
// Reaching that speed takes v^2/(2a) = ~420 microsteps, a quarter of a motor
// revolution, so any move longer than half a turn cruises.
#define DEFAULT_ACCEL_SPS2  30000u

// ---- status LED ------------------------------------------------------------
// The onboard WS2812 — the Supermini's only LED (../docs/wiring.md). Blinks red
// when the desk lock refuses a panel press. On its own PIO block: the driver's
// UART has TMC_UART_PIO.
#define PIN_LED             16
#define LED_PIO             pio1
#define LED_ORDER_GRB       1       // most WS2812s; 0 if "red" comes out green
#define LED_BRIGHTNESS      40      // of 255 — these are bright at arm's length

// ---- settings in flash ----------------------------------------------------
// The last flash sector. PICO_BOARD=pico2 assumes 4 MB; if the boot banner
// does not say "restored from flash", run `picotool info -a` and set this.
//   #define NVS_FLASH_SIZE_BYTES   (2 * 1024 * 1024)
//
// A change is written once the settings have been untouched for a moment,
// never mid-move: the write blanks interrupts for tens of milliseconds.
#define SETTINGS_QUIET_MS       2000
#ifndef SETTINGS_ENABLE_FLASH
#define SETTINGS_ENABLE_FLASH   1
#endif
