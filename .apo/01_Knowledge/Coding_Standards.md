---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0006"
category: standards
title: "Coding Standards"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Code_Map]]", "[[01_Knowledge/Prompt_Standards]]"]
tags: [apovault, knowledge, standards]
---

# Coding Standards

## Lint / format (vault-wide)

**None configured.** No `.clang-format`, `.clang-tidy`, `.editorconfig`, or any other formatter or linter
config exists in the repository. Formatting is by convention only, and the conventions are not uniform — see
"Formatting" below. **Source:** `ls .clang-format .clang-tidy .editorconfig` → none present (read 2026-08-27).

## Build / run commands (vault-wide)

There is no `Makefile`, `justfile` or `Taskfile.yml`. PlatformIO CLI is the interface, per `README.md` step 5:

```bash
pio run -t erase --upload-port /dev/ttyUSB0    # first time only — initialises the SPIFFS partition
pio run -t upload --upload-port /dev/ttyUSB0   # build and flash
pio device monitor -b 115200 --port /dev/ttyUSB0
```

`platformio.ini:10-11` registers a post-build hook, `scripts/post_build_timestamp.py`, which copies each
successful `firmware.bin` to `builds/gbscanner-<YYYYmmdd_HHMMSS>.bin`. `builds/` is not present in a clean
checkout and is not listed in `.gitignore` — verify before committing after a build.
**Sources:** `README.md`, `platformio.ini:10-11`, `scripts/post_build_timestamp.py:13-25`, `.gitignore` (read
2026-08-27).

## Theme Tokens (Observed)

**No token system.** `apo mine theme-sources --json` returned `{"sources": [], "multi_source": false}`. This
is embedded C++ with no CSS, Tailwind or design-token layer. **Source:** `apo mine theme-sources --json`
(read 2026-08-27).

The nearest equivalent is the palette table `pals[NUM_PALETTES][4]` in `src/emulator_bridge.cpp:61-82` —
20 named four-entry RGB565 tables with a parallel `palnames[]` at `:83-88`, and `NUM_PALETTES` defined at
`include/emulator_bridge.h:22`. Colours are raw hex literals; there are no named colour constants. When
adding or editing a palette, all three of `NUM_PALETTES`, `pals[]` and `palnames[]` must stay in agreement.

## Test conventions (observed)

**No tests exist.** No `test/` directory, no file matching `*test*` or `*spec*`, no `.github/workflows/`, and
no test framework in `lib_deps`. **Source:** `find` for test/spec files returned nothing; `ls .github` → not
present; `platformio.ini:73-76` (read 2026-08-27).

Verification in this project is by bench measurement on hardware, not automated tests. `ROADMAP.md` §10 gives
each phase an explicit **Exit:** condition, and §11 lists eight bench tests with their consequences. Treat
those as the acceptance criteria a step's Validation section should cite.
**Source:** `ROADMAP.md:579-652` (read 2026-08-27).

> **(verify)** — whether automated tests are wanted at all for this project.
>
> Expected answers when this section is filled:
> - [ ] Should any logic be extracted for host-side unit testing (e.g. NDEF parsing per `ROADMAP.md` §6.3,
>       filename normalisation per §6.4 — both pure functions with clear inputs)?
> - [ ] If so, PlatformIO `test/` with Unity, or a separate host build?
> - [ ] Is a CI build (compile-only, no hardware) wanted to catch breakage?
>
> Look at: `platformio.ini` (`test_framework`), `ROADMAP.md` §6.3–6.4 for the pure-function candidates.

---

## Naming Conventions (Observed) — `cyd-gb`

**Subsystem path:** `src/`, `include/` — scanned 2026-08-27.

`apo mine conventions` targets JS/TS UI-primitive suffixes (`Dialog`, `Modal`, `Card`, …) and is not
meaningful for embedded C++; run against `src` with C++ extensions it reports no such primitives. The counts
below come from direct greps of the first-party tree.

### Module-prefixed free functions — the dominant rule

Every public function is a free function named `<module>_<verb>`, in `snake_case`, declared in
`include/<module>.h`. Counts of distinct declared functions per prefix:

| Prefix | Declared | Header |
|---|---|---|
| `emu_` | 18 | `include/emulator_bridge.h` |
| `touch_` | 11 | `include/touch_input.h` |
| `sd_` | 7 | `include/sd_manager.h` |
| `display_` | 5 | `include/display.h` |
| `button_` | 3 | `include/button_input.h` |
| `launcher_` | 3 | `include/ui_launcher.h` |
| `bt_scanner_` | 3 | `include/bt_scanner.h` |

**No competing pattern.** There are no classes, no namespaces, and no first-party camelCase functions — every
camelCase call site in `src/` resolves to an Arduino or TFT_eSPI library method (`drawString`, `setTextColor`,
`fillScreen`, …). **Source:** `grep -rhoE '\b<prefix>[a-z0-9_]+\(' include/*.h`, and
`grep -rhoE '\b[a-z]+[A-Z][a-zA-Z]*\(' src/*.cpp` cross-checked against library APIs (read 2026-08-27).

The module prefix does **not** always match the file name: `ui_launcher.{h,cpp}` exposes `launcher_*`, and
`emulator_bridge.{h,cpp}` exposes `emu_*`. Match the existing prefix in the file, not the filename.

### Other observed rules

