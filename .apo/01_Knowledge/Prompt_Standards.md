---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0007"
category: standards
title: "Prompt Standards"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-09-03'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Coding_Standards]]", "[[01_Knowledge/System_Overview]]"]
tags: [apovault, knowledge, standards]
---

# Prompt Standards

Agent-facing rails. Each is either cited to a source in this repository, or marked as confirmed by the user
during `/apo:init`.

## Project rails from `reference/ORIGINAL_ROADMAP.md`

`reference/ORIGINAL_ROADMAP.md:14-16` addresses agents directly: "Any agent planning work on this project should internalise
them before proposing changes." These are therefore rails, not background.

- **Do not add a ROM browser, a game-switching UI, a "recent games" list, or an on-device NFC tag writer.**
  The cartridge system is the product. `reference/ORIGINAL_ROADMAP.md:404-415` goes further than a preference: "The firmware
  must be physically incapable of writing tags." Hiding such a feature behind a boot combo is explicitly
  rejected as insufficient. **Source:** `reference/ORIGINAL_ROADMAP.md:16-20, 404-415, 668` (read 2026-08-27).
- **Do not switch the emulator to Retro-Go.** It is a launcher; adapting it means suppressing its central
  feature. Borrow its techniques — core split, DMA, single framebuffer push — not its architecture.
  **Source:** `reference/ORIGINAL_ROADMAP.md:338-345, 669-670` (read 2026-08-27).
- **Do favour solder-free connections and per-unit adjustability in NVS.** Ten units are assembled once by
  kids. Anything needing a rework station or per-unit firmware variation is the wrong answer.
  **Source:** `reference/ORIGINAL_ROADMAP.md:21-23` (read 2026-08-27).
- **Do not treat `reference/ORIGINAL_ROADMAP.md`'s flagged numbers as settled.** The document says so directly at `:26-29`, and
  §11 (`:679-696`) lists nine bench items with the test that resolves each — five still open as of wiring
  PDF rev C (2026-09-01), which answered items 2, 3 and 4. Frame-time figures in §3.1
  are marked "estimated, not measured" and "could be off by 50% either way."
  **Source:** `reference/ORIGINAL_ROADMAP.md:26-29, 293-300, 679-696` (read 2026-09-01).
- **Do read `reference/ORIGINAL_ROADMAP.md` §13 before proposing anything that looks like an obvious improvement.** It is a
  list of things already considered and rejected, each with the reason: no ROM browser, no Retro-Go, no
  driving IO4 (the bench proved it is not the amp enable and its real function is unknown), no building on
  the vendor datasheet without metering (it has been wrong twice: the header pinout and IO4), no stretching
  to 240 rows, no blending byte-swapped pixels, no per-pixel cross-palette branch, no manual Save/Load, no
  kid-accessible tag locking, no per-line `pushImage`. (The old no-I²C-on-IO3 entry is moot — I²C lives
  entirely on CN1 as of wiring PDF rev C.)
  **Source:** `reference/ORIGINAL_ROADMAP.md:710-726` (read 2026-09-01).

## `README.md` vs `reference/ORIGINAL_ROADMAP.md` — which wins

