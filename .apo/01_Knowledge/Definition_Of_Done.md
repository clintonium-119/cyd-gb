---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0008"
category: definition_of_done
title: "Definition of Done"
status: planned
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Coding_Standards]]", "[[01_Knowledge/Agent_Workflow]]"]
tags: [apovault, knowledge, definition_of_done]
---

# Definition of Done

Team-owned. Not extractable from the codebase — `/apo:init` does not invent it.

> **(verify)** — the completion gate for a step, a phase, and the workstream.
>
> Expected answers when this section is filled:
> - [ ] **Build gate:** must `pio run` succeed warning-free, or are warnings tolerated? Is a successful build
>       enough, or must the firmware be flashed and booted?
> - [ ] **Hardware gate:** `ROADMAP.md` §10 gives each phase an explicit **Exit:** condition (e.g. Phase 2
>       "a game renders, and you have a measured baseline"). Is meeting that Exit line the definition of done
>       for the corresponding apo phase?
> - [ ] **Measurement gate:** several phases say "re-measure" (§10 Phases 3–6). Must a frame-time number be
>       recorded in the step's Outcome section before the step closes?
> - [ ] **Lint/test gate:** there is no linter and no test suite today
>       ([[01_Knowledge/Coding_Standards]]). Should either become a gate, or does the hardware check stand
>       alone?
> - [ ] **Commit/PR requirements:** must a commit reference the roadmap section it implements? Is there a PR
>       step at all, or does `poc-gb` accumulate direct commits?
> - [ ] **Regression check:** is there a canonical test ROM and a set of screens that must still render
>       correctly before a rendering change is called done?
> - [ ] **Bench-item gate:** `ROADMAP.md` §11 marks Phase 0 blocking. Must its open items be closed before
>       any dependent phase can be marked done?
>
> Look at: `ROADMAP.md:579-652` (phase Exit conditions and the open-items table), `git log --oneline` for
> the commit-message conventions used so far.
