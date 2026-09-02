# NFC Amendments — on-device cart writer

**Status:** proposed changeset against `ROADMAP.md` (and the design doc it references).
**Date:** 2026-09-02
**Scope:** everything downstream of the decision to move cart writing **onto the device** — gated by a
physical MENU cartridge and a one-shot first-boot wizard — and to delete the phone/web writing path
entirely.

This document is a brief for revising `ROADMAP.md`. It does not replace it. Where it says "replace §X",
the agent should edit the design doc; where it says "WS-NN scope changes", edit the workstream entry.

---

## 0. Why this is not a reversal

`ROADMAP.md` §3 rule 1 currently reads:

> **Never add** a ROM browser, a tag-write path, or a "recent games" feature.

**That rule must be rewritten, not deleted.** The intent behind it — *choosing a game is a deliberate
physical act, not a menu interaction* — is unchanged and still governs. What changes is the mechanism that
enforces it.

The old enforcement was "the firmware cannot write tags." The new enforcement is a set of four properties
that together preserve the same intent:

1. **The writer is reachable only by booting with a MENU cartridge inserted, or through the one-shot
   first-boot provisioning wizard (§3a).** The wizard closes itself when setup finishes and is re-armed only
   by clearing NVS from the flashing station over USB — a computer-and-adult gate, the same class of gate the
   original design placed on the phone writing station. There is no button combo, no settings entry, no
   hidden path. The cart is the key.
2. **Selecting a game in the writer does not launch it.** It records a pending write and instructs the user
   to power off. Playing the chosen game requires a power cycle and a physical cart swap.
3. **A running game has no path back to selection.** Unchanged from the current rule, and still the thing
   WS-07's `grep` exit criterion protects.
4. **The device never writes content to a tag that carried a game at boot.** A blank tag in the wizard
   carried no game, so writing it is exempt by definition. Config-only protection writes (§4) change no
   content and are likewise exempt.

Property 2 is the one doing the real work. The writer is a browser in the literal sense, but it cannot be
used as a game launcher, because a selection costs a power cycle and a cart swap — the same friction the
cartridge system was built to impose.

### Replacement wording for §3 rule 1

> **Never add** a path from a running game back to ROM selection, a "recent games" list, or any way to reach
> the cart writer other than booting with a MENU cartridge (or the one-shot first-boot wizard, re-armed only
> by an NVS clear at the flashing station). Selecting a ROM in the writer must never load it in the same
> session. Re-read §6.1 and §13 before planning. WS-06's guard tests enforce the gating mechanically.

### The WS-06 symbol test is replaced, not dropped

The current test greps the symbol table for `ntag2xx_Write` and fails if present. That test must go — the
write path is now required. Replace it with four narrower guard tests:

- **(a) One write site.** `ntag_write_*` / `nfc_write_*` symbols may be referenced only from
  `src/cart_provision.cpp`. Enforce with a link-time or source-grep test.
- **(b) One writer entry point.** Exactly one call site of `writer_open()`, and it is inside the boot state
  machine of §2.
- **(c) The writer never writes and never launches.** `cart_writer*` translation units reference none of
  `nfc_write_*`, `ntag_write_*`, `emu_*`, `rom_store_*`. The writer returns a selection; the boot state
  machine decides what to do with it.
- **(d) Irreversible pages are unreachable.** Host unit test: the page-write whitelist refuses page 2 (static
  lock bytes), page 3 (capability container, OTP), and the dynamic-lock page (0x82 on NTAG215), and the
  config-page composer never sets `CFGLCK`.

---

## 1. Tag taxonomy

Tags are **self-describing**. The device stores **no UIDs anywhere**, and no per-tag state in NVS beyond
the single pending-write record and the wizard flags. This is deliberate:

- Carts remain **tradeable** — a kid's wildcard or menu cart works in any unit in the group.
- A firmware reflash orphans nothing.
- There is no per-tag assignment table that could drift from the physical carts.

A UID binding for the wildcard was considered and rejected. It would have protected against exactly one
mistake — a neighbour's wildcard inserted while a write is pending — at the cost of restricting retargeting
to the originating unit and adding re-adoption state to the wizard. The `WILD:` prefix is enough.

### On-tag format

An NDEF Text record whose payload is one of:

