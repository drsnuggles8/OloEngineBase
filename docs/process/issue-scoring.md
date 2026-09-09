# Issue Scoring

How we decide what to work on next. This replaces gut-feel / "whatever the
roadmap doc says" backlog ordering with a small, explicit rubric tuned for a
**personal game-engine project** — where there is no commercial "business
value," the work is a dependency tree more than a backlog, and rendering
changes can look done while being wrong.

It is derived from **WSJF / Cost of Delay** (the `value / job-size` model used
in commercial agile) but with the one axis that doesn't survive the move to a
hobby engine — *Business Value* — replaced by engine-specific value axes, and
with two unapologetically personal axes (**Learning**, **Fun**) bolted on the
side.

> **Source of truth is the axes, not a single number.** We deliberately do
> *not* store a priority score — only the raw per-axis inputs, which live in an
> `olo-score` block **in each issue's body** (§5). The score is *derived on
> read* by the picker; the ranking it produces is just the *default lens*
> `/start-work` uses to pick the next task. On any given day you can re-sort by a
> single axis ("today I want a high-Stability cleanup") — the raw axes make that
> free. **Nothing derived (score, rank, tier) is ever stored** — that only goes
> stale the moment you edit an axis.

---

## §1 — The axes

Every axis is scored on a **constrained Fibonacci scale** `{1, 2, 3, 5, 8, 13}`.
Constrained because the gaps are the point: Fibonacci forces "is this *really*
twice that?" decisions and kills the false precision of a 1–10 scale. Estimate
**relative to the anchor issues** in §4, never in the abstract.

### Value side — the Cost of Delay (what you lose by *not* doing it)

These four are the only axes that feed the default ranking. They sum into a
`CoD` (Cost of Delay) numerator.

**Capability / Enablement** — *does it unlock the engine to do things it
couldn't, including unblocking other queued issues?* This is the
engine-completeness driver and usually the dominant axis, because the backlog
is a tech tree.

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| pure polish, nothing depends on it | self-contained feature, one subsystem | several workflows use it | a subsystem others build on | unblocks multiple queued features / foundational |

**Craft** — *visible quality + portfolio value + screenshot-ability +
architectural elegance.* This is our stand-in for "business value": the proxy
for value-to-an-outside-viewer.

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| invisible, no portfolio value | debug view / visible-but-minor | solid visible feature | clearly impressive rendering/gameplay feature | flagship "this is the demo" piece |

**Stability / Correctness** — *does it fix or prevent crashes, data loss,
silent corruption, footguns?* Weighted to feel heavier than its number suggests,
because in an engine a bug in a core path (ECS, serialization, memory, the
cross-binding touch-points) poisons everything built on top of it.

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| no stability angle | minor / rare bug | bug that bites occasionally | data loss / silent scene-or-save corruption | crash/corruption in a core path everything depends on |

**Decay** — *does the cost rise if you wait?* WSJF's time-criticality,
reframed. **Most engine work is a 1** (no external deadline), and that's a
useful signal: when Decay is flat across the board, raw value-over-effort
dominates. Decay earns its keep on compounding tech debt (touch-point sprawl
that gets worse with every new component) and the rare real deadline (a demo).

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| flat over time | mild compounding | harder each month / near milestone | actively metastasizing / blocks imminent demo | hard deadline or runaway debt |

### Effort (the denominator)

**Effort** — relative Fibonacci story-size. **Include verification cost**, not
just authoring cost. For a rendering change the "capture screenshots from
multiple angles and *actually look at the pixels*" tax (see
[`CLAUDE.md`](../../CLAUDE.md) → "Rendering changes MUST be visually verified")
is real and often dwarfs the code change. A logic-only task of the same LOC is
cheaper here.

| 1 | 2 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|---|
| routine single PR — a day or two, ~hundreds of lines (**the common case**) | chunky single PR — ~900 LOC / 15+ files, one sitting | self-contained system — several days / 2–3 PRs | ~a week, multiple PRs | multi-week across subsystems | multi-feature epic — split it |

