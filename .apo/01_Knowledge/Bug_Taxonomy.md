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
updated: '2026-08-31'
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

- **Do not:** treat a green `pio run -e <target>` as proof that a new `lib/gbcore/**` module compiles for that target. PlatformIO's `chain` dependency finder only links a `lib/` module into an environment once some `src/*.cpp` in that environment `#include`s its header; an as-yet-unconsumed module is silently absent from the build, and a passing `pio test -e native` doesn't cover it either, since that reaches the module through the test env instead. Until the target-side consumer lands, verify the module with a strict host compile (`gcc -std=c99 -Wall -Wextra -Werror -pedantic`) and say explicitly that target compilation is unproven.