| Payload | Meaning |
|---|---|
| `MENU` | Menu cartridge. Boots the writer. Never a write target. |
| `WILD:tetris.gb` | Wildcard. Loads `tetris.gb`; the default target of a pending write. |
| `tetris.gb` | Ordinary game cart. Loads `tetris.gb`. |
| *(blank)* | Factory-empty NDEF message (`03 00 FE`) **or** all zeros. Both classify as blank. |

Parsing keeps the existing rules from §6.3 — TLV walk, Text record, **language-code length read from the low
6 bits of the status byte**, never assumed to be 2. When the device composes a record it writes language
code `en`.

### Filenames are the permanent key

The filename on a protected tag is the identifier for the life of that cart. **Freeze the ROM filenames
now.** The longest in the current library is 30 characters; the on-tag and NVS caps follow
`ROM_STORE_NAME_MAX` (64) so the tag, the pending record and the ROM store header all truncate identically.
A rename after tags are written is recoverable only by rewriting every affected cart through the menu.

---

## 2. Boot decision flow

Replaces the WS-06 boot flow in `main.cpp`. **Order is load-bearing.** The previous draft evaluated the
pending write before classifying the payload, which would have overwritten a menu cart inserted while a
write was pending.

```
power on
  └─ read tag (one InListPassiveTarget, MaxTg = 2, ~1 s) — before display and SD init
       ├─ no target ......................... "No cartridge"        (halt)
       ├─ two targets ....................... "Shielding fault"     (halt)   ← §5
       ├─ unreadable / not NDEF Text ........ "Unreadable tag"      (halt)
       │
       ├─ classify payload: MENU | WILD | GAME | BLANK
       │
       ├─ setup not finished (NVS setup_done unset)
       │    └─ wizard step (§3a). Accepts BLANK, or an existing MENU / WILD tag that
       │       authenticates with the build password (re-adoption after an NVS clear).
       │
       ├─ pending set? .................... show "Pending: <TITLE> — insert your wildcard" briefly
       │
       ├─ MENU .............................. open writer, pending mode (§3)     ← never a write target
       │
       ├─ pending set and tag matches pending.target
       │      WILDCARD → tag is WILD
       │      NEW_CART → tag is BLANK
       │      REWRITE  → tag authenticates with the build password and is not MENU
       │    └─ write → verify → protect → clear pending → load
       │       (idempotent: power loss before clear-pending simply repeats on the next boot)
       │
       ├─ pending set, tag does not match ... "Insert your wildcard" / "Insert a blank cart"  (halt)
       │
       ├─ BLANK (setup finished, no pending)  "Blank cart. Use your MENU cart."               (halt)
       │
       └─ WILD:<rom> / <rom> ................ resolve <rom>, load
            └─ resolve failure ............. "Not found: <string read>"                     (halt)

after load: if the tag is a valid MENU / WILD / GAME tag and AUTH0 == 0xFF, apply protection (§4).
Config-only write; heals a tag that lost power between write and protect.
```

**Halt means halt.** No retry loop, no fallback browser. Power-cycle is the retry, per §6.2 — the DMG's
mechanical interlock already forces a power-off to change carts.

`MaxTg = 2` is not optional. See §5.

---

## 3. The writer

`src/cart_writer.cpp`, delivered by WS-12. A full-screen UI reachable only from the `MENU` branch of §2
and from the wizard of §3a.

### Layout constraint (important, easy to miss)

**The writer must render inside the same window as the game** — `GAME_X`/`GAME_Y` and the chosen scale's
`GAME_W`×`GAME_H`. The 3D-printed bezel masks everything outside it. A menu drawn to the full 320×240 panel
will be partly hidden behind plastic on every unit.

This applies to the WS-07 in-game menu and WS-09 diagnostics too, and should be stated once as a
cross-cutting rule in `ROADMAP.md` §3 rather than rediscovered three times.

### Behaviour

- Scrolling list of titles from the catalog (§6), D-pad navigated, key repeat from WS-05, Left/Right jump a
  page — the library is 132 entries.
- Highlighted entry shows **96×96 box art** and a **short description**. Art loading is debounced ~150 ms so
  holding Down does not trigger a read per row. Missing art → placeholder, never a failure.
- **A** → confirmation screen naming the exact target and filename → **hold to confirm**. **B** backs out.
- The writer **returns a selection `{rom, target}`** to the boot state machine. It never writes and never
  launches (guard test (c)).
- The writer reuses WS-07's unit-tested list state machine, which is why WS-12 follows WS-07 in the serial
  order.

### Two modes

**Pending mode** — opened from a `MENU` tag. A confirmed selection is recorded to NVS as `pending_t` and the
screen shows *"Power off, insert your wildcard, power on."* The menu also offers:

