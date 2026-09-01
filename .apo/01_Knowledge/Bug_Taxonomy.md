---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0010"
category: taxonomy
title: "Bug Taxonomy"
status: planned
owner: ""
created: '2026-08-27'
updated: '2026-09-01'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Definition_Of_Done]]"]
tags: [apovault, knowledge, taxonomy]
---

# Bug Taxonomy

Team-owned. Not extractable from the codebase — `/apo:init` does not invent it.

> **(verify)** — severity ladder, category enum, lifecycle, and tracker.
>
> Expected answers when this section is filled:
> - [ ] **Severity ladder:** what do sev-1 through sev-4 mean in this project's terms? Firmware suggests
>       candidates worth defining explicitly — boot failure or brick, emulation hang, save-data corruption or
>       loss, wrong rendering, audio artifact, cosmetic. Where does each land?
> - [ ] **Data-loss weighting:** `reference/ORIGINAL_ROADMAP.md:501-508` moves saving to automatic precisely because losing
>       progress is the worst outcome for the users. Should save-data bugs get their own severity floor?
> - [ ] **Category enum:** which of `logic` / `integration` / `regression` / `performance` / `ux` / `docs` /
>       `test` apply? Hardware-specific categories may need adding — e.g. `hardware` (a per-unit assembly
>       or component fault, not a firmware defect) and `timing` (frame-budget overruns, I²C or SPI races).
> - [ ] **Per-unit vs all-unit:** with ten hand-built units, "fails on unit 7 only" is a distinct class from
>       "fails on all units". Should that be a field, a category, or a severity modifier?
> - [ ] **Lifecycle:** what are the states between `new` and `closed`, and who verifies a fix — the person
>       who wrote it, or a second builder on their own unit?
> - [ ] **Primary tracker:** this vault (`apo item list`), GitHub issues on the fork, or something else?
>       There is no issue tracker configured in-repo and no JIRA host set in `.apo/.config.json`.
>
> Look at: `reference/ORIGINAL_ROADMAP.md` §8.2 (the diagnostic screen, which exists to turn "it doesn't work" into a specific
> failing signal), §11 (open bench items — note these are *unknowns*, not bugs, and should not be filed as
> bugs), `.apo/.config.json`.

## Build & Toolchain Gotchas

- **Do:** spell file-parameter types as `fs::File` (not bare `File`) in any `include/*.h`. TFT_eSPI's ESP32 processor header defines `FS_NO_GLOBALS` before including `FS.h`, which suppresses the global `File` alias project-wide once any translation unit reaches the display header first; a header using bare `File` then fails to compile only from `.cpp` files that include the display path first, while compiling fine elsewhere. Do not rely on `#include <FS.h>` or `<SD.h>`'s `using namespace fs;` to make unqualified `File` available in a header — that depends on what the includer pulled in first, which the header can't control.
- **Do:** check for the object file (`find .pio/build/<env> -name "<module>*"`) to decide whether a `lib/gbcore/**` module compiles for a target. PlatformIO's `chain` dependency finder links per **library**, not per header: `lib/gbcore/` is one library, so once any `src/*.cpp` includes any gbcore header, the whole library is linked and every `.c` beneath it compiles — including modules nothing has included yet. Do not infer coverage either way from which headers are included: a library with no `src/` consumer at all is silently absent from the build (and a green `pio test -e native` does not cover it, reaching the module through the test env instead), while one with any consumer covers all of its modules. When target compilation really is unproven, a strict host compile (`gcc -std=c99 -Wall -Wextra -Werror -pedantic`) is the honest interim check.
