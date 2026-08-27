---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0005"
category: architecture
title: "Agent Workflow"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Prompt_Standards]]"]
tags: [apovault, knowledge, architecture]
---

# Agent Workflow

## Agent Roles

**No repository-level agent rails exist.** There is no `CLAUDE.md`, `AGENTS.md`, `.cursorrules`, or
`.pi/agent/AGENTS.md` in this project. **Source:** `ls CLAUDE.md AGENTS.md .cursorrules .pi` → none present
(read 2026-08-27).

The user's machine-level rails at `~/.pi/agent/AGENTS.md` do apply to work in this repo but are not
project-scoped and are not vendored here; they are not restated in this vault.

`ROADMAP.md` is the closest thing this project has to agent rails, and it addresses agents explicitly at
`:14-16`. Its rails are captured in [[01_Knowledge/Prompt_Standards]] rather than duplicated here.

> **(verify)** — whether project-scoped agent rails should exist as a committed file.
>
> Expected answers when this section is filled:
> - [ ] Should a `CLAUDE.md` / `AGENTS.md` be committed so contributors and agents share the same rails, or
>       is `ROADMAP.md` plus this vault's `01_Knowledge/` sufficient?
> - [ ] If committed, should it restate the two governing constraints (`ROADMAP.md:14-23`) or link to them?
> - [ ] Are other people working in this repo with agents, or is this single-user?
>
> Look at: `ROADMAP.md` §0 and §13, `01_Knowledge/Prompt_Standards.md`.

## Workflow Patterns

> **(verify)** — how work moves from roadmap phase to merged code.
>
> Expected answers when this section is filled:
> - [ ] Do `ROADMAP.md` §10 phases map one-to-one onto apo phases, or do they subdivide?
> - [ ] Branching: does each phase get its own branch off `poc-gb`, or does `poc-gb` accumulate all of it?
> - [ ] Is there a review step before merge, or is this solo work?
> - [ ] What proves a phase done — `ROADMAP.md` §10 gives each phase an explicit **Exit:** condition; is
>       that the gate, and who signs it off?
>
> Look at: `ROADMAP.md:579-637` (the phase list with exit conditions), `git log --oneline` for the branching
> pattern used so far.

## Tooling

Observed from the repository:

- **PlatformIO** is the build and flash interface. No `Makefile` or task runner wraps it.
  **Source:** `platformio.ini`, `README.md` step 5 (read 2026-08-27).
- **`.vscode/` is gitignored** (`.gitignore:2`), indicating VS Code with the PlatformIO extension is at least
  one contributor's environment, but no editor config is shared.
- **No CI.** There is no `.github/` directory, so nothing builds or checks on push.
  **Source:** `ls .github` → not present (read 2026-08-27).
- **Post-build artifact copying** is automated: `scripts/post_build_timestamp.py` copies each successful
  build to `builds/gbscanner-<timestamp>.bin`. **Source:** `platformio.ini:10-11`,
  `scripts/post_build_timestamp.py` (read 2026-08-27).

Verification on this project is physical — flashing to hardware and measuring — not automated. See
[[01_Knowledge/Coding_Standards]] § Test conventions and `ROADMAP.md` §11.

## Verification status

The Agent Roles and Tooling sections are cited. Workflow Patterns is genuinely unknown from the repository
alone and is marked `(verify)` rather than guessed.
