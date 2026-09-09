#pragma once

#include "settings.h"

// The diagnostic screen's interface — the eight pages a builder works through
// on a unit with no computer attached (design §2.2, §8.2).
//
// The body is src/diag.cpp, a thin binding over the pure state machine and
// layout modules in lib/gbcore/ui/. Five rules define it, and none of them are
// incidental:
//
//   1. One entry. Start+Select held at power-on, sampled once as soon as the
//      button expander is up and before anything reads a cartridge, and
//      nothing else. There is no menu entry and no in-game route.
//   2. It never returns. The mode is a halt: the way out is a power cycle,
//      which is also how a builder gets back to playing. Nothing runs after
//      it in the same boot, so its buffers may be static and generous.
//   3. It reads a tag and never writes one. The inspector shows what is on a
//      cart — its UID, its protection bytes, its raw NDEF and its class — and
//      has no path to changing any of it. A guard test pins that by
//      substring, so the rule holds in comments too.
//   4. It reaches for nothing above or below itself: no writer, no
//      provisioner, no ROM storage and no emulator symbol appears in its
//      sources. It selects no game, and has no list of games to select from.
//   5. When it renders, it renders inside the game window, like every other
//      screen on this device — which is what lets the nudge page move the
//      whole window while a builder watches its border.
//
// `s` is the loaded settings, and the two values the nudge and frameskip
// pages change are written back through it. `nfc_ok` is what nfc_init()
// returned, so the tag page can say the reader did not answer rather than
// blaming the cart; `sd_ok` is what sd_init() returned, so a missing card is
// a page result rather than a halt screen.
void diag_run(settings_t* s, bool nfc_ok, bool sd_ok);