**Effort is work size, not wall-clock-to-merge.** Calibrated against PR history
(`gh pr list --state merged`): most features land as a *single* few-hundred-line
PR — even substantial ones (area-light shadows #422 at +702/15f, ragdoll
foundation #401 at +931/17f, motion vectors #416). The long pole on the calendar
is CI + review latency, which is **not** effort. So the typical task is a **1**,
the scale is deliberately compressed at the bottom, and an 8 or 13 is a genuine
multi-subsystem epic that should probably be split.

### Confidence (the multiplier)

**Confidence** ∈ `{1.0, 0.8, 0.5}` — how sure are you the approach will work
*and* that "done" will actually be done?

- `1.0` — known territory, clear approach.
- `0.8` — some unknowns, but no fundamental risk.
- `0.5` — **spike first.** This is where "tests green, screen wrong" lives:
  rendering work where you won't know if it looks right until you try. A `0.5`
  is a signal to prototype/timebox before committing, not to commit and hope.

### Personal axes — scored, stored, but **never divided by effort**

These are about *you*, not about the artifact, so they stay out of the value
math. They are tiebreakers and override levers.

- **Learning** (`1` nothing new → `5` a technique you've read but not built →
  `13` frontier stretch).
- **Fun** (`1` chore → `5` enjoyable → `13` can't wait).

Keeping them separate means a boring-but-educational task (high Learning, low
Fun) and a shallow-but-delightful one (low Learning, high Fun) stay
distinguishable instead of averaging into a meaningless middle.

---

## §2 — Tags (metadata, not scored)

Two things that a number can't express but the picker needs:

- **Kano class** — `table-stakes` / `performance` / `delighter`. This
  operationalizes the "engine completeness" goal. Without it you systematically
  starve the unsexy-but-required baseline ("every engine needs working X") in
  favor of delighters. **Rule: the top of the ranked list must always carry
  table-stakes coverage** — completeness is a conscious budget, not an accident.
- **Tech-tree edges** — `blocked_by: [#N, …]` / `blocks: [#N, …]`. The
  dependency graph WSJF cannot model. **A `blocked_by` naming an issue that is
  still open excludes the issue from the picker entirely, regardless of score**
  — you can't pick what you can't start. **A closed blocker does not block**:
  the picker resolves each number's state on every run, so an edge whose
  blocker has merged stops holding its dependents back the moment it merges,
  with no body edit. See §3.6.
- **External blockers** — `blocked_by_external: ["…", …]`, a list of free-text
  reasons. Same effect on the picker as `blocked_by`, for everything the
  in-repo tech tree cannot name: an upstream release we are waiting on, a
  compiler feature that has not shipped, a vendor fix. Omit the field entirely
  when there is none — an empty list on every issue is noise.

  The picker's question is *"can I start this today?"*, and an issue waiting on
  someone else's release answers no just as firmly as one waiting on a sibling
  issue. Without this the two looked different: **#815 sat at rank 7** with an
  empty `blocked_by` while waiting on two upstream `repowise` fixes, so every
  sweep re-derived "oh, not that one" by hand and then discarded the finding.
  Ranked rows carry an `external` flag so the demotion is visible rather than
  mysterious.

  Use it for a genuine external dependency, **not** to park work you simply do
  not want — that is what the low-value floor and the axes are for (§6.3: fix
  the inputs, don't hide the issue). A standing `Track:` watch item, which can
  never be "done" and only ever waits on someone else, is the archetypal case.

---

## §3 — The picker (default lens for `/start-work`)

```
CoD   = Capability + Craft + Stability + Decay
Score = Confidence × CoD / Effort          # computed only over UNBLOCKED issues
```

1. Drop every issue still held by a blocker — a `blocked_by` entry whose issue
   is **open**, a blocker whose state could not be read, or any
   `blocked_by_external`. A `blocked_by` entry whose issue is already **closed**
   does not count (§3.6).
2. Pick the **max `Score`**.
3. Break ties with **Learning, then Fun**.
4. **Pull override:** any issue with `Fun ≥ 8` may be pulled to the front
   regardless of `Score` — but the override is *logged* (a note in the pick).
   Motivation is a first-class, deliberate input, not a guilty deviation from
   "the correct task."
5. **Low-value floor:** an issue whose `CoD < 6` sorts **below every
   normal-value unblocked issue** and is flagged `low-value:<CoD>` — **except
   when rule 4 applies.** The Pull-override outranks the floor: `Fun ≥ 8` is an
   explicit "I want this", while the floor exists to catch work that floated up
   on arithmetic alone, and a cheap *and* fun task is precisely the case the
   floor is not meant to bury. So the exact guarantee is: *a low-value issue
   never outranks normal-value unblocked work **unless it carries the
   Pull-override**, in which case it is flagged `PULL low-value:<CoD>` and the
   deviation is visible on the row.*

The printed order is authoritative, and `next:` is simply the first unblocked
row — so the recommendation can never disagree with what is printed at the top.

### §3.6 — Blocker state is resolved, never assumed

**A `blocked_by` entry blocks only while the issue it names is open.** The
picker resolves every distinct blocker number on each run (one batched
`gh api graphql` call, not one call per edge, and not cached between runs) and
treats a closed issue — or a merged PR — as delivered. Three consequences:

- **A blocker that merges unblocks its dependents immediately**, with no body
  edit. The number stays written down; it just stops being believed.
- **A partially-delivered tech tree stays blocked, and the `blocked:` flag names
  only the edges still open.** An issue blocked by a closed #1123 and an open
  #1126 prints `blocked:1126` — never the delivered one.
- **`blocked_by_external` is unaffected** and still blocks unconditionally. It
  is free text naming something outside this repo; there is no state to query.

**If a blocker's state cannot be read, the issue stays blocked and says so.**
An unresolvable edge — a number that does not exist, `gh` offline,
unauthenticated or rate-limited — is flagged `blocked?:<N>` and counted in a
banner above the table. The direction is chosen, not defaulted: assuming *open*
recreates the bug below, and assuming *closed* invents startable work out of a
network error, which costs a whole task slot. "We could not tell" must never
read as "it is fine" (`CLAUDE.md` → *no silent fallbacks*).

`issue_scores.py lint` reports stale edges (blocker already closed — tidy the
body) and unresolvable ones, and exits non-zero on either, so neither waits for
a sweep to trip over it. `issue_scores.py selftest` runs the blocker cases on
their own; `rank` and `lint` run them too, on every invocation.

Before this, a written-down number blocked forever. On 2026-09-09 the closed
#1123 and #1139 were holding **seven** issues out of `rank` entirely, including
#1126 and #1130, the two highest-scoring rows in the whole backlog. A stale row
does not sort low, it is *absent* — so nothing in the picker's output could
reveal it, and it took a `/start-work` sweep comparing the list against the
tracker by hand to notice (#1161).

### Why the floor exists

A ratio model rewards cheap work, and that is usually right — a cheap enabler
*should* out-rank an expensive feature. But `value / effort` cannot tell
"cheap and worth doing" from "cheap and not worth a slot": shrink the
denominator far enough and a near-valueless issue floats to the top on
arithmetic alone.

The motivating case was **#411** (`capability 1, craft 1, stability 2, decay 1`
→ CoD 5, effort 2 → **1.25**), which ranked 5th of 13 while having no remaining
actionable content at all. Nothing was mis-scored — every axis was honest. The
formula was simply being asked a question it can't answer: *is this worth doing
at all?* CoD answers that; the ratio doesn't.

The floor **demotes, never hides** — a low-value issue still appears with its
real score and a `low-value:<CoD>` flag, because a cheap chore you actually want
on a Friday afternoon is a legitimate pick; it just shouldn't displace real work
in the default lens — and, per rule 4 above, an explicit `Fun ≥ 8` still lets it
through. `rank --no-low-value-floor` drops the demotion step only; blocked-handling,
the Pull-override and the Learning/Fun tie-break all still apply.

> Per §6.3, prefer fixing the inputs first: if an issue looks low-value but
> isn't, the axes are wrong — raise them. Reach for the floor only when the
> axes are honest and the *ranking* is still wrong.

Because the raw axes are stored, the formula is just the default. Re-sorting by
`Stability` desc (cleanup day), `Learning` desc (study day), or `Craft / Effort`
(portfolio-screenshot day) is all legitimate and free.

---

## §4 — Anchors (reference issues)

Relative Fibonacci only works if "an 8 looks like *this specific issue*" is
written down **before** the backlog is scored. The anchors below are the
canonical reference points per axis; score everything else by comparison to
them. (A *closed* issue is a valid anchor — it's a reference point, not a task.)

| Axis | anchored rungs |
|---|---|
| **Capability** | `3` = #424 (self-contained audio feature) · `8` = #430 (spatial index many subsystems reuse) · `13` = #452 (unblocks dead rollback netcode + replays) |
| **Effort** | `1` = #424 (routine single PR) · `3` = #430 (multi-day self-contained system) · `13` = #435 (three rendering sub-features — split it) |
| **Craft** | `3` = #457 (visible-but-minor UI polish) · `8` = #459 (impressive, demo-able gameplay) · `13` = #435, #439 (flagship atmosphere / baked GI) |
| **Stability** | `3` = #446 (real but test-only) · `8` = #454 (prevents save/scene data loss) · `13` = #325 *(closed)* (silently dropped 28 components from every save — corruption across a core path) |

---

## §5 — Storage: the `olo-score` block

The canonical scores live **in the GitHub issue body** — one source of truth,
where the work is, born with the issue, can't rot out of sync with a separate
file. Each scored issue carries a visible `## Score` section whose fenced
`olo-score` block holds **only the raw inputs** (no derived score/tier):

````markdown
## Score
```olo-score
capability: 8
craft: 3
stability: 3
decay: 1
effort: 3
confidence: 0.8
learning: 3
fun: 3
kano: table-stakes
blocked_by: []
blocks: []
```
<sub>Rated per [issue-scoring](../../docs/process/issue-scoring.md) · score = confidence × (capability + craft + stability + decay) / effort, derived by the picker.</sub>
````

The fenced ` ```olo-score ` info-string is the parse delimiter: visible and
skimmable on GitHub web (renders as a code block), yet unambiguous to read back.
The repo is public, so **learning/fun are public too** — that's an accepted
trade for a single self-contained source of truth.

Tooling lives in [`scripts/issue_scores.py`](../../scripts/issue_scores.py):

- `rank` — pull every open issue, parse its block, print the ranked list (with
  the Pull-override and the §3 low-value floor applied). This is what
  `/start-work` calls. `--no-low-value-floor` ranks without low-value demotion
  (blocked-handling, the Pull-override and the tie-break are unaffected).
- `lint` — list open issues with **no** `olo-score` block (the "needs-score"
  nudge).
- `apply` — one-time / migration helper: write blocks into issue bodies from a
  local source (used to seed the backlog; not part of the steady state).

There is **no committed scores file** and **no tier label** — ranking is always
recomputed from the issues on demand.

---

## §6 — Workflow

1. **New issues are born scored.** The feature issue template
   (`.github/ISSUE_TEMPLATE/`) carries an `olo-score` block pre-seeded to neutral
   (`3`s, `confidence: 0.8`, `kano: table-stakes`). Fill it in by comparison to
   the §4 anchors — a rough score beats no score. `issue_scores.py lint` flags
   any that slipped through unscored.
2. **Re-score when reality changes** — edit the block in the issue body: a
   dependency lands (clear `blocked_by`), a spike resolves the unknowns
   (`Confidence` 0.5 → 1.0), debt compounds (`Decay` climbs), or the work ships
   (close it).
3. **Calibration gold:** when the derived rank disagrees with your gut about
   what to do next, *that's the signal* — either an axis is mis-scored (fix the
   number) or the rubric is missing something (fix the rubric). Don't silently
   override; adjust the inputs so the system learns.

---

## §7 — Temporary lenses: `--freeze`

`issue_scores.py rank --freeze` re-sorts using the same axes but first drops any
issue that carries **none** of the labels `performance`, `robustness`,
`architecture`, `bug`, `tooling`, `cleanup` — i.e. it hides pure net-new-feature
work. `tooling` is the standing exception for the MCP diagnostics server /
dev-tooling / codegen line of work (it doesn't get its own value axis; it's a
policy carve-out, applied by hand to the issues it covers). `cleanup` covers
config/dead-code/smell work that isn't perf/robustness/architecture-labeled but
still isn't a feature (e.g. #411's SonarQube setup issues).

This is a **policy lens on top of the rubric, not a rubric change** — the
underlying score/rank is untouched; `--freeze` only filters the candidate set
before sorting. First used 2026-07 for a month-long feature freeze (perf /
robustness / architecture / cleanup focus, MCP+codegen exempted), **ended
2026-07-31**; the flag is opt-in, so plain `rank` is the live lens again and the
lens stays available for the next one. If an issue that's clearly freeze-eligible
doesn't show up, it's very likely mislabeled — fix the label, not the filter.
