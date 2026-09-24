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
updated: '2026-09-23'
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

The nearest equivalent is the palette table `pals[PALETTE_COUNT][3][4]` in `lib/gbcore/render/palette.c:26` —
named three-ramp RGB565 sets with a parallel `palnames[]` at `:99`, and `PALETTE_COUNT` defined at
`lib/gbcore/render/palette.h:32`. Colours are raw hex literals; there are no named colour constants. When
adding or editing a palette, all three of `PALETTE_COUNT`, `pals[]` and `palnames[]` must stay in agreement.
**Source:** `lib/gbcore/render/palette.{h,c}` (read 2026-09-23).

## Test conventions (observed)

Host-side Unity tests run under PlatformIO's native environment: `pio test -e native`. One suite per
module at `test/test_<name>/test_main.c`; suites cover the gbcore seams, a host toolchain check, gnuboy core checks (`test_gnuboy_core`),
a headless emulator smoke test, and a golden-frame hash regression over the committed
`test/roms/dmg-acid2.gb` (MIT, provenance in `test/roms/README.md`). `.github/workflows/ci.yml` runs
`pio run -e cyd` + `pio test -e native` on every push and PR.
**Sources:** `platformio.ini` (`[env:native]`, `test_framework = unity`), `test/`,
`.github/workflows/ci.yml` (read 2026-08-31).

### Core module boundary and tests

Any function with no `Arduino.h` (or ESP-IDF) dependency lives in `lib/gbcore/` as pure C and gets a
Unity test (`ROADMAP.md` §3 rule 3: "host tests for anything pure"). `src/` files are thin Arduino
wrappers over gbcore, so the tested code is the shipped code — PlatformIO's LDF builds the same
sources for `env:cyd` and `env:native`. The test-only headless runner in `lib/gnuboy_runner/` sits beside
gbcore but must never be referenced from `src/`. `[env:native]`'s emulator build flags must mirror
`[env:cyd]`'s exactly, or the golden-frame test pins the wrong configuration (comment at the
`[env:native]` block).
**Sources:** `lib/gbcore/`, `platformio.ini`, `ROADMAP.md` §3 (read 2026-08-31);
`lib/gnuboy_runner/gnuboy_runner.h` (read 2026-09-23).

On-target verification remains bench measurement on hardware. `reference/ORIGINAL_ROADMAP.md` §10 gives
each phase an explicit **Exit:** condition, and §11 lists eight bench tests with their consequences. Treat
those as the acceptance criteria a step's Validation section should cite.
**Source:** `reference/ORIGINAL_ROADMAP.md:579-652` (read 2026-08-27).

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
`emulator_bridge.h` (implemented in `emulator_bridge_gnuboy.cpp`) exposes `emu_*`. Match the existing prefix in the file, not the filename.

### Other observed rules

- **Header guards:** `#pragma once`, in all 20 headers under `include/`. Use `#pragma once` for new
  first-party headers. **Source:** `grep -l '#pragma once' include/*.h` → 20 of 20 (read 2026-09-23).
- **Constants and macros:** `UPPER_SNAKE_CASE` `#define`, not `constexpr` or `enum`. Pin and geometry
  constants live in `include/hw_config.h`; module-private tuning constants sit in the `.cpp`
  (`GNUBOY_AUDIO_HEADROOM`, `FF_AHEAD_US`, `DEMO_RATE_MAX`). **Source:** `include/hw_config.h`,
  `src/emulator_bridge_gnuboy.cpp:325, 362, 885` (read 2026-09-23).
- **File-scope state is `static`**, and short — `rom`, `romlen`, `curpal`, `fskip`, `jpad`, `ready`.
  Counts of `static` declarations per file range from 1 (`battery.cpp`, `i2c_bus.cpp`) to 87
  (`emulator_bridge_gnuboy.cpp`). **Source:** `grep -c '^static ' src/*.cpp` (read 2026-09-23).
- **Section banners:** many files use a box-drawing comment rule,
  `// ─── Name ─────────────────────────────────────`, to separate regions. Present in `hw_config.h`,
  `emulator_bridge_gnuboy.cpp`, `main.cpp`, `display.cpp`. Match it when adding a region to those files.
  **Source:** `include/hw_config.h:4`, `src/emulator_bridge_gnuboy.cpp:86, 96, 112` (read 2026-09-23),
  `src/main.cpp:78, 134, 176`.
