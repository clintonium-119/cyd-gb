# CYD-GB — Workstream Roadmap

**Design doc:** [`reference/ORIGINAL_ROADMAP.md`](reference/ORIGINAL_ROADMAP.md) — the settled design.
Section references below (`§n.n`) point into it. This file is the *work breakdown*: how that design becomes a
sequence of apo workstreams, in what order, with what exit criteria, and what each one defers to the bench.

**Status as of 2026-09-01:** hardware is on the bench — `reference/DMG-CYD-wiring.pdf` rev C is
bench-verified (I²C on CN1 with SDA IO22 / SCL IO27, onboard amp confirmed with no hardware mute, SW1
bridge, straight-ribbon button map). Each workstream still reaches *code-complete* as planned; the formal
deferred-verification pass stays collected in WS-11, minus the items rev C already answered (§11 items
2, 3, 4).

---

## 0. How this maps onto apo

- **One workstream = one git branch.** `poc-gb` is the umbrella/integration workstream: it holds project-wide
  decisions, the bench-check results, and the mechanical/build-day tasks that aren't software. Feature
  workstreams branch **off `poc-gb`** and merge back when their code-complete exit is met. `poc-gb → main` at
  the end.
- **Branch naming:** `ws/<slug>` (e.g. `ws/strip-boot`). The apo workstream folder takes the same name.
- **Each workstream is planned with `/apo:plan`** once its dependencies are merged. This file gives the plan
  its scope, exit, and deferred-verification list; the plan supplies the phases and steps.
- **Two exits per workstream.**
  - **Code-complete** — the gate for merging: builds warning-free for `env:cyd`, `pio test -e native` passes,
    every bench-dependent value is a named constant, the deferred checks are written down.
  - **Bench-verified** — the §10 Exit line of the design doc, run on real hardware in WS-11. A workstream is
    not *done* until WS-11 ticks it, but it can *merge* before that.
- **Bench-dependent constants** live in `include/hw_config.h` (pins, expander, addresses, clock) and
  `include/render_config.h` (scale, `GAME_X/Y` defaults). A bench result that changes one of them is a
  one-line commit on `poc-gb`, not a reopened workstream.
- **Decisions** that this document pre-empts (branching model, expander part, saves-on-SD, tooling in-repo)
  should be recorded as `DEC-` notes in `poc-gb/decisions/` when planning starts.

### Proposed Definition of Done (answers `KNOW-0008`'s open questions)

| Gate | Proposal |
|---|---|
| Build | `pio run -e cyd` succeeds with zero warnings from first-party code (library warnings tolerated). Firmware size recorded in the step Outcome when it changes by >5%. |
| Test | `pio test -e native` passes. Any pure-logic change adds or updates a test. |
| Hardware | Deferred to WS-11 until parts arrive. Each workstream's "Deferred verification" list becomes WS-11 steps verbatim. |
| Measurement | Where the design says "re-measure", the step Outcome records *how* to measure (the counter, the serial line) and leaves the number blank until WS-11. |
| Commit | Subject ≤72 chars; body cites the design-doc section (`§2.3`) when implementing one. Direct commits on `ws/*`; merge to `poc-gb` via fast-forward or a local merge commit — no PR ceremony needed for a solo dev, but `git log poc-gb` must stay readable. |
| Regression | Once WS-02 exists: the golden-frame test (render N frames of a test ROM to a buffer, compare hash) must pass before any rendering change merges. |
| Bench-item gate | Not a merge gate. It is the WS-11 gate for calling the project done. |

---

## 1. Dependency graph