- **Cancel pending write** (when one is set).
- **New cart** — target `NEW_CART`; the next blank tag becomes an ordinary game cart.
- **Rewrite a game cart** — target `REWRITE`; the next tag that authenticates with the build password
  (except MENU) is overwritten. Heavier confirmation wording: this is the mislabelled-cart recovery path and
  should not be reached by accident.

**Immediate mode** — opened by the wizard with a blank tag already in the slot. The selection is returned
and `cart_provision.cpp` writes it now. The writer itself behaves identically.

**Starter filter:** during setup the list shows catalog entries flagged `starter`; from a MENU tag it shows
the whole catalog.

### Pending write record (NVS)

```c
struct pending_t {
    char    rom[ROM_STORE_NAME_MAX];  // filename as it appears in games.json
    uint8_t target;                   // WILDCARD (default) | NEW_CART | REWRITE
};
```

One record, overwritten by a later selection. No timestamp — millis do not survive the power cycle the
record exists to span.

### Any boot with pending set shows it

Before anything else, display *"Pending: TETRIS — insert your wildcard"* for a second or two. A kid who
forgets what they were doing should not have to guess.

### The writer never coexists with the emulator

Every writer exit is a halt. Its art buffer, catalog index and list state can therefore be static and
generous; heap contention with the ROM store or the audio ring buffer is not a constraint.

---

## 3a. First-boot provisioning wizard

`src/cart_provision.cpp`. WS-06 ships the write handler and the boot-flow integration; the picker UI is
WS-12's. Until WS-12 lands, WS-06 uses a stub picker that writes a fixed starter title.

NVS flags: `menu_done`, `wild_done`, `setup_done`. A fresh flash has none set. Every boot with `setup_done`
unset enters the wizard instead of the normal cart flow. **One write per boot** — the DMG interlock forces
a power cycle per cart anyway, so the wizard just changes what gets written.

1. **"Make your MENU cart."** Blank in the slot → write `MENU`, protect, set `menu_done`. Power off.
2. **"Pick your wildcard's first game."** Picker (starters) → write `WILD:<rom>`, protect, set
   `wild_done`. Power off. The wildcard is never empty.
3. **"Make a game cart."** Picker (starters, already-made ones marked) → write `<rom>`, protect. Power off.
   Repeat.
4. The picker carries **Finish setup** — hold to confirm → set `setup_done`. The wizard is now unreachable.

Each step is a boot with a blank cart, so an unfinished ceremony simply resumes on the next power-on. The
wizard also accepts an existing MENU or WILD tag that authenticates with the build password, so an NVS clear
re-adopts the kid's existing carts rather than demanding new ones.

**Build day:** run the wizard **after** the shell is assembled. It doubles as the per-unit NFC read test
through the shell — the kid proves the reader works before there is anything else to blame.

**Lost menu cart:** borrow any unit's — tags are self-describing. Failing that, an adult clears NVS at the
flashing station and reruns the wizard. Neither a phone nor a `tools/write_menu_tags.py` exists or is
needed.

---

## 4. Protection

**Password protection, never permanent locking.** NTAG21x static and dynamic lock bits are irreversible;
across ~60 tags written by kids, mistakes are certain, and an irreversible mistake is landfill.

After a successful write **and read-back verification**:

- `AUTH0` → first user page (0x04): protect all user memory and the config pages after it
- `ACCESS` `PROT` → **0** (write-protect only; reads stay open, so a phone can still inspect)
- `AUTHLIM` → 0 (no lockout counter — a lockout would be as bad as a lock)
- `PWD` / `PACK` → build-wide constants in `include/hw_config.h`
- **Never set `CFGLCK`.** It permanently freezes the configuration and defeats the whole scheme.

Applied uniformly to **every** tag, MENU and wildcard included. The firmware knows the password, so it can
`PWD_AUTH` and rewrite any tag this build produced. That is the recoverability: a mislabelled cart is a
thirty-second fix via the menu, not a bin.

> **The password is not a secret.** It is a shared constant across all ten units — it has to be, or carts
> would not be tradeable. Its only job is stopping a stray phone from clobbering a cart. Document it as
> such in `hw_config.h` so nobody later mistakes it for a security boundary.

### Protocol facts that bite

- **`PWD` and `PACK` read back as zeros.** Read-back verification must skip those pages. Verify protection
  by issuing `PWD_AUTH` with the build password and comparing the returned `PACK`.
