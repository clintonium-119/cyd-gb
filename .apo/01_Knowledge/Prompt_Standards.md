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
updated: '2026-09-25'
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
  **One narrow exception (user-confirmed 2026-09-25):** a games list may exist only while a builder has
  switched the per-unit games-list fallback mode on from the diagnostics System page. It is off by default,
  opens only where a boot would otherwise halt "No cartridge" or "Reader not responding", launches games,
  and writes no tag. The in-game menu's Return to Games List row appears only for a game launched from that
  list. The cart writer, its write path and its single entry are unchanged. Nothing else relaxes: no boot
  combo, no list for tag-launched games, no history.
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
  list of things already considered and rejected, each with the reason: no ROM browser (bar the
  diagnostics-toggled games-list fallback above), no Retro-Go, no
  driving IO4 (the bench proved it is not the amp enable and its real function is unknown), no building on
  the vendor datasheet without metering (it has been wrong twice: the header pinout and IO4), no stretching
  to 240 rows, no blending byte-swapped pixels, no per-pixel cross-palette branch, no manual Save/Load, no
  kid-accessible tag locking, no per-line `pushImage`. (The old no-I²C-on-IO3 entry is moot — I²C lives
  entirely on CN1 as of wiring PDF rev C.)
  **Source:** `reference/ORIGINAL_ROADMAP.md:710-726` (read 2026-09-01).
- **Do:** pick a picture-quality or output-equality fixture for what the change could actually break, and state which property of the fixture makes the artefact visible (e.g. "a full-map scroller, so every background line changes every frame"). **Do not:** default to whatever title or reference ROM is already loaded, and do not read a pass on it as coverage — a reference fixture only catches drift in something already known-good, and says nothing about a property it can't express. If no available fixture exhibits the property, that's a finding to report, not a reason to accept the wrong one.

## `README.md` vs `reference/ORIGINAL_ROADMAP.md` — which wins

