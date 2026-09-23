#pragma once
#include <stdint.h>

// Every pin below appears in the ESP32-2432S024 pin map (design §1.2 / §1.4).
// Each line carries that table's status and the §11 bench item that settles it.
// Render geometry lives in render_config.h, not here.

// ─── Display (onboard) ──────────────────────────────────────────────────────
// Backlight PWM (§1.2). Was 21, which is CN1 pad 3 on this board's silkscreen
// and now carries I2C SCL; 27 is the 2.4" board's backlight per the same
// reading of the board against the vendor pin table (silkscreen, 2026-09-14).
// One bench check confirms it: the panel lights and dims on the first flash
// with this value, or it does not. Must equal -DTFT_BL in platformio.ini.
#define TFT_PIN_BL     27   // Panel pins are set in platformio.ini.

// Landscape: the panel is 320 px horizontal x 240 px vertical once rotated
// (§2.1). TFT_WIDTH / TFT_HEIGHT in platformio.ini still describe the
// controller's native portrait orientation — rotation, not those, decides
// which way round the image sits.
#define SCREEN_W      320
#define SCREEN_H      240
#define TFT_ROTATION_LANDSCAPE 1   // §11 confirms 1 vs 3 once a board sits in a shell;
                                   // 3 is the same landscape flipped end for end, so this
                                   // flips if the USB socket lands on the wrong side.

// The panel's own orientation, which the column-major frame path addresses it
// in: one write then advances along a gate line, which is what writing along
// the scan means. It must stay the partner of the landscape rotation above —
// 0 goes with 1, 2 goes with 3 — because the two differ by 180 degrees and
// the frame path maps the landscape viewport origin through the pair.
//
// MEASURED, not inferred, by PANEL_FILL_PROBE on 2026-09-21: pushing a known
// column order through the real window at rotation 0 put the first column at
// the LEFT and the first pixels of a column at the BOTTOM. So at rotation 0
// the slow axis runs with landscape x and the fast axis runs AGAINST
// landscape y — which would render every column upside down, because the
// order of the pixels inside a column is the one thing the frame path cannot
// cheaply reverse.
//
// Rotation 2 mirrors both, so its fast axis runs with landscape y and its
// slow axis against landscape x. That is the usable pairing, and the
// backwards slow axis is free: FRAME_COLS_DESCENDING below has the scaler lay
// each block's columns down in reverse, which costs a sign on a stride.
//
// It follows the landscape rotation rather than standing alone, because the
// two landscape values are 180 degrees apart and so are the two portrait
// ones: flipping TFT_ROTATION_LANDSCAPE to 3 for a shell that puts the USB
// socket on the other side has to flip this with it, or the game image turns
// over while the UI does not.
#ifndef TFT_ROTATION_PORTRAIT
#if TFT_ROTATION_LANDSCAPE == 1
#define TFT_ROTATION_PORTRAIT 2
#else
#define TFT_ROTATION_PORTRAIT 0
#endif
#endif
#if TFT_ROTATION_PORTRAIT != 0 && TFT_ROTATION_PORTRAIT != 2
#error "TFT_ROTATION_PORTRAIT must be 0 or 2 — a landscape value is not a scan-order mapping."
#endif

// The panel's scan-out order, reversed. MADCTL's ML bit, which changes the
// order the gate lines are refreshed without touching how frame memory maps
// onto them — so the image does not move and only the direction the refresh
// sweeps does.
//
// On by default, and half of what makes the column-major push worth having.
// Measured on the bench of 2026-09-21: with the write running against the
// sweep a seam is permanently on screen, and with it running along the sweep
// the write can stay ahead for whole frames, so the seam becomes an occasional
// fast crossing instead. The panel honoured it in two independent ways — the
// column artefact went intermittent, and the row-major one's diagonal
// reversed its lean.
//
// Set to 0 to compare against the panel's own order.
#ifndef PANEL_SCAN_REVERSE
#define PANEL_SCAN_REVERSE 1
#endif

// ─── Panel rate trim ────────────────────────────────────────────────────────
// PORCTRL's front porch, in whole lines plus 64ths of a line, which is what a
// unit carries in NVS once it has been calibrated. One line is about 0.17 Hz —
// coarser than the oscillator error being cancelled — so the porch alternates
// between neighbouring values and the 64ths are the duty of that alternation.
//
// These are the values an UNCALIBRATED unit runs at, not a constant the code
// relies on: every unit's own pair overrides them from the store. They are the
// batch-typical null rather than the panel's power-on porch, because the blind
// trial of 2026-09-21 scored a foreign constant at 9-12 s between seam
// crossings against under 5 s for power-on — so a shared value makes an
// unvisited unit mediocre where power-on makes it bad. Evidence is one board,
// which is why the choice is cheap to move and lives here.
//
// The pair is the committed anchor, 11 + 20/64: the stripe fixture's null of
// 2026-09-20 and the rate every build since has been measured against.
#ifndef PANEL_TRIM_FPA
#define PANEL_TRIM_FPA 11
#endif
#ifndef PANEL_TRIM_RATIO
#define PANEL_TRIM_RATIO 20
#endif
#if PANEL_TRIM_FPA < 1 || PANEL_TRIM_FPA > 126
#error "PANEL_TRIM_FPA is a PORCTRL front porch in lines: 1..126."
#endif
#if PANEL_TRIM_RATIO > 63
#error "PANEL_TRIM_RATIO is 64ths of a line: 0..63."
#endif