- **Serial logging is tagged.** Every log line opens with a bracketed module tag — `[SD]`, `[EMU]`,
  `[SPIFFS]`, `[TFT]`, `[SAVE]`, `[INIT]`. 24 `Serial.printf` calls to 8 `Serial.println`; prefer `printf`
  when there is any value to include. **Source:** `grep -rho 'Serial.print*' src/`, and call sites in
  `src/sd_manager.cpp:14,44,64,72`, `src/emulator_bridge_gnuboy.cpp:1299` (read 2026-09-23),
  `src/main.cpp:53,173`.

### Patterns NOT present

No classes, no namespaces, no C++ exceptions, no RAII wrappers, no `constexpr`, no `enum class`, no smart
pointers, no STL containers. Allocation is bare `malloc` with a null check
(`src/emulator_bridge_gnuboy.cpp:1188-1193`, read 2026-09-23); buffers are fixed-size C arrays. **Ask before introducing any of these**
— the constraint is a 520 KB no-PSRAM target, and the existing code is uniform on this point.
**Source:** first-party `src/` and `include/` files as read 2026-08-27.

---

## API-Usage Patterns (Observed) — `cyd-gb`

**Subsystem path:** `src/`, `include/` — scanned 2026-08-27.

| Area | Newer / preferred | Older / legacy | Verdict |
|---|---|---|---|
| Formatting style | Expanded, one statement per line (`src/main.cpp:19-43, 56-76`; `src/sd_manager.cpp:70-82`) — from commit `3dd6145`, 2026-08-04 | Compressed one-liners (`ui_launcher.cpp` 50, `touch_input.cpp` 35, `main.cpp` 21, `sd_manager.cpp` 18) — inherited from the fork | **Use expanded (user-confirmed).** See [[01_Knowledge/Prompt_Standards]] |
| String handling | Fixed `char[]` with `strncpy` / `snprintf` and explicit NUL termination (`sd_manager.cpp:52-58`; `main.cpp:191-195`) | Arduino `String` — 12 occurrences, mostly in SD directory iteration (`sd_manager.cpp:29-30`) | Prefer `char[]`; `String` in new hot-path or long-lived code risks heap fragmentation on a no-PSRAM target |
| Settings persistence | Arduino `Preferences` (NVS) | — | Single approach. Note the API is currently *behind* `touch_*` names (`include/touch_input.h:26-30`) even though it stores palette / frame-skip / brightness, which are not touch concerns |
| Logging | `Serial.printf` (24) with a `[TAG]` prefix | `Serial.println` (8) | Dominant; use `printf` with a tag |
| Bus access | `SPIClass sdSPI(VSPI)` explicit instance for SD; `Wire` global for I²C; bit-bang for touch | — | Single approach each |

**Source:** `apo mine api-patterns` probes plus the greps and file reads cited inline (read 2026-08-27).

Areas checked and found single-approach, so recorded without an ASK: header guards, naming, constants,
logging, bus access, allocation.

### Tests (subsystem-specific)

`test/test_<module>/test_main.c`, one Unity suite per module, run with `pio test -e native`. See
"Test conventions (observed)" above.

---

## File organization (observed) — `cyd-gb`

Flat. `src/*.cpp` and `include/*.h`, no subdirectories, one header per implementation file plus
header-only `build_info.h`, `gnuboy_hook.h`, `hw_config.h` and `render_config.h` (read 2026-09-23). `README.md` states this is load-bearing for PlatformIO: "the
`.cpp` files MUST be inside the `src/` folder and `.h` files inside `include/`."
**Sources:** `ls -F src/ include/`, `README.md` § File Structure (read 2026-08-27).

New modules should follow the same pairing: `src/<name>.cpp` + `include/<name>.h`, prefix chosen to match the
module's public API rather than the filename.
- **Do:** for a `lib/` module that needs a vendored project header from `include/`, add it explicitly in the library's `library.json` via `"build": {"flags": ["-I../../include"]}` (path relative to the library root) — PlatformIO does not put the project `include/` dir on a library's search path automatically (unlike `src/`/`test/`). **Do not:** copy the vendored header into the library to work around the path.

## Verification status

Cited throughout. The formatting rail is user-confirmed rather than derived. The former `(verify)` on test
strategy was resolved 2026-08-31: host-side Unity tests under `[env:native]` with the pure logic in
`lib/gbcore/` (see "Test conventions (observed)").

## API-Usage Patterns (Observed)

- **Do:** write a card-file replacement to a temp path, verify the write is full-length, remove the destination, then rename the temp file into place; **do not** remove or truncate the destination first. FAT32 journals nothing and the device can lose power mid-write (battery-powered, no clean shutdown), so the destination must stay intact until the replacement is verified and ready to swap in.
