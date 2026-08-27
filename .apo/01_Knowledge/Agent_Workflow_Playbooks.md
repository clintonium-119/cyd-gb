---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0009"
category: playbooks
title: "Agent Workflow Playbooks"
status: planned
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Coding_Standards]]", "[[01_Knowledge/Code_Map]]"]
tags: [apovault, knowledge, playbooks]
---

# Agent Workflow Playbooks

Team-owned recipes for repeatable tasks. Not extractable from the codebase — `/apo:init` does not invent
them. The candidates below are drawn from the recurring shapes visible in this repository and in
`reference/ORIGINAL_ROADMAP.md` §10, but the steps themselves are unwritten.

> **(verify)** — the repeatable recipes an agent should follow.
>
> Expected playbooks when this section is filled:
> - [ ] **Adding a module.** `src/<name>.cpp` + `include/<name>.h`, `#pragma once`, `<module>_<verb>`
>       prefixed API, file-scope `static` state. Does anything else need touching — `main.cpp` include list,
>       `setup()` call order?
> - [ ] **Adding or changing a pin.** Both `include/hw_config.h` and `platformio.ini` `build_flags` carry pin
>       definitions, and `SD_CS` is declared in both under different names
>       ([[01_Knowledge/Prompt_Standards]] § Hardware constants). What is the checklist that keeps them in
>       sync, and does the change need re-flashing all ten units?
> - [ ] **Adding a palette.** Three parallel declarations must stay in agreement: `NUM_PALETTES`
>       (`include/emulator_bridge.h:22`), `pals[]` and `palnames[]` (`src/emulator_bridge.cpp:61, 83`). Note
>       `reference/ORIGINAL_ROADMAP.md:230-266` is about to reshape all three to `[N][3][4]` — write this playbook after that
>       lands, not before.
> - [ ] **Making a rendering change.** `reference/ORIGINAL_ROADMAP.md` §10 requires re-measuring after Phases 3, 4, 5 and 6.
>       What is the standard measurement procedure — which ROM, which scene, how many frames, recorded where?
> - [ ] **Deleting a fork subsystem.** `reference/ORIGINAL_ROADMAP.md` §9 lists files to delete. `button_input.cpp:2` includes
>       `touch_input.h` for the `GB_BTN_*` constants, so deleting touch breaks buttons
>       ([[01_Knowledge/Code_Map]] § Module Boundaries). What is the general order — move shared constants
>       first, then delete, then rebuild?
> - [ ] **Flashing a unit.** `reference/ORIGINAL_ROADMAP.md:634-637` mentions a pre-assembly flashing station using ESP Web
>       Tools over USB-C, flashing before the board goes in the shell. What is the per-unit procedure and
>       what is verified before the shell closes?
>
> Look at: `reference/ORIGINAL_ROADMAP.md:552-577` (delete list), `:579-637` (phases), `include/hw_config.h`,
> `platformio.ini:19-71`.