// Which end of the landscape x axis the portrait address window starts at.
// Rotation 2 advances against landscape x once a gate line is filled, so the
// frame path hands its output columns over right to left; rotation 0 runs the
// other way in both axes. Derived here rather than at the two call sites so
// the window origin and the walk direction cannot disagree.
#if TFT_ROTATION_PORTRAIT == 2
#define FRAME_COLS_DESCENDING 1
#else
#define FRAME_COLS_DESCENDING 0
#endif

// ─── SD card (onboard, VSPI, now exclusive to SD) ───────────────────────────
#define SD_PIN_CS       5   // §1.2, confirmed
#define SD_PIN_MOSI    23   // §1.2, confirmed
#define SD_PIN_MISO    19   // §1.2, confirmed
#define SD_PIN_SCK     18   // §1.2, confirmed

// ─── I²C (buttons, NFC) ─────────────────────────────────────────────────────
// The whole bus lives on the CN1 plug: GND / IO22 / IO21 / 3.3V, as printed on
// the board itself (silkscreen read 2026-09-14). The wiring PDF rev C and the
// vendor pin table say IO27 for pad 3 — that is the 2.8" board's CN1, and the
// pin table is the document that has been wrong before, so the silkscreen wins
// until a meter says otherwise (§1.2, §1.3). Neither pin is shared with the
// UART, so no bus recovery is needed.
#define I2C_SDA        22   // CN1 pad 2 (silkscreen; verified rev C)
#define I2C_SCL        21   // CN1 pad 3 (silkscreen, 2026-09-14; bench check pending)
#define BTN_I2C_ADDR 0x20   // MCP23017, A0-A2 to GND (§1.4)

// Button expander bit map. The button PCB's bottom eight header pins run in
// order to GPA7..GPA0 — a straight ribbon, no crossover (§1.4, wiring PDF
// rev C) — which puts Up on the high bit and B on bit 0. Each switch is
// active LOW against the expander's internal pull-ups. §11's "all eight
// buttons register" item is what verifies the order on a built unit.
#define BTN_GPA_UP      7   // §1.4, rev C
#define BTN_GPA_DOWN    6   // §1.4, rev C
#define BTN_GPA_LEFT    5   // §1.4, rev C
#define BTN_GPA_RIGHT   4   // §1.4, rev C
#define BTN_GPA_START   3   // §1.4, rev C
#define BTN_GPA_SELECT  2   // §1.4, rev C
#define BTN_GPA_A       1   // §1.4, rev C
#define BTN_GPA_B       0   // §1.4, rev C

#define PN532_I2C_ADDR 0x24 // PN532 breakout, DIP switches to I²C (§1.4)
// One InListPassiveTarget must come back inside this window with no tag
// present; the PN532's MxRtyPassiveActivation is set to match (§6.2 "~1 s",
// bench item in §11 may tune it). The per-command wait for the ready bit is
// separate and short: every command but the tag search answers at once.
#define NFC_DETECT_TIMEOUT_MS 1000
#define NFC_READY_TIMEOUT_MS   100

// ─── NFC tag password ───────────────────────────────────────────────────────
// THIS PASSWORD IS NOT A SECRET, and nothing about the cartridge scheme
// depends on it staying one.
//
// All ten units share it, deliberately: a cart written on one device has to
// work on every other, so the kids can trade cartridges. That means the
// password cannot be per-unit, cannot be derived from a serial number, and
// has no business being hidden — it lives here in the repository in plain
// sight.
//
// Its only job is to stop a stray phone tapped against a cart from
// overwriting it by accident. Anyone who reads this file can write our tags,
// and that is fine: the failure it prevents is a clobbered cartridge, not an
// attacker. It is not a security boundary, and no other part of the system
// may come to treat it as one.
//
// Reads stay open (PROT = 0) so a phone can still inspect a cart, and the
// tag is never permanently locked — see the cartridge protection decision.
#define NTAG_PWD  { 0x47, 0x42, 0x43, 0x54 }  // "GBCT"
#define NTAG_PACK { 0x47, 0x42 }              // "GB"

// ─── Backlight adjustment ───────────────────────────────────────────────────
// The Select+Left/Right combo steps the backlight by BL_STEP and clamps to
// [BL_MIN, 255]. The two constants are chosen together: the ladder is
// BL_MIN + n*BL_STEP, so 10 + 7*35 = 255 lands the top rung exactly on the
// clamp and gives 8 evenly spaced levels with no stubby final step. Change
// one and the other has to move, or the level count drifts.
//
//   10  45  80  115  150  185  220  255
//
// The floor is not 0 on purpose: a unit mounted in a shell at brightness 0
// looks dead, and the operators cannot recover it by sight. The bench asked
// for a dimmer bottom than the original 32 (the header used to anticipate
// raising it; hardware wanted the opposite), and 10 is ~4% duty — dim enough
// for a dark room, still visibly lit.
#define BL_STEP        35
#define BL_MIN         10