- **`PWD_AUTH` goes through the PN532's `InDataExchange`** (raw transceive), not the page-read helper. The
  minimal I²C driver needs a transceive primitive.
- **Writability check order:** read the config page. `AUTH0 == 0xFF` → unprotected → writable. Otherwise
  `PWD_AUTH` with the build password: success → ours → writable; NAK → foreign → "Insert your wildcard".
  A foreign tag with read protection fails the config read and lands in "Unreadable tag".

### Part and page map

**Standardise on NTAG215.** Config page addresses differ between NTAG213/215/216; hard-code the NTAG215 map
as constants in `hw_config.h`, verified against the NXP datasheet for the exact part ordered — not from a
tutorial. Run `GET_VERSION` on the first tag out of the bag to confirm the chip is what the listing claimed.

| Page(s) | Contents | Policy |
|---|---|---|
| 0x00–0x01 | UID | read only |
| 0x02 | static lock bytes | **never write** |
| 0x03 | capability container (OTP) | **never write** |
| 0x04–0x81 | user memory | write |
| 0x82 | dynamic lock bytes | **never write** |
| 0x83 | CFG0 — `AUTH0` byte 3, `CFGLCK` bit | write; `CFGLCK` never set |
| 0x84 | CFG1 — `ACCESS` | write |
| 0x85 | `PWD` | write; reads as zeros |
| 0x86 | `PACK` | write; reads as zeros |

### Module layout

- `lib/gbcore/cart/ntag.c` — pure C protocol layer over an **injected transceive function**: page
  whitelist, config-page composition, the write → verify → protect sequence. Host-tested against a fake tag
  model, including guard test (d).
- `lib/gbcore/cart/ndef.c` — gains **compose** alongside parse.
- `lib/gbcore/cart/match.c` — **demoted**. Tags are now device-written from the catalog, so exact match is
  the rule; the normaliser and fuzzy fallback survive as legacy for hand-written tags. The normaliser must
  test for a `.gb` **suffix**, not the presence of a dot — add `Snow Bros. Jr..gb`, `Dr. Mario.gb`,
  `Super R.C. Pro-Am.gb` and `Mr. Do!.gb` as tests.
- `src/nfc_cart.cpp` — minimal I²C-only PN532 driver (read pages, InDataExchange, InListPassiveTarget,
  GET_VERSION).

---

## 5. Two-target check

`InListPassiveTarget` is called with **`MaxTg = 2`**. If two targets respond, the device halts with a
shielding fault rather than picking one.

The two-sided disc that motivated this is **dropped** (§7) — unproven, thicker, a labelling trap, and it
forced a menu cart on every kid by construction. Keep the check anyway: it costs nothing and catches a real
assembly-error class (a stray tag in the shell, a disc left in the slot behind a second cart).

---

## 6. SD card layout and assets

```
/roms/gb/Tetris.gb
/art/Tetris.565          raw RGB565, 96×96, little-endian
/saves/Tetris.sav
/catalog.txt             generated; never hand-edited
games.json               (repo only — not on the card)
```

Art filenames are the ROM stem with a different extension. There is no `/desc/` directory; descriptions
live in the catalog.

Library facts: 132 ROMs, ~30 MB; 128 MB cards; art at 96×96 is 18,432 bytes per image, ~2.4 MB total.

### Art

**Do not use PNG.** Decoding on the ESP32 is slow and heap-hungry. The SD image is generated by tooling and
cloned ten times, so pre-convert during imaging. Store **little-endian** RGB565 so the writer pushes art
with the same `setSwapBytes(true)` the frame path uses — design §2.3's byte-order pitfall, avoided once
instead of rediscovered in WS-12:

```bash
ffmpeg -i tetris.png -vf "scale=96:96:force_original_aspect_ratio=decrease,pad=96:96:-1:-1:color=black,format=rgb24" \
       -f rawvideo -pix_fmt rgb565le /art/Tetris.565
```

Sources are RGBA; flatten onto a background before conversion. Loading is then `fread` into a static buffer
and one `pushImage` — no decoder, no allocation.

### Catalog

`/catalog.txt` is **generated only by `tools/image_sd.py` from `games.json`** and never hand-edited. TSV,
one entry per line, plain ASCII, fields in order:

```
filename \t title \t flags \t description
```

`flags` is a comma-separated list (currently only `starter`). Description ≤ 200 bytes. The tool enforces a
line cap and the validator round-trips the catalog against `games.json` in CI.