`README.md` documents the **inherited fork** (2.8" ESP32-2432S028R, ILI9341, touchscreen, ROM browser).
`reference/ORIGINAL_ROADMAP.md` documents the **target** (2.4" ESP32-2432S024, ST7789, physical buttons, NFC cartridges) and
calls itself "the settled design." **Source:** `README.md`; `reference/ORIGINAL_ROADMAP.md:11` (read 2026-08-27).

- **Do treat `reference/ORIGINAL_ROADMAP.md` as authoritative for intent**, and the current `src/` tree plus `README.md` as the
  starting point being replaced.
- **Do not "fix" code to match `README.md`.** README statements can lag the code; check the tree.
- **Do expect current code to contradict the roadmap**, and say so rather than silently picking a side. Known
  divergences are catalogued in [[01_Knowledge/Integration_Map]] and [[01_Knowledge/Code_Map]] — the button
  expander part (PCF8574 in code vs MCP23017 in the roadmap), the display driver define, `LED_R_PIN` on IO4,
  and the `loop()`-driven ROM browser.

## Formatting

- **Do write new code in the expanded style:** one statement per line, braces on multi-line control flow,
  spaces around operators, descriptive parameter names. Match `src/main.cpp:19-43` and
  `src/sd_manager.cpp:70-82` (both from commit `3dd6145`, 2026-08-04). **Confirmed during /apo:init on
  2026-08-27.**
- **Do not add new compressed one-liner code** in the inherited fork style (`ui_launcher.cpp` 50
  multi-statement lines, `touch_input.cpp` 35). It is legacy, and `reference/ORIGINAL_ROADMAP.md` §9
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
- **Do use `#pragma once`** in new headers — all 20 under `include/` do (read 2026-09-23).
- **Do not introduce classes, namespaces, `enum class`, `constexpr`, STL containers, smart pointers or
  exceptions without asking.** None appear anywhere in first-party code, and the target has 520 KB of SRAM
  and no PSRAM. **Source:** `src/`, `include/` as read 2026-08-27.
- **Do:** name each build image with the arm it belongs to as soon as it's built (not later, by `ls -t` order), and record the mapping in the step's outcome — a build script that names from commit + minute-resolution timestamp will collide multiple images of one commit built in the same minute, overwriting all but the last. **Do not** fix this by folding build flags into the name generator; flags often carry values (feature bypasses, secrets) that shouldn't be echoed into a filename.

## Hot-path constraints

- **Do not add work to `emu_gnuboy_line()` without accounting for it in the frame budget.** gnuboy calls it
  once per scanline through `GNUBOY_DRAW_LINE()` at the tail of `lcd_renderline`.
  **Source:** `src/emulator_bridge_gnuboy.cpp:1072`, `include/gnuboy_hook.h:24`, `lib/gnuboy/lcd.c:714`
  (read 2026-09-23).
- **Do state the per-frame cost of a rendering change in the step note.** `reference/ORIGINAL_ROADMAP.md` does this throughout
  (§2.3 "~29k blends/frame, roughly 0.5 ms"; §2.4 "23,040 ANDs/frame"); match that standard.
  **Source:** `reference/ORIGINAL_ROADMAP.md:225-228, 254-258` (read 2026-08-27).
- **Do not allocate in a frame loop.** Allocation happens once in `emu_init()` via `malloc` with null checks
  (`src/emulator_bridge_gnuboy.cpp:1188-1193`, read 2026-09-23); `String` and heap churn belong in setup paths only.
- **Do not** validate a design against a proxy metric alone; when a design is tuned against a countable stand-in for a property a person judges (pixels changed, underrun count, peak frame time), assert the bound that proxy was standing in for too, on the other side. A proxy driven to its extreme is often the point where the real property it stood for is gone — check what that extreme looks like before trusting a one-sided assertion on it.
- **Do:** treat a tear as a count of write-order pairs — spatially adjacent regions written far apart in time — not as a size to shrink; a monotonic sweep minimizes that count (fewest possible boundaries) while any interleaved, scattered, or tiled write order maximizes it, and a shorter boundary from splitting the write does not compensate for having more of them, since boundary count dominates boundary length. **Do not:** redistribute or reorder a display write to "spread out" a visible artifact — check the pair count analytically before spending a bench window on a candidate order.
- **Do:** before predicting what fraction of boots will be tear-free, establish the direction a frame write runs relative to the panel's refresh sweep — writing against the sweep crosses the scan exactly once per frame (phase sets only _where_ the seam lands, every boot has one), while writing with the sweep can stay ahead for a whole frame (phase then sets _whether_, which is what makes a fraction meaningful). **Do not** assume the rotation that orients the image also fixes the sweep direction: MY/MX/MV remap the frame onto the panel, while separate ML (0x10) and MH (0x04) bits set the refresh order, so a rotation library exposing only some MADCTL combinations can leave the sweep direction unresolved. Settle it by pushing a pattern of known order at the panel and observing, not by reasoning from the datasheet.
- **Do:** measure a new code path's flash cost from a build whose flag actually reaches it, not the default build — espressif32 links with section garbage collection, so an uncalled public function is dropped from the image entirely and its "cost" shows as 0 or as noise from an unrelated refactor in the same file. When the calling code doesn't exist yet, read the new symbols' sizes directly from the ELF (`nm -S --size-sort` against a build that does reach them) rather than diffing one symbol across two builds — inlining can move a function's own reported size by an order of magnitude between builds without the total flash budget moving at all.

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
- **Do:** treat the static DRAM segment (`dram0_0_seg`) as nearly full — with the frame path's buffers in place, headroom is about 15 KB. Put any new buffer over a few KB on the heap, allocated once in `emu_init()` with a null check, and record the `.dram0.bss` delta for buffer-shaped changes in the step note. **Do not** read PlatformIO's aggregate "RAM: NN%" line as static headroom — it counts the whole RAM including heap, not the static segment.
- **Do:** write compile-time guards against the arithmetic quantity a constraint actually binds (e.g. transfer pixel count divisible by four), not against the specific configuration observed failing when the guard was planned. **Do not:** enumerate today's failing combinations by name — an enumerated guard stops guarding silently once the configuration space changes and the enumeration goes stale, because it then simply compiles and passes.
- **Do:** answer a risk about a third-party library's behaviour (or geometry constants in this repo) by reading the vendored source under `.pio/libdeps/<env>/` before booking bench time — it's the exact version the image links, and the answer is usually a grep away. **Do not:** classify a library-behaviour question as a hardware unknown; that misroutes an answerable question onto the bench, where a wrong guess costs a scarce test window instead of a desk-side grep.
- **Do not** trust a datasheet register on this panel until a bench effect is seen; treat every ST7789 feature as unproven until measured (this panel implements only a subset). **Do** pick the observable before writing the register, and prefer one with an unmissable, large-magnitude signal over a subjective "looks better" judgment. **Do not** read a null result (no observable effect) as a negative result (feature confirmed useless) — a silent no-op and a real negative look identical from the outside and imply opposite next actions.

## Theme tokens

There is no design-token system in this project — `apo mine theme-sources` found no sources. The analogous
rail concerns the palette table:

- **Do read `lib/gbcore/render/palette.c` before referencing a palette.** Colours are raw RGB565 hex
  literals with no named constants; there is nothing to guess correctly.
- **Do keep `PALETTE_COUNT` (`lib/gbcore/render/palette.h:32`), `pals[]` (`palette.c:26`) and `palnames[]`
  (`palette.c:99`) in agreement.** They are three parallel declarations with no compile-time link between them.
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
- **Do:** after writing an acceptance criterion, re-read the step's own preconditions and DO-NOT list against the observable it names, and confirm that observable is reachable from that starting state — for firmware, trace the actual code path rather than the design doc's description of it; an observable printed after an early-exit the step itself induces is not reachable, it is a contradiction. **Do not:** infer a probe point or reachable state from a plan's prose just because the named artifact exists somewhere in the system. Where an observable is genuinely unreachable in this step, move it to the first step that can reach it and record the reason in both places.
- **Do:** establish a branch's actual contents with `git rev-parse`, `git diff --stat` against its base, or a grep of the file in question, before recommending it as a base or describing what it carries. **Do not:** infer a branch's contents from `Active_Context.md`, a workstream summary, or a phase note — those describe intent and work in flight, and go stale the moment work is stashed, reverted, or abandoned rather than committed.

## Verification status

Every rail above is cited to `reference/ORIGINAL_ROADMAP.md`, to first-party source, or marked user-confirmed. No rail here is
speculative.
- **Do:** write each acceptance criterion as the check that would fail if the property were false, naming the exact artifact it reads — the assertion and the field it checks, the grep and its expected count, the command and its output — not a comment, a constant's name, or a struct field that doesn't exist. **Do not:** tick a criterion whose only evidence is a comment claiming the property or a constant whose name implies the bound; if no observable exists yet for the property, say so explicitly so making it checkable becomes part of the work, not a discovery at review time.
- **Do not:** phrase an acceptance criterion as a bare token grep over a whole file. If the token also occurs in ordinary English prose (e.g. `millis` inside `millisecond`, `return` inside `returns`) or inside a comment the same plan separately mandates, a whole-file grep is unsatisfiable — the executing agent must violate the plan, silently reword prose to dodge the grep, or stop and ask. Scope the grep to the construct that actually carries the property (`grep -cE '^#include.*(Arduino|esp_)'` for a dependency, `grep -c 'millis()'` for a call, not the bare word) and, when the plan requires prose that itself must contain the forbidden token, state that exception in the criterion.
- **Do:** derive every number an acceptance criterion asserts — expected values, case counts, pixel or byte budgets — by walking the sequence, counting the list, or doing the arithmetic, and show that working in the criterion (`6 x 26 + 40 = 196 <= 216`, not "fits the window"); where a number genuinely cannot be settled until execute, mark it `(verify)` with the fallback named. **Do not:** write an expected value you have not traced, a count you have not counted, or a budget you have not multiplied out — a criterion with a wrong number reads as settled, so the agent that finds it wrong has to disprove it before it can proceed, instead of just meeting a missing one.
- **Do not treat a scaffold verb's success or a lint pass as proof of correctness:** re-running a scaffold command against an existing file is not guaranteed to be idempotent (it can append a duplicate block instead of updating in place), and a structural linter that parses only the first well-formed block will report clean even when a stale or duplicate block sits behind it. Verify generated/updated files by reading the whole file, not by trusting the tool's exit status or lint output alone.
- **Do:** before writing a criterion that adds, subtracts, or compares two or more measurements, read where each one is taken in the code and state the containment relationship in the step — if one is measured inside the call the other wraps, name the derived quantity explicitly (e.g. `core = total − nested_a − nested_b`) rather than leaving readers to infer it. **Do not:** treat fields printed on the same diagnostic line as independent just because they're adjacent in the output and have distinct names — adjacency in a log line says nothing about the call graph, and a well-named field invites exactly that wrong assumption, which can misattribute a slow path's cost to a different subsystem entirely.
- **Do:** when a step needs a measurement, look first for a form of it counted in units the code itself already emits — frames pushed, samples written, blocks queued — before reaching for seconds or hertz. A count needs no assumed clock rate and no calibration constant, so it can't be biased by a stale anchor value and its own resolution floor (where the rounding first exceeds the target precision) states itself in the same units, with no unit conversion to get wrong. **Do not:** default to a physical-unit measurement (an interval in seconds, a frequency, a calibration constant read from a prior bench measurement) when an equivalent count is available; keep any seconds-or-hertz conversion at the point where a human reads the number, not inside the calculation itself.

## Bench fixture fidelity

- **Do:** decide, for every bench fixture, whether it drives the shipping output path or reimplements it — and when it reimplements it, name in the file what it assumes about that path. **Do not:** trust a fixture's own comment about what it shows: a fixture that reimplements a pipeline stage keeps producing a confident picture after the path's order or contract changes, while a fixture that only swaps content at the top of the real path inherits such changes for free.
- **Do:** subtract `qstall` from `emu` before comparing two arms whose consumers do different amounts of work — `emu` is measured around the emulator step and includes any time spent waiting on the other core's queue, so a cheaper consumer makes an unrelated producer look faster. Treat the net-of-stall figure as a bound unless both arms carry a matched, real consumer load; read the idle/wait time as the actual spare budget, not anything derived from the frame period.

## Plan note corrections

- **Do:** rewrite a plan section from the settled position when a bench result or decision falsifies it — state what now holds, and name what died only as a brief "not this" list pointing at the decision that killed it. **Do not** append the correction below the falsified text and leave it standing; stacked corrections hide which layer is current and duplicate the history that decision records already keep.