- **Header guards:** `#pragma once`, in 8 of 9 headers. The exception is the vendored `include/peanut_gb.h`,
  which uses `#ifndef`. Use `#pragma once` for new first-party headers.
  **Source:** `grep -l '#pragma once' include/*.h` → 8; `grep -l '#ifndef' include/*.h` → `peanut_gb.h` only.
- **Constants and macros:** `UPPER_SNAKE_CASE` `#define`, not `constexpr` or `enum`. Pin and geometry
  constants live in `include/hw_config.h`; module-private tuning constants sit at the top of the `.cpp`
  (`PG_SZ`, `PG_N`, `HASH_SZ`, `B0SZ`, `MAXRAM`). **Source:** `include/hw_config.h`,
  `src/emulator_bridge.cpp:15-19, 47, 50`.
- **File-scope state is `static`**, and short — `acc`, `npg`, `romf`, `curpal`, `fskip`, `cram`, `ready`,
  `jpad`. Counts of `static` declarations per file range from 1 (`display.cpp`) to 92 (`bt_scanner.cpp`).
  **Source:** `grep -c '^static ' src/*.cpp`.
- **Section banners:** many files use a box-drawing comment rule,
  `// ─── Name ─────────────────────────────────────`, to separate regions. Present in `hw_config.h`,
  `emulator_bridge.cpp`, `main.cpp`, `display.cpp`. Match it when adding a region to those files.
  **Source:** `include/hw_config.h:4`, `src/emulator_bridge.cpp:14`, `src/main.cpp:78, 134, 176`.
- **Serial logging is tagged.** Every log line opens with a bracketed module tag — `[SD]`, `[EMU]`,
  `[SPIFFS]`, `[TFT]`, `[SAVE]`, `[INIT]`. 24 `Serial.printf` calls to 8 `Serial.println`; prefer `printf`
  when there is any value to include. **Source:** `grep -rho 'Serial.print*' src/`, and call sites in
  `src/sd_manager.cpp:14,44,64,72`, `src/emulator_bridge.cpp:107,125`, `src/main.cpp:53,173`.

### Patterns NOT present

No classes, no namespaces, no C++ exceptions, no RAII wrappers, no `constexpr`, no `enum class`, no smart
pointers, no STL containers. Allocation is bare `malloc` with a null check
(`src/emulator_bridge.cpp:160-166`); buffers are fixed-size C arrays. **Ask before introducing any of these**
— the constraint is a 520 KB no-PSRAM target, and the existing code is uniform on this point.
**Source:** first-party `src/` and `include/` files as read 2026-08-27.

---

## API-Usage Patterns (Observed) — `cyd-gb`

**Subsystem path:** `src/`, `include/` — scanned 2026-08-27.

| Area | Newer / preferred | Older / legacy | Verdict |
|---|---|---|---|
| Formatting style | Expanded, one statement per line (`src/main.cpp:19-43, 56-76`; `src/sd_manager.cpp:70-82`) — from commit `3dd6145`, 2026-08-04 | Compressed one-liners (`emulator_bridge.cpp` 44 multi-statement lines, `ui_launcher.cpp` 50, `touch_input.cpp` 35, `main.cpp` 21, `sd_manager.cpp` 18) — inherited from the fork | **Use expanded (user-confirmed).** See [[01_Knowledge/Prompt_Standards]] |
| String handling | Fixed `char[]` with `strncpy` / `snprintf` and explicit NUL termination (`sd_manager.cpp:52-58`; `main.cpp:191-195`) | Arduino `String` — 12 occurrences, mostly in SD directory iteration (`sd_manager.cpp:29-30`) and the SPIFFS path (`emulator_bridge.cpp:123`) | Prefer `char[]`; `String` in new hot-path or long-lived code risks heap fragmentation on a no-PSRAM target |
| Settings persistence | Arduino `Preferences` (NVS) | — | Single approach. Note the API is currently *behind* `touch_*` names (`include/touch_input.h:26-30`) even though it stores palette / frame-skip / brightness, which are not touch concerns |
| Logging | `Serial.printf` (24) with a `[TAG]` prefix | `Serial.println` (8) | Dominant; use `printf` with a tag |
| Bus access | `SPIClass sdSPI(VSPI)` explicit instance for SD; `Wire` global for I²C; bit-bang for touch | — | Single approach each |

**Source:** `apo mine api-patterns` probes plus the greps and file reads cited inline (read 2026-08-27).

Areas checked and found single-approach, so recorded without an ASK: header guards, naming, constants,
logging, bus access, allocation.

### Tests (subsystem-specific)

None. See "Test conventions (observed)" above.

---

## File organization (observed) — `cyd-gb`

Flat. `src/*.cpp` and `include/*.h`, no subdirectories, one header per implementation file plus
`hw_config.h` and the vendored `peanut_gb.h`. `README.md` states this is load-bearing for PlatformIO: "the
`.cpp` files MUST be inside the `src/` folder and `.h` files inside `include/`."
**Sources:** `ls -F src/ include/`, `README.md` § File Structure (read 2026-08-27).

New modules should follow the same pairing: `src/<name>.cpp` + `include/<name>.h`, prefix chosen to match the
module's public API rather than the filename.

## Verification status

Cited throughout. The formatting rail is user-confirmed rather than derived. The `(verify)` on test strategy
is a genuine open question, not a gap in the scan.
