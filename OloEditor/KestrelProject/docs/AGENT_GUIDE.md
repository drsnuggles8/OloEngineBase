# KESTREL agent guide

Status: adopted 2026-11-27, replacing the "integration proposal" in the v1.0 draft package (its text is
preserved in the bullets below). This is the one place the agent workflow for KESTREL gameplay work is
written. `CLAUDE.md`, [`.claude/rules/kestrel-design.md`](../../../.claude/rules/kestrel-design.md),
`/start-work` §5a and the [task loop](../../../docs/process/task-loop.md) point here instead of repeating
it; Codex reaches the same files through `AGENTS.md`. Every repository rule in `CLAUDE.md` and the task
loop, including every engineering quality gate, applies to KESTREL work unchanged.

## When this guide applies

This section is the one definition the other instruction files refer to.

A **KESTREL task** is one whose issue carries the `kestrel` label or sits in the *KESTREL: vertical
slice* milestone, or whose diff touches `OloEditor/KestrelProject/`. Every KESTREL task gets exactly one
of three classifications, recorded in `HANDOVER.md` under *Design context*:

- **Gameplay work**: the change alters what a player can do, see, hear, decide or experience (rules,
  simulation, content, balance, UI, audio, player-facing tools), wherever its files live, or it changes a
  decision, an open question or a tunable in these design documents. *Design context* is the
  [design-acceptance block](#the-design-acceptance-block), and the PR carries *Design validation*.
- **Engine-only**: a pure engine capability with no player-facing change (the `OloEngine/` column of
  [ADR 0027 §3](../../../docs/adr/0027-kestrel-lives-in-the-engine-repo-as-a-second-project.md#3-the-enginegame-boundary),
  such as terrain level of detail or crowd rendering). *Design context* is the one line
  `Design context: engine-only — <reason>`, naming any decision that still constrains it (for example
  constitution principle 5: camera, rendering tier, frame rate and time speed never change gameplay).
- **Mechanical design-doc edit**: a link, path, typo, status line or navigation change in these
  documents. *Design context* is the one line `Design context: mechanical doc edit`; follow
  [Editing these documents](#editing-these-documents).

Engine-only and mechanical tasks owe no *Design validation* section and need not read further than
this. **A task that is not a KESTREL task reads none of this** and has no *Design context* section.

## What to read

For gameplay work, in this order, and no more than the task needs:

1. [README.md](README.md): authority and precedence.
2. [VISION.md](VISION.md) and [CONSTITUTION.md](CONSTITUTION.md).
3. This guide.
4. The topic specifications the task touches, including their cross-system contracts.
5. The decisions the task applies. [DECISIONS.md](DECISIONS.md) is the lookup index from ID to canonical
   record and disposition; look IDs up there, do not read it whole each time.
6. The [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) rows (`EXP-nn`) that cite those decisions.

The [archive](archive/README.md) is for tracing a decision's history. It never supplies requirements.

## Authority

- Reference stable decision IDs in plans, issue acceptance, implementation and review. Use
  `DECISIONS.md` to locate the current record; do not treat archive proposals as current requirements.
- Treat approved design, preferred implementation, hypotheses/tunables and rejected alternatives
  differently; the disposition says which. An accepted direction does not approve its numerical examples,
  implementation options or unresolved parts. `OPEN_QUESTIONS.md` and sections labelled proposed are not
  requirements.
- The [north star](../../../docs/process/north-star.md) controls the vertical slice's scope and its
  June 30, 2027 date. The full-game design is not an instruction to build every mechanic in the slice.
  ADR 0027 controls the engine/game boundary.
- Agents may propose evidence-based design revisions; they may not change approved design, resolve an
  open question or promote a hypothesis or tunable to a requirement without the owner's approval
  ([V-16](validation/automated-balancing.md#v-16), [V-24](validation/playtesting.md#v-24)). That
  approval is cited, as a link to or quote of the owner's words, in the commit message that makes the
  change and in the PR body; nowhere else counts. Ask the
  owner when a product choice cannot be resolved from the specification, or when the design conflicts
  with an issue, the north star or an ADR; present evidence and options rather than silently changing it.
  In a task worktree this is a stop that only the user can clear.
- Before implementation check existing code, issue owners and dependencies. Do not duplicate existing
  systems, rewrite issues or silently change the vertical slice.
- Use authoritative gameplay commands and state; preserve camera/speed invariance, accounting,
  identity/ownership, navigation, deterministic replay and content provenance where applicable.

## The design-acceptance block

A gameplay task carries the [acceptance template](validation/acceptance-template.md) twice: as a
**Design context** section in `HANDOVER.md`, written by `/start-work` §5a and completed in Phase 1 if the
handover lacks it, and as a **Design validation** section in the PR body (task loop Phase 4). Both contain
the template's ten fields, in its order:

1. **Design references**: each applied decision ID with its disposition, the specifications read and the
   `EXP-nn` rows touched. "None apply" is a claim to justify, not a default.
2. **Player experience and hypothesis**: what the player should be able to do, and why it should be
   enjoyable, understandable and meaningful ([V-01](validation/playtesting.md#v-01)).
3. **Scope**: the north-star capability it serves and the full-game parts deliberately left out.
4. **Interactions and invariants**: authoritative state, commands, cost and rights, time, RNG, ownership,
   saves, mod versions and accessibility, as far as the change reaches them.
5. **Technical acceptance**: tests, invariants, performance, save/load, headless equivalence. Each one is a
   row in the task loop's verification matrix like any other cell.
6. **Player-facing acceptance**: responsive controls, causes explained, readable presentation.
7. **Validation plan**, in two separate parts: *technical verification* (what an agent runs) and *human
   design validation* (the playtest question, who answers it, on which build).
8. **Evidence**: in the HANDOVER, what will count as evidence; in the PR, the actual results and their
   limits.
9. **Status**: technical `proposed` / `implemented` / `verified`; design `not ready` / `playtest-ready` /
   `human-tested` / `design-validated` / `revision required`.
10. **Risks and approvals**: outstanding risks and assumptions, and any design change that needs the
    owner's approval.

A gameplay issue an agent files carries fields 1 to 7 in its body, in addition to the repository's
issue format and `olo-score` block. Existing issues are not rewritten to add them unless the owner asks.

## Evidence

- Technical verification and human design validation are separate claims
  ([V-02](validation/playtesting.md#v-02)). Agent play, bots, headless labs, replays and screenshots are
  technical evidence. Never label agent play as human testing, and never claim enjoyment from automated
  results. Report actual checks and remaining human evidence separately.
- An agent sets design status to `playtest-ready` at most. `human-tested` and `design-validated` are
  recorded by the owner from human evidence.
- A gameplay PR may be technically complete with human validation outstanding
  ([V-17](validation/automated-balancing.md#v-17)): state it as `Design: playtest-ready; human validation owed:
  <question>` in field 9 (Status). The owed validation is then stated, not a gap; all ten fields of the
  Design validation section are still required.
- Keep raw tester data and credentials out of public issues, repository and logs; only sanitized evidence
  is shared ([V-10](validation/steam-and-feedback.md#v-10)).

## Editing these documents

- Never edit an archived snapshot: a file listed in [archive/SHA256SUMS](archive/SHA256SUMS), which
  records its hash in the v1.0 draft package. The archive's `README.md` (navigation) and the manifest
  itself are not snapshots.
- A mechanical fix in a current document (a link, a path, a typo, a status line) needs no approval.
- A change to a decision's content, a new decision or a resolved open question needs the owner's
  approval, cited as the Authority section says. A new decision gets a new ID, an `<a id>` anchor and a
  `### ID` heading in its topic document, and a row in `DECISIONS.md`. IDs are never reused or
  renumbered.
- `pre-commit`'s `docs-consistency` hook checks every link and anchor in this tree, that every file is
  reachable from [README.md](README.md), that each decision has exactly one record and one
  `DECISIONS.md` row pointing at it, and that no archived snapshot differs from its listed hash. It
  checks structure, not whether the design is right.