```
                    ┌──────────────┐
                    │ WS-01        │
                    │ strip-boot   │
                    └──────┬───────┘
                           │
             ┌─────────────┼─────────────────┐
             ▼             ▼                 ▼
      ┌────────────┐ ┌────────────┐   ┌────────────┐
      │ WS-02      │ │ WS-05      │   │ WS-06      │
      │ host-test  │ │ input      │   │ nfc-cart   │
      └─────┬──────┘ └─────┬──────┘   └─────┬──────┘
            ▼              │                │
      ┌────────────┐       │                │
      │ WS-03      │       │                │
      │ render     │       │                │
      └─────┬──────┘       │                │
            ▼              │                │
      ┌────────────┐       │                │
      │ WS-04      │◀──────┘  (perf owns    │
      │ perf+rom   │            the ROM     │
      └─────┬──────┘            load path   │
            │                   nfc calls)  │
            ├────────────┬──────────────────┘
            ▼            ▼
      ┌────────────┐ ┌────────────┐
      │ WS-08      │ │ WS-07      │
      │ audio      │ │ menu-saves │
      └─────┬──────┘ └─────┬──────┘
            └──────┬───────┘
                   ▼
            ┌────────────┐     ┌────────────┐
            │ WS-09      │     │ WS-10      │  (independent of firmware;
            │ diagnostics│     │ build-tools│   can run any time after WS-06
            └─────┬──────┘     └─────┬──────┘   fixes the NDEF/filename contract)
                  └───────┬──────────┘
                          ▼
                   ┌────────────┐
                   │ WS-11      │  ← needs hardware
                   │ bench      │
                   └────────────┘
```

**Recommended serial order for one developer:** 01 → 02 → 03 → 04 → 05 → 06 → 07 → 08 → 09 → 10 → 11.
WS-05, WS-06 and WS-10 are independent of the render/perf chain and can be pulled forward if the render work
stalls — they touch different files (`button_input.cpp`, a new `nfc_cart.cpp`, `tools/` + `web/`).

Why this order rather than the design doc's Phase numbering: with no hardware, measurement-driven phases
(perf, audio) can't close anyway, so deterministic, host-testable work (input, NFC, menu) is front-loaded, and
perf's ROM-partition change goes before NFC because NFC's boot flow calls into the ROM load path.

---

## 2. Workstreams

Each entry: **Branch · Depends on · Design doc refs · Scope · Code-complete exit · Deferred verification ·
Notes/risks.** The "Deferred verification" bullets are copied verbatim into WS-11 when it is planned.

---

### WS-01 · `ws/strip-boot` — Strip the fork, boot the 2.4" board

**Depends on:** nothing. **Design:** §1.1–1.3, §9, §10 Phase 1.

**Scope**
- Delete `src/bt_scanner.cpp`, `include/bt_scanner.h`, and every call site; drop the BLE library.
- Move `touch_load_settings()` / `touch_save_settings()` (NVS) into a new `src/settings.cpp` with a
  `settings_t` struct (palette, frameskip, brightness, volume, `GAME_X`, `GAME_Y`), then delete
  `src/touch_input.cpp`, `include/touch_input.h`, and the `XPT2046` dep and `TOUCH_*` build flags.
- Stub `ui_launcher`: keep `launcher_ingame_menu()` / `launcher_settings_menu()` compiling (they are rewritten
  in WS-07); delete `launcher_show()` and the ROM list. `loop()` temporarily loads a hard-coded
  `/roms/gb/test.gb` so there is *something* to run until WS-06 lands. **This stub must not become a browser.**