The firmware reader is pure C, `lib/gbcore/cart/catalog.c`: it keeps a static index of `CATALOG_MAX`
entries (file offset, title, filename) and reads the description line on highlight. **No JSON parser in
firmware.** Why:

- No ArduinoJson: no flash cost, no heap-built document, no hand-rolled parser bugs.
- Static, bounded memory — a fixed index, one line buffer — on a board with no PSRAM.
- Schema decoupling: `games.json` can grow fields freely without touching firmware.
- Failures move to CI: a malformed `games.json` fails the validator on the dev machine, never a unit at boot.

### `games.json`

Remains the **single source of truth**, in the repo. Per entry: `filename` (the frozen key), `title`,
`description`, `art` (source image path), `starter`, plus `developer`, `publisher`, `year`, `genre`,
`players` for the record. Consumed by the imaging tool and validated in CI.

---

## 7. Physical construction

Unchanged from the roadmap unless listed. Recorded here because they interact with read reliability, which
is a WS-06 concern.

- **Anti-metal (on-metal) NTAG215 discs, 25 mm, one per cart.** Not standard tags plus ferrite stickers —
  ferrite raises coil inductance and detunes a tag that was not tuned for it. Anti-metal tags are retuned at
  manufacture. Expect roughly **half the read range** of plain tags; verify the budget with real parts.
- **PN532 mounting:** spiral face toward the cart, ferrite sheet on the component side between module and
  CYD. Mount it behind the cart slot, as far from the CYD as the shell allows.
- **Read timing:** perform the boot read **before** initialising the display and SD, while the RF
  environment is quietest. Retry once after the display is up only so an error can be shown.
- **Tag cup:** printed cup glued into a 1.125" bore in the cart's label face; three tabs at ~0.30 mm
  overhang retain the disc; Ø5 push-out hole in the floor. Well depth 1.4 mm for a single disc.
- **Storage:** printed tray in the DMG battery bay, four discs per layer, three layers. **Gated on the
  battery relocating below the button PCB — measure before designing.**

The two-sided tag/steel/tag puck and its labelling rule are removed.

---

## 8. Workstream impact

| WS | Change |
|---|---|
| **WS-06** `nfc-cart` | **Substantially expanded.** Owns the full tag protocol layer: `lib/gbcore/cart/ntag.c` (injected transceive, page whitelist, write → verify → protect), `ndef.c` compose, `catalog.c`, the minimal I²C PN532 driver in `src/nfc_cart.cpp`, the boot state machine of §2 with `MaxTg = 2`, the `pending_t` NVS record, the wizard write handler in `src/cart_provision.cpp` with a fixed-starter stub picker, the four guard tests of §0, and the dotted-stem matcher tests. The old write-symbol test is replaced per §0. |
| **WS-12** *(new)* `cart-writer` | **New workstream.** `src/cart_writer.cpp`: catalog list, D-pad navigation with page jump, art and description loading, confirmation screen, pending / immediate modes, Cancel pending, New cart, Rewrite a game cart, starter filter, Finish setup. Depends on WS-03 (render), WS-05 (input), WS-06 (tag I/O, catalog), WS-07 (list state machine). Keeping it separate preserves WS-06 as a testable protocol layer with no display dependency. |
| **WS-07** `menu-saves` | Mostly unchanged. The in-game menu keeps **Resume / Volume / Brightness / Palette / Cart Info / Reset** and gains nothing — the writer is *not* reachable from it. Cart Info gains protection state and the `MENU` / `WILD:` / plain classification. Add the render-inside-`GAME_*`-window constraint. |
| **WS-09** `diagnostics` | Gains a **read-only** tag inspector: UID, `GET_VERSION`, protection state, raw NDEF hex, decoded payload. Must not gain a write path — guard tests (a) and (c) apply. Add the window constraint. |
| **WS-10** `build-tools` | **Shrinks and shifts.** Delete `web/` (Web NFC app, clipboard fallback, QR generator) and the Web NFC exit criterion. Keep `games.json` + validator, `image_sd.py`, the ESP Web Tools flashing station, `docs/ASSEMBLY.md`. **Gains:** `tools/seed_games_json.py`, a one-shot seed from `~/ES-DE/gamelists/gb/gamelist.xml` (85 of 132 curated stems match a `<name>` exactly; `tools/esde_aliases.json` maps the 47 shortened names; 74 covers match by stem) with descriptions truncated at a sentence boundary to 200 bytes; art conversion (PNG → `.565`); catalog emission; and a flashing-station **factory reset** (NVS clear) step documented in `ASSEMBLY.md`. |
| **WS-11** `bench` | New verification items — see §9. Remains the last workstream. |