`README.md` documents the **inherited fork** (2.8" ESP32-2432S028R, ILI9341, touchscreen, ROM browser).
`reference/ORIGINAL_ROADMAP.md` documents the **target** (2.4" ESP32-2432S024, ST7789, physical buttons, NFC cartridges) and
calls itself "the settled design." **Source:** `README.md`; `reference/ORIGINAL_ROADMAP.md:11` (read 2026-08-27).

- **Do treat `reference/ORIGINAL_ROADMAP.md` as authoritative for intent**, and the current `src/` tree plus `README.md` as the
  starting point being replaced.
- **Do not "fix" code to match `README.md`.** Several README statements are already stale for this fork — for
  example step 3 says `peanut_gb.h` "is **not included** in this repo", but it is committed and tracked.
  **Source:** `README.md` step 3 vs `git log -1 -- include/peanut_gb.h` (read 2026-08-27).
- **Do expect current code to contradict the roadmap**, and say so rather than silently picking a side. Known
  divergences are catalogued in [[01_Knowledge/Integration_Map]] and [[01_Knowledge/Code_Map]] — the button
  expander part (PCF8574 in code vs MCP23017 in the roadmap), the display driver define, `LED_R_PIN` on IO4,
  and the `loop()`-driven ROM browser.

## Formatting

- **Do write new code in the expanded style:** one statement per line, braces on multi-line control flow,
  spaces around operators, descriptive parameter names. Match `src/main.cpp:19-43` and
  `src/sd_manager.cpp:70-82` (both from commit `3dd6145`, 2026-08-04). **Confirmed during /apo:init on
  2026-08-27.**
- **Do not add new compressed one-liner code** in the inherited fork style (`emulator_bridge.cpp` 44
  multi-statement lines, `ui_launcher.cpp` 50, `touch_input.cpp` 35). It is legacy, and `reference/ORIGINAL_ROADMAP.md` §9
  marks most of those files for deletion or replacement anyway. **Confirmed during /apo:init on 2026-08-27.**
- Reformatting untouched legacy code is not required and creates review noise — expand as you rewrite, not
  as a separate sweep.

## Naming and structure

- **Do name new public functions `<module>_<verb>` in `snake_case`**, declared in `include/<module>.h`. This
  is universal across all 50 first-party declared functions. Match the module's existing prefix, not its
  filename — `ui_launcher.h` exposes `launcher_*`, `emulator_bridge.h` exposes `emu_*`.
  **Source:** `include/*.h` prefix counts (read 2026-08-27); see [[01_Knowledge/Coding_Standards]].
- **Do keep module state file-scope `static`** and expose only functions. The one existing global is
  `extern TFT_eSPI tft` (`include/display.h:3`).
- **Do use `#pragma once`** in new headers — 8 of 9 headers do; the exception is vendored.
- **Do not introduce classes, namespaces, `enum class`, `constexpr`, STL containers, smart pointers or
  exceptions without asking.** None appear anywhere in first-party code, and the target has 520 KB of SRAM
  and no PSRAM. **Source:** `src/`, `include/` as read 2026-08-27.

## Hot-path constraints

- **Do not add work to `gb_rom_read`, `gb_cram_r`, `gb_cram_w`, `cget` or `lcd_line` without accounting for
  it in the frame budget.** All five are `IRAM_ATTR` and run per-access or per-scanline.
  **Source:** `src/emulator_bridge.cpp:32, 96, 99, 102, 109` (read 2026-08-27).
- **Do state the per-frame cost of a rendering change in the step note.** `reference/ORIGINAL_ROADMAP.md` does this throughout
  (§2.3 "~29k blends/frame, roughly 0.5 ms"; §2.4 "23,040 ANDs/frame"); match that standard.
  **Source:** `reference/ORIGINAL_ROADMAP.md:225-228, 254-258` (read 2026-08-27).
- **Do not allocate in a frame loop.** Allocation happens once in `emu_init()` via `malloc` with null checks
  (`src/emulator_bridge.cpp:160-166`); `String` and heap churn belong in setup paths only.

## Hardware constants

- **Do check both `include/hw_config.h` and `platformio.ini` `build_flags` when changing a pin.** The same
  physical pin is declared twice under two names — `SD_PIN_CS` (`hw_config.h:16`) and `-DSD_CS=5`
  (`platformio.ini:66`). Changing one and not the other produces a silent mismatch.
  **Source:** `include/hw_config.h:16-19`, `platformio.ini:64-68` (read 2026-08-27).
- **Do not reference a pin number as a literal.** Use the `hw_config.h` macro; there is no second source of
  truth inside C++ code.
- **Do treat `reference/ORIGINAL_ROADMAP.md` §1.2 pin rows as `proposed` or `confirmed` per the row's own Status column**, and
  do not promote a `proposed` row to settled without the §11 bench test.
  **Source:** `reference/ORIGINAL_ROADMAP.md:58-74` (read 2026-08-27).

## Theme tokens

There is no design-token system in this project — `apo mine theme-sources` found no sources. The analogous
rail concerns the palette table:

- **Do read `src/emulator_bridge.cpp:61-88` before referencing a palette.** Colours are raw RGB565 hex
  literals with no named constants; there is nothing to guess correctly.
- **Do keep `NUM_PALETTES` (`include/emulator_bridge.h:22`), `pals[]` (`:61`) and `palnames[]` (`:83`) in
  agreement.** They are three parallel declarations with no compile-time link between them.
- **Do not assume the palette shape is stable.** `reference/ORIGINAL_ROADMAP.md:230-266` widens it from `[N][4]` to `[N][3][4]`
  and replaces the lookup with a flat 64-entry LUT; `reference/ORIGINAL_ROADMAP.md:216-223` warns that byte-swap ordering
  interacts with blending. **Source:** `reference/ORIGINAL_ROADMAP.md:206-266` (read 2026-08-27).

## Vault-artifact citations in generated content

- **Do not** embed vault IDs or vault paths in anything that ships: no `DEC-NNNN`, `OBS-NNNN`, `PHASE-NN`,
  `STEP-NN-NN` or `SESSION-*` identifiers, and no `02_Work/**`, `01_Knowledge/_pending/**` or
  `01_Knowledge/_archive/**` wikilinks, in code comments, test titles, runtime strings, commit messages, or
  the body of `01_Knowledge/*` rails.
- **Do** keep vault IDs in their structural homes — file names, frontmatter fields, and cross-references
  inside `02_Work/**`.
- Domain item IDs referenced as kanban items (`TASK-NNNN`, `BUG-NNNN`) are allowed.
- Enforced by `/apo:lint`'s "Check for vault-artifact citations".

## Verification status

Every rail above is cited to `reference/ORIGINAL_ROADMAP.md`, to first-party source, or marked user-confirmed. No rail here is
speculative.
- **Do:** write each acceptance criterion as the check that would fail if the property were false, naming the exact artifact it reads — the assertion and the field it checks, the grep and its expected count, the command and its output — not a comment, a constant's name, or a struct field that doesn't exist. **Do not:** tick a criterion whose only evidence is a comment claiming the property or a constant whose name implies the bound; if no observable exists yet for the property, say so explicitly so making it checkable becomes part of the work, not a discovery at review time.