// ─── Audio ──────────────────────────────────────────────────────────────────
// No amp-enable pin exists. The vendor datasheet calls IO4 the amp enable, but
// the bench proved it is not (§1.6, wiring PDF rev C); its real function is
// unknown, so leave IO4 unused (§13). With no hardware mute, volume "off"
// holds the DAC at 128 (mid-scale) instead.
#define SPEAKER_DAC_PIN 26  // DAC channel 2 = the I2S built-in DAC's left
                            // channel (§1.6, confirmed rev C). The I2S API
                            // selects the channel, not the pin, so this is
                            // documentation plus an init-time check.
// Must equal AUDIO_SAMPLE_RATE in platformio.ini — the same two-places rule
// as SD_PIN_CS / -DSD_CS. The bridge static-asserts the pair.
#define SPEAKER_SAMPLE_RATE 32768
// One Game Boy frame at that rate (§4). Static-asserted in the bridge against
// the APU's own AUDIO_SAMPLES, which derives it from the vertical-sync rate.
#define SPEAKER_SAMPLES_PER_FRAME 548
// The same figure undivided, in ten-thousandths: 32768 / (4194304 / 70224) =
// 548.6197 samples a frame. The constant above is that truncated, which is all
// a core emitting a fixed count per frame can use — but a caller that paces
// ITSELF off the speaker's DMA queue is choosing its own frame rate when it
// chooses a sample count, and 548 is 32768/548 = 59.7956 Hz, a full 0.068 Hz
// above the cadence gnuboy's cycle-derived count produces.
//
// That matters wherever a rate is being nulled against "the rate a game runs
// at": a page paced at the truncated figure would calibrate the panel against
// a cadence no game has, leaving a residual beat that caps the result at about
// fifteen seconds between tear crossings however well it converges. A caller
// that spends the fraction as well matches a game by construction, because
// both are then the same physical clock divided by the same number — crystal
// error included.
#define SPEAKER_SAMPLES_X10000 5486197
// The most one write may hand over, which is deliberately more than nominal.
//
// MiniGB APU emits exactly SPEAKER_SAMPLES_PER_FRAME whatever the frame did,
// so for that core the two are the same number and this changes nothing.
// gnuboy's count is cycle-derived and follows the frame's real emulated
// length: 548 or 549 at this rate, averaging 548.62. Truncating to nominal
// discarded the surplus — about 37 samples a second, each one a discontinuity
// in the middle of the waveform — which is audible and is not what the
// underflow counter measures.
//
// With the surplus delivered instead of dropped, the DAC's own back-pressure
// paces emulation to 32768 / 548.62 = 59.727 fps, which is a Game Boy's true
// frame rate. That needs only one frame's overshoot of headroom; the rest is
// fast-forward's, whose time-stretch lengthens a frame by up to its splice
// seam, 200 samples, and shortens others to match.
#define SPEAKER_SAMPLES_MAX (SPEAKER_SAMPLES_PER_FRAME + 256)
// DMA queue depth, in frames. Latency is depth x 16.7 ms, about 67 ms here;
// whether that is audible against on-screen events is §11 bench work, and
// lowering it is a one-line change.
#define SPEAKER_DMA_FRAMES 4
// The bound on the pacing wait: one frame plus margin. A write that cannot
// place its frame inside this window gives up rather than stalling the
// emulator (§4).
#define SPEAKER_WRITE_TIMEOUT_MS 20

// ─── Power ──────────────────────────────────────────────────────────────────
#define BAT_ADC_PIN    34   // Battery sense (§1.2). Divider ratio undocumented — §11 item 6.

// The two bench-dependent numbers. Both are placeholders: the CYD's IO34
// divider is undocumented (§1.2), so §11 item 6 meters the cell against the
// [BAT] boot line and corrects them together in one commit.
#define BAT_DIVIDER   2.0f  // (verify) Assumed 2:1 — §11 item 6 measures the real ratio.
#define BAT_LOW_MV    3500  // (verify) Flush threshold — §11 item 6 sets it from the cell's cutoff.
// Re-arm margin above BAT_LOW_MV, so a cell that sags under load and
// recovers between samples does not save every second.
#define BAT_HYST_MV    100
// One ADC read per second is enough: the low-battery latch carries its own
// hysteresis, so there is nothing for a faster sample to catch.
#define BAT_SAMPLE_MS 1000

// ─── Status LEDs ────────────────────────────────────────────────────────────
// §1.2 gives no LED pin on this board. The fork drove IO4, which must stay
// unused (§13); all three are -1 and every use site guards on >= 0.
#define LED_R_PIN      -1
#define LED_G_PIN      -1
#define LED_B_PIN      -1

// ─── Game Boy ───────────────────────────────────────────────────────────────
#define GB_SCREEN_W   160
#define GB_SCREEN_H   144