- `platformio.ini`: `-DILI9341_2_DRIVER` → `-DST7789_DRIVER`; confirm `TFT_RST=-1`, `TFT_BL=21`; board comment
  → ESP32-2432S024; keep `SPI_FREQUENCY` at 40 MHz here (80 MHz is WS-04's experiment).
- Rewrite `include/hw_config.h` for the 2.4" board per §1.2: `LED_R_PIN` off IO4; `AMP_EN_PIN 4`;
  `I2C_SDA 27`, `I2C_SCL 1` with a comment pointing at the IO22 contingency *(as merged; rev C later moved
  I²C to CN1 — SDA IO22 / SCL IO27 — and deleted `AMP_EN_PIN`)*; `BAT_ADC_PIN 34`; remove all
  `TOUCH_*` and touch-zone defines; introduce `GAME_X/GAME_Y/GAME_W/GAME_H` as defaults (values from §2.2).
- Pin `include/peanut_gb.h`: identify the upstream commit it matches (diff against Peanut-GB history), add a
  header comment with the SHA and date, and a `scripts/update_peanut_gb.sh` that fetches a given SHA.
- Update `README.md`: strip the touchscreen/2.8" marketing, point to the design doc, remove the `curl` step.
- Record firmware size before and after.

**Code-complete exit**
- `pio run -e cyd` builds warning-free.
- No Bluetooth or touchscreen code links into the firmware:
  `xtensa-esp32-elf-nm .pio/build/cyd/firmware.elf | grep -E ' (T|t|D|d|B|b|R|r) .*(BLEDevice|BLEScan|XPT2046|touch_(init|update|get|save|load|run|set))' | wc -l`
  returns 0. This replaces the original `firmware.map` grep, which counted ESP-IDF's capacitive
  touch-sensor driver (`touch_pad_*`, a different peripheral) and so could never reach 0 — see WS-01's
  DEC on the symbol-absence gate.
- Flash usage recorded in the step Outcome (expect ≥1 MB reclaimed).
- `hw_config.h` has no pin assignment that isn't in §1.2.

**Deferred verification**
- Board boots on the 2.4" panel, black screen, no crash loop over 5 minutes of serial log.
- IO4 is never toggled by anything (rev C: it is not an amp enable and its real function is unknown —
  scope/meter while booting).

**Notes/risks**
- The fork's `button_input.cpp` stays as-is in this workstream (it compiles); WS-05 rewrites it.
- Don't touch partitions here — that is WS-04's job and needs the post-BT size numbers this workstream produces.

---

### WS-02 · `ws/host-test` — Host-side test harness + CI

**Depends on:** WS-01. **Design:** §0 (constraint 3), §10 Phase 0.5.

**Scope**
- `[env:native]` in `platformio.ini` (`platform = native`, `test_framework = unity` or `doctest`), with a
  `lib/gbcore/` or `src/core/` split so pure-logic modules compile with no Arduino headers:
  - `render/scaler.{c,h}` — 2→3 scaler, `avg565`, LUT (WS-03 fills these; WS-02 creates the seams and a
    nearest-neighbour placeholder).
  - `cart/ndef.{c,h}` — NDEF Text-record parser (WS-06 fills).
  - `cart/match.{c,h}` — filename normaliser + matcher over an injected directory listing (WS-06 fills).
  - `input/combo.{c,h}` — debounce + combo state machine over a raw button word (WS-05 fills).
  - `audio/mix.{c,h}` — mono sum, volume scale, dither (WS-08 fills).
- Headless Peanut-GB runner: `gb_init` with an in-memory ROM, run N frames, capture the 160×144 index buffer.
  Needs a **freely redistributable test ROM** (e.g. a homebrew/test ROM with a permissive licence — decide and
  record in a `DEC-`; do not commit commercial ROMs).
- Golden-frame test: hash of frame N of the test ROM, checked in; fails if rendering changes unintentionally.
- GitHub Actions: `pio run -e cyd` + `pio test -e native` on push/PR.
- Document the module boundary in `01_Knowledge/Coding_Standards.md` (which today says there are no tests).

**Code-complete exit**
- `pio test -e native` runs at least the Peanut-GB smoke test and one test per module seam.
- CI green on `poc-gb`.

**Deferred verification** — none; this workstream is host-only.

**Notes/risks**
- Peanut-GB uses `uint_fast*` and some GCC-isms; native build on x86-64 is known to work upstream but check
  `-DPEANUT_GB_IS_LITTLE_ENDIAN` handling.
- Keep the Arduino-side files thin wrappers over the core modules so the tested code is the shipped code.

---

### WS-03 · `ws/render` — Scaler, 12-colour LUT, blend, single-window push

**Depends on:** WS-02. **Design:** §2.1–2.5, §10 Phases 2–4.

**Scope**
- Replace `src/display.cpp`: landscape rotation, `GAME_X/Y/W/H` from `render_config.h` + NVS override, one
  `setAddrWindow` per frame, `pushPixels` per 2-source-line block (3 output rows). No DMA yet.
- Scale factor as a compile-time constant `SCALE_K` (24 or 26 of 16) with both paths compiling; default 24.
- Palette path in `emulator_bridge.cpp`: `pals[N][3][4]`, flat 64-entry `lut[]`, `emu_set_palette()` rebuilds
  it, inner loop `lbuf[x] = lut[px[x]]`. Remove the `SW()` pre-swap; palette stays native RGB565 and
  `setSwapBytes(true)` at push time.
- Design the 20 palettes as 3-ramp sets sharing a hue family (§2.4). Keep names.
- `avg565` blend on both axes in the scaler; unit tests prove `avg565(a,a)==a`, symmetry, and that blending
  never produces a value outside the two inputs' channel ranges.
- Instrumentation: FPS counter already exists (`cfps`); add per-frame `emu_us`, `scale_us`, `push_us` via
  `esp_timer_get_time()`, printed once/second on serial and available to the WS-07 overlay.
- Golden-frame test extended to the *scaled* output (240×216 buffer hash) so the blend is pinned.

**Code-complete exit**
- Golden tests pass for nearest-neighbour and blended output at both `SCALE_K` values.
- No `pushImage` call remains in the frame path.
- Frame-time counters are printed on serial (values blank in Outcome; filled in WS-11).

**Deferred verification**
- Game renders correctly oriented, centred with default `GAME_X/Y`.
- Baseline `emu_us / scale_us / push_us` recorded on the test ROM at 40 MHz SPI (§11 item 8).
- Blend shows no colour fringing (byte-order check, §2.3).
- 12-colour output visible on a sprite-heavy title.

**Notes/risks**
- If §11 item 8 comes back with emulation alone >16 ms, the fallback (gnuboy core) is a new workstream — flag
  early, don't absorb it here.

---

### WS-04 · `ws/perf-rom` — Core split, DMA, ROM partition mmap

**Depends on:** WS-03. **Design:** §3.1–3.3, §10 Phase 5.

**Scope**
- **ROM partition.** New `partitions.csv`: `nvs`, `otadata` (drop OTA — single app slot), `app0` sized to
  WS-01's measured flash use + 25%, `romdata` (raw `data/0x40`) ≥ 2 MB for the rest, keep a small `spiffs` only
  if something still needs it (nothing should). `rom_store.cpp`: `rom_store_write(File&)` (erase + write, with
  a header `{magic, size, crc32, filename}` so an unchanged ROM is not rewritten), `rom_store_mmap()` →
  `const uint8_t*`. `gb_rom_read` becomes `return rom[a]`. Delete the page cache and `cp2spiffs`.
- **Core split.** Emulation task pinned to core 1; display push task on core 0 fed by a two-slot queue of
  scaled row-block buffers (each `3 × GAME_W × 2` bytes). `pushPixelsDMA` with the two buffers alternating.
  Menu/diagnostic drawing pauses the push task and takes the bus directly.
- `SPI_FREQUENCY` as a single build flag; default stays 40 MHz until WS-11 sweeps it (§11 item 7).
- Frameskip retained as a setting but default 0.
- Free-heap accounting: record heap before/after removing the page cache (expected ~96 KB back).

**Code-complete exit**
- Builds with the new partition table; `rom_store` unit-tested on host with a fake flash backend (erase
  granularity, CRC, unchanged-ROM short-circuit).
- The queue/double-buffer logic is unit-tested for ordering (frames never interleave rows).
- No SPIFFS symbol remains.

**Deferred verification**
- 60 fps, frameskip 0, on a 1 MB MBC5 title with heavy bank switching — no stutter.
- `SPI_FREQUENCY` sweep 40/55/80 MHz, artifacts noted (§11 item 7).
- 26/16 vs 24/16 flipped and compared; decision recorded as a `DEC-` (§2.1).
- Write-to-partition time on cart insert measured (expect a few seconds; needs a "Loading…" screen — WS-06).

**Notes/risks**
- Flash writes stall the *other* core's instruction fetch from flash unless code is in IRAM; do the partition
  write before the emulation task starts, never during play.
- Read `ROM_PARTITION_FIX_COMPLETE.md` in `GodSpoon/cyd-gb-v2` first (§3.2).

---

### WS-05 · `ws/input` — MCP23017 buttons, combos, NVS

**Depends on:** WS-01 (WS-02 for tests). **Design:** §1.3, §1.4, §5, §10 Phase 7.

**Scope**
- Rewrite `src/button_input.cpp` for MCP23017 at `BTN_I2C_ADDR 0x20`: `IODIRA=0xFF`, `GPPU=0xFF`, read `GPIOA`
  once per frame. Keep the `button_init/update/get_buttons` interface.
- I²C bring-up in a shared `i2c_bus.cpp` (PN532 in WS-06 uses the same bus): bus recovery (9 SCL pulses +
  STOP) gated on `I2C_SCL == 1`, `Wire.begin(I2C_SDA, I2C_SCL)`, 400 kHz. *(As merged; rev C moved the bus
  to CN1 and the recovery routine was deleted with it.)*
- `input/combo.c` (host-testable): 8 ms debounce, edge detection, Start+Select → menu, Select+Up/Down →
  volume, Select+Left/Right → brightness, D-pad masked out of the joypad word while Select is held,
  optional key repeat (400 ms / 200 ms). Volume/brightness go through `settings.cpp` → NVS with a
  write-coalescing delay so a held button doesn't hammer NVS.
- Remove the fork's `input_task` 12 ms polling in favour of a per-frame poll from the emulation loop.

**Code-complete exit**
- Combo state machine unit tests: single-step on hold, masking, no wrap on volume clamp, debounce rejects
  <8 ms glitches.
- Expander driver isolated behind a 3-function interface; `hw_config.h` selects pins/address.

**Deferred verification**
- All eight buttons register (feeds WS-09's button test screen) on the rev C straight-ribbon map
  (Up=GPA7 … B=GPA0).
- First I²C transaction on a cold boot succeeds on CN1 (SDA IO22 / SCL IO27) with no recovery step.

**Notes/risks**
- `Wire` and the display share no pins, but the PN532 read at boot and the button poll share the bus — the
  read happens before emulation starts, so no arbitration is needed. Document that assumption.

---

### WS-06 · `ws/nfc-cart` — PN532 boot read, NDEF, filename matching

**Depends on:** WS-04 (ROM load path), WS-05 (I²C bus). **Design:** §6.1–6.5, §10 Phase 8.

**Scope**
- `src/nfc_cart.cpp`: PN532 over I²C at 0x24, `SAMConfig`, one `InListPassiveTarget` with a ~1 s timeout at
  boot, read NTAG pages 4..N via `ntag2xx_ReadPage`, hand the raw bytes to the parser. **Read-only API by
  construction** — no write function exists in the module; add a unit test that greps the symbol table for
  `WritePage`/`ntag2xx_Write` and fails if present.
- `cart/ndef.c`: TLV walk → NDEF Text record → language-length from status byte → filename. Tests for `en`,
  `fr`, 5-byte language codes, missing terminator, non-Text record, empty tag.
- `cart/match.c`: normalise (lowercase, trim, append `.gb`), exact match, then substring fallback over an
  injected listing. Tests including `tetris` → `Tetris (World).gb` and ambiguous matches (pick shortest name,
  log it).
- Boot flow in `main.cpp`: splash → read tag → match → if ROM differs from `rom_store` header, "Loading…" and
  write → `emu_init` → `load_ram` → run. Failure screens show the *string read*: `No cartridge`,
  `Unreadable tag`, `Not found: <name>`. No retry loop, no browser; power-cycle is the retry (§6.2).
- Remove the WS-01 hard-coded ROM stub. Keep a **build-flag-only** `DEV_ROM_PATH` for bench work, off by
  default, and fail the CI build if it is defined in the default env.
- `games.json` schema defined here (filename, title, size, optional palette hash) — WS-10 consumes it.

**Code-complete exit**
- Parser and matcher tests pass; the read-only symbol test passes.
- `main.cpp` has no code path from a running game back to ROM selection.

**Deferred verification**
- Tap → correct ROM on ≥3 different carts, one written from an iOS locale device.
- Read succeeds with the DMG's RF shield removed; fails with it in place (confirm the §6.2 claim).
- Time from power-on to game start recorded, with and without a partition rewrite.

**Notes/risks**
- Adafruit PN532 library pulls in SPI/HSU paths; consider a minimal I²C-only driver (~200 lines) to keep flash
  down. Decide in `/apo:plan`.

---

### WS-07 · `ws/menu-saves` — D-pad menu, automatic saves, Cart Info

**Depends on:** WS-05, WS-06. **Design:** §7, §8.1, §10 Phase 9.

**Scope**
- Rewrite `ui_launcher.cpp` → `menu.cpp`: D-pad navigated list, items **Resume / Volume / Brightness /
  Palette / Cart Info / Reset**. Remove Quit, Calibrate, Save, Load, the touch `mbtn()` code and the settings
  submenu's frameskip/overlay entries move to WS-09's diagnostic screen (frameskip stays reachable there).
- Auto-save: `gb_cram_w` sets `dirty` + `last_write_ms` (only inside the real save size); flush when dirty and
  idle ≥10 s, on menu open, on Reset, and on the low-battery hook (`battery.cpp` reads IO34 with a
  `BAT_DIVIDER` constant and a threshold that WS-11 calibrates — §11 item 6). Brief "SAVED" toast.
- Auto-load on boot (already exists as `load_ram`; keep).
- Cart Info: UID hex, raw filename read, matched path, ROM title from header, palette hash.
- `Reset` → flush → `gb_reset()`.

**Code-complete exit**
- Dirty-flag logic unit-tested (writes outside save size don't dirty; idle timer; menu flush).
- Menu navigation logic unit-tested as a state machine (no display involved).
- `grep -n "quit\|launcher_show" src/` returns nothing.

**Deferred verification**
- Play, wait 10 s, power off, power on → progress retained.
- Low-battery flush triggers at the calibrated threshold.
- Menu usable with the physical D-pad with no touch fallback.

---

### WS-08 · `ws/audio` — MiniGB APU, DAC output, volume

**Depends on:** WS-04 (needs the reclaimed heap and the core split). **Design:** §1.6, §4, §10 Phase 6.

**Scope**
- Vendor `minigb_apu.{c,h}` (pin SHA, same header convention as Peanut-GB). `ENABLE_SOUND 1`; implement
  `audio_read`/`audio_write`.
- `audio/mix.c` (host-testable): stereo→mono sum, 16-bit volume scale via `vol_lut`, ±1 LSB dither, truncate
  to 8-bit. Tests: silence stays silent, full-scale doesn't wrap, dither bounded.
- Output: timer-driven DAC on IO26 at 32768 Hz from a ring buffer filled once per frame by the APU. There
  is no amp-enable pin — rev C proved IO4 is not one and no hardware mute exists.
- Volume states high/med/low/off; off holds the DAC at 128 (mid-scale), not 0 and not zero samples. The
  designed IO4 auto-mute is gone with the mute pin; the amp is always live.
- Volume wired to WS-05's Select+Up/Down and WS-07's menu.

**Code-complete exit**
- Mixer tests pass; firmware builds with `ENABLE_SOUND 1`; ring buffer under/overflow counters exposed on
  serial.

**Deferred verification**
- §11 item 3 is already answered (rev C): onboard amp verified with real GB music from SD, on battery —
  recorded as a `DEC-` on `poc-gb`; no escalation path needed.
- Audio plays at 60 fps with no dropouts; frame-time impact recorded.
- Idle-hiss level with the amp always live and the DAC parked at 128 judged acceptable; volume steps sound
  evenly spaced.

**Notes/risks**
- The MAX98357A fallback is dead: rev C verified the onboard amp, and IO22 now carries I²C SDA anyway.

---

### WS-09 · `ws/diagnostics` — Boot-combo diagnostic screen

**Depends on:** WS-05, WS-06, WS-07, WS-08. **Design:** §2.2, §8.2, §10 Phase 10.

**Scope**
- Hold Start+Select at power-on (sampled after WS-05's bus init, before the NFC read) → `diag.cpp`.
- Pages, D-pad to switch: buttons (live, GPA bit labelled), SD (present, ROM count, free), NFC (PN532 firmware
  version, live UID), battery (raw ADC + computed V using `BAT_DIVIDER`), audio (test tone, cycles volume
  states), display (colour bars, 1-px border at `GAME_X/Y/W/H`, checkerboard for the blend), **nudge**
  (`GAME_X/Y` ± with A to save to NVS, B to reset default), frameskip and FPS overlay toggles, firmware
  version/build timestamp (from `scripts/post_build_timestamp.py`).
- **No tag write path.** The read-only symbol test from WS-06 covers this module too.

**Code-complete exit**
- Every page renders on host in a framebuffer test (so layout is at least sane); nudge persistence
  unit-tested through `settings.cpp`.

**Deferred verification**
- A second person can follow a one-page checklist and confirm each subsystem on a fresh unit without a
  computer.
- Nudge-saved `GAME_X/Y` survive a power cycle and are honoured in-game.

---

### WS-10 · `ws/build-tools` — games.json, SD imaging, phone web app, flasher

**Depends on:** WS-06 (NDEF/filename contract). Firmware-independent otherwise. **Design:** §6.5, §6.6,
§10 Phase 11 (software parts).

**Scope**
- `games.json` (schema from WS-06) as the single source of truth; a validator in CI (unique filenames, files
  exist in the private ROM directory given by env var — ROMs themselves are never committed).
- `tools/image_sd.py`: format-agnostic copy of `/roms/gb/*` + empty `/saves/` from a local library dir to a
  mounted card, verify by hash, print a manifest; idempotent so ten cards come out identical.
- `web/` on GitHub Pages: lists `games.json`; Android Chrome → Web NFC read-before-write, write Text record
  with the filename, read back and show result; no `NDEFReader` → clipboard fallback (synchronous
  `writeText` inside the tap handler) with NFC Tools instructions. QR code generator for the wildcard sticker
  linking to the page. Never offers a lock action.
- Flashing station: ESP Web Tools page under `web/flash/` with a `manifest.json` pointing at CI-built
  `firmware.bin` + `bootloader` + `partitions` artefacts from a tagged release.
- `docs/ASSEMBLY.md`: build-day checklist skeleton (polarity meter check, RF shield removal, switch-on-to-
  charge, flash-before-shell), to be completed from WS-11 findings.

**Code-complete exit**
- Web app works against a real NTAG215 on an Android phone (this *is* testable now — tags and a phone don't
  depend on the board order).
- `image_sd.py` produces two byte-identical manifests from two runs.
- Release workflow publishes flashable artefacts.

**Deferred verification**
- A card imaged by the tool boots a unit and every `games.json` entry is found by a cart.
- ESP Web Tools flashes a bare board over USB-C.

---

### WS-11 · `ws/bench` — Bench checks and deferred verification *(hardware-gated)*

**Depends on:** parts arriving; ideally all of WS-01..10 merged. **Design:** §10 Phase 0, §11, every Exit line.

**Scope**
- §11's still-open items in order — item 1's brownout sweep, items 5–8, and the new item 9 (SW1 bridge
  check) — each a step whose Outcome records the measurement and the resulting constant change (if any) as
  a commit on `poc-gb` plus a `DEC-`. Items 2, 3 and 4 were answered by wiring PDF rev C (2026-09-01) and
  already have `DEC-` notes.
- Then every workstream's "Deferred verification" list above, as steps grouped by workstream.
- 24/16 vs 26/16 decision (§2.1). `SPI_FREQUENCY` decision (§3.3). (The audio escalation decision (§1.6)
  was settled by rev C: onboard amp, no mod.)
- `BAT_DIVIDER` and low-battery threshold calibration (§11 item 6).
- Complete `docs/ASSEMBLY.md` from what actually went wrong.
- Mechanical items (bezel print/fit with rear flange, USB-C slot, battery harness, RF shield removal) are
  tracked as **tasks** on `poc-gb` (`/apo:task-create`), not steps here — they're not software and don't
  gate a merge.

**Exit**
- All eight §11 items answered; all deferred checks ticked or converted into bugs (`/apo:bug-create`) on the
  owning workstream; one unit plays a cart end-to-end from a cold boot with audio.

---

## 3. Cross-cutting rules for every workstream

1. **Never add** a ROM browser, a tag-write path, or a "recent games" feature. Re-read §6.1 and §13 before
   planning. The WS-06 symbol test enforces the tag-write rule mechanically.
2. **Bench-dependent values are constants**, named in one of two headers, defaulted to the design doc's value,
   with a comment citing the § and the §11 item that verifies it.
3. **Host tests for anything pure.** If a function has no `Arduino.h` dependency, it goes in the core modules
   and gets a test.
4. **Measurements are recorded, not remembered.** Frame times, heap, flash size go into the step Outcome, even
   when the value is "deferred to WS-11".
5. **Design-doc edits go to `reference/ORIGINAL_ROADMAP.md`** when a workstream learns something that changes
   the design; this file only changes when the *work breakdown* changes.

---

## 4. Next actions

1. Record the four pre-empted decisions as `DEC-` notes on `poc-gb` (branching model; MCP23017; saves on unit
   SD; tooling in-repo).
2. `git checkout -b ws/strip-boot poc-gb` → `/apo:plan` WS-01 using §2 above as the brief.
3. On merge, proceed down the serial order in §1.
