---
note_type: review
template_version: 1
contract_version: 1
title: "<review-title>"
owner: ""
created: '<TODAY>'
updated: '<TODAY>'
tags: [apovault, review]
review_id: "REVIEW-YYYY-MM-DD-HHMMSS"
workstream: "[[02_Work/<branch-path>/workstream|<branch-name>]]"
scope: "<workstream|PHASE-NN>"
verdict: "<passed|changes_requested>"
findings:
  unmet: 0
  undocumented: 0
  sot_update: 0
  unverified: 0
  drift: 0
sources_checked:
  diff_range: "<base>...HEAD"
  tests_run: false
  jira: []
  figma: []
reviewed_on: '<TODAY>'
---

# REVIEW-YYYY-MM-DD-HHMMSS

## Verdict

_One line — `passed` or `changes_requested` — plus a rationale sentence: which finding categories drove it. Blocking categories (unmet acceptance, undocumented deviation, unverified claim) force `changes_requested`; a review whose only findings are source-of-truth updates or spec drift resolves to `passed` with those riding as tracked follow-up._

## Unmet acceptance

_Jira or plan acceptance criteria with no corresponding evidence in the diff or a passing test. `None` if all criteria are met._

## Undocumented deviations

_Delivered code that departs from Jira/Figma with no decision note explaining it — scope creep or a bug. `None` if every deviation is documented._

## Source-of-truth updates recommended

_Intentional, documented deviations from the ticket/design. For each: the divergence, the decision that authorized it, and drafted comment/annotation text plus a PR-description blurb the user can paste. apo never writes back to Jira/Figma — these are recommendations only. `None` if the delivery matches the source-of-truth._

## Unverified claims

_An `## Outcome` claims PASS but the cited test is absent or does not run on a fresh run. `None` if every claimed test exists and passes._

## Spec drift

_The design may have moved under the implementation: the Figma file/node changed after the plan artifact's date. Re-verify against the current design. `None` if no in-scope design post-dates its plan artifact._

## Not checked

_Comparisons that were skipped and why — e.g. `jira: not checked (no jira ref)`, `figma: not checked (MCP absent)`. Never a silent pass. `None` if every in-scope source was reachable._