### Dependency graph and order

WS-12 slots after WS-07. Serial order becomes **06 → 07 → 12 → 08 → 09 → 10 → 11**. WS-06 remains
pullable forward (protocol layer, no display dependency); WS-12 needs the render chain, so the "WS-06 is
independent of render/perf" note in `ROADMAP.md` §1 needs qualifying. WS-10 loses its dependency on the
phone side of the NDEF contract but keeps it for the filename and catalog formats.

### Cross-cutting rule for `ROADMAP.md` §3

Add: **All full-screen UI — the writer, the WS-07 menu, WS-09 diagnostics — renders inside
`GAME_X`/`GAME_Y`/`GAME_W`/`GAME_H`.** The bezel hides everything else.

---

## 9. New bench items (WS-11)

Append to §11 of the design doc and to WS-11's step list:

| # | Item | Test |
|---|---|---|
| 10 | Anti-metal read range | Read reliability at final geometry, through the shell, display at full backlight. |
| 11 | Protect-then-rewrite cycle | On one sacrificial tag: write, protect, confirm a phone cannot write, confirm the device can. |
| 12 | Config page addresses | Verified against the NXP datasheet for the exact part ordered; `GET_VERSION` confirms the chip. |
| 13 | Cup retention | Print tabs at 0.20 / 0.30 / 0.40 mm overhang; pick the one that clicks without force. |
| 14 | Bezel window | Confirm the writer and menu render fully inside the visible aperture on an assembled unit. |
| 15 | Wizard end-to-end | On an assembled unit: MENU, wildcard, one game cart, Finish setup. Then two tags in the field → "Shielding fault". |

---

## 10. Open questions and risks

**Friction is now a policy choice, not a mechanism.** The wizard gives every kid a menu cart, so a kid can
reach any of 132 games in roughly twenty seconds: insert menu, select, power off, swap, power on. That is
more friction than a launcher and less than the original design — and 132 games behind a menu cart is a far
more attractive menu than ten. The lever is **collecting the menu carts** after build day, or running the
wizard's MENU step only on an adult's unit. Decide before build day, not after. The firmware is unaffected
either way.

**Losing the menu disc no longer bricks provisioning.** Any unit's menu cart works in any unit, and an NVS
clear at the flashing station reruns the wizard, which re-adopts existing tags (§3a). Do *not* add a
firmware backdoor beyond that.

**Wrong cart with a write pending is now harmless for game carts and menu carts.** The pending target is
matched against the tag class (§2): a WILDCARD write only lands on a `WILD:` tag, NEW_CART only on a blank,
REWRITE never on MENU. The remaining exposure is a neighbour's wildcard inserted by mistake — recoverable in
twenty seconds.

**NVS survives reflash but not a full erase.** After `esptool erase_flash` the wizard reruns and re-adopts
the existing MENU and wildcard tags; a pending write set before the erase is lost silently. One line in
`ASSEMBLY.md`.

**Filename renames.** Once tags are written, renaming a ROM is recoverable only by REWRITE, one cart at a
time. Freeze names now (§1).

---

## 11. What this deletes

- Design doc **§6.6** (phone writing, Web NFC, NFC Tools, clipboard fallback) — replace with on-device
  writing per §2–§3a.
- **§6.1's** claim that "the firmware must be physically incapable of writing tags" — replace with §0.
- **§6.5's** reference to the web app as the writing mechanism.
- **§13's** "don't add a tag-write path" and "don't lock tags" bullets — replace with the §0 wording and
  "never set lock bits or `CFGLCK`; password-protect instead".
- **WS-10's** `web/` scope, the Web NFC exit criterion, and the QR sticker generator.
- **WS-06's** `ntag2xx_Write` symbol test — replace with the four guard tests in §0.
- The permanent-lock plan wherever it appears — replaced by password protection (§4).
- The two-sided disc, its bench item, and its labelling rule.
- `tools/write_menu_tags.py` and any phone-based MENU pre-write — replaced by the wizard (§3a).
- `/desc/` — descriptions live in the catalog (§6).
- Wildcard UID binding (§1).
- The 32-byte `rom` field and `set_ms` in the pending record — replaced by §3's `pending_t`.
