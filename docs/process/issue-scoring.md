# Issue Scoring

How we decide what to work on next. Each issue carries raw per-axis scores in its body; the picker
(`scripts/issue_scores.py`) derives a ranking from them, and `/start-work` takes its pick from that
ranking. The model is WSJF / Cost of Delay with *Business Value* replaced by engine-specific value axes,
plus two personal axes (Learning, Fun) that never enter the arithmetic.

**Store the axes, never a derived number.** Score, rank and tier are recomputed on every read; a
stored one goes stale the moment an axis changes. Re-sorting by a single axis ("a Stability day") is
free because the inputs are kept.

The reasons behind each rule, with the measurements, are in
[issue-scoring-details.md](issue-scoring-details.md).

---

## §1 — The axes

Every axis uses the constrained Fibonacci scale `{1, 2, 3, 5, 8, 13}`. Score **relative to the §4
anchors**, never in the abstract.

### Value: the Cost of Delay

`CoD = Capability + Craft + Stability + Decay`.

**Capability**: does it unlock things the engine could not do, including queued issues?

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| polish, nothing depends on it | self-contained feature, one subsystem | several workflows use it | a subsystem others build on | unblocks multiple queued features |

**Craft**: visible quality, portfolio value, architectural elegance.

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| invisible | debug view / visible-but-minor | solid visible feature | impressive rendering/gameplay feature | flagship demo piece |

**Stability**: does it fix or prevent crashes, data loss, silent corruption, footguns? A bug in a core
path (ECS, serialization, memory, the cross-binding touch-points) poisons everything above it.

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| no stability angle | minor / rare bug | bites occasionally | data loss / silent scene-or-save corruption | crash/corruption in a core path |

**Decay**: does the cost rise if you wait? Most engine work is a 1.

| 1 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|
| flat | mild compounding | harder each month / near milestone | metastasizing / blocks imminent demo | hard deadline or runaway debt |

### Discovery: the bugs the work will find (computed, added to Stability)

Much of a task's value is the bugs it finds and fixes on the way (task-loop Phase 1a). No axis
scores them, so the picker computes a term and adds it to Stability, at most **+5**, to one decimal.
`rank` shows it as `disc:+N`; `rank --no-discovery` gives the plain order. The term is computed on
every run and never written into an issue. Its only human input is the issue's **`areas:`** (§2).

1. **Yield history.** Every merged `feature/*` PR whose body has the task-loop sections *Found and
   fixed* and *Filed, not fixed* (task-loop Phase 4). A PR without them is left out as no data,
   never counted as zero. Its yield is the sum of its found bugs' severities: a fixed bullet's
   `[S<n>]` tag, a filed issue's own `stability:`. An untagged bullet (all of them before this rule)
   gets a keyword guess: crash, corrupt, data loss, leak, race, deadlock and the like → 8; a
   `docs`/`test` commit, typo, log noise → 1; anything else → 3. A guessed severity is marked
   **estimated** (`~`) wherever it is shown. A filed issue is credited once, to the first PR that
   lists it.
2. **Areas of a PR.** The files its merge commit changed against its first parent, mapped to a
   fixed area list (`AREAS` in the script): Renderer, Groom, Vulkan/RHI, Scene/ECS, Serialization,
   Physics, Scripting, Animation, Audio, Networking, Assets, Editor, Gameplay, Core, Tests/CI,
   Tooling. A PR's share in an area counts distinct directories, not files.
3. **Effort of a PR.** The summed `effort` of the issues it delivered **and closed**. An issue still
   open after the PR was delivered in slices, so its effort is the epic's: the unknown case uses 2.
4. **Per area**, the expected severity points per unit of `effort^0.5`:
   - *rate*: recent yield per unit of effort in the area (PRs weighted by a 60-day half-life,
     shrunk toward the all-area rate);
   - *staleness*: how harvested the area is now compared with when its rate was measured. Each PR's
     yield depletes its areas, and the depletion halves every 30 days and every 10 later PRs' worth
     of changes to the area. A sweep drops it; quiet months and new code restore it. Capped at 2,
     because how much a never-worked area yields is not measured;
   - *level*: one multiplier, Σ actual / Σ predicted over the history, so a prediction reads in
     found severity points.
5. **Per issue**: `effort^0.5 ×` the mean of its areas' factors, then ÷ 10 into Stability points,
   capped at 5.

`issue_scores.py yield` prints the per-area table and, for every merged PR, the yield predicted
from the PRs before it against what it found. That check is what the term is held to; see the
[details](issue-scoring-details.md#what-the-discovery-term-was-set-on) for the numbers it was set on.

### Effort (the denominator)

Relative story size **including verification cost**. For a rendering change, capturing and inspecting
frames from several angles often outweighs the code.

| 1 | 2 | 3 | 5 | 8 | 13 |
|---|---|---|---|---|---|
| routine single PR, ~hundreds of lines (**the common case**) | chunky single PR, ~900 LOC / 15+ files | self-contained system, several days | ~a week, multiple PRs | multi-week across subsystems | multi-feature epic |

Effort is work size, not wall-clock to merge; CI and review latency are not effort.

### Confidence (the multiplier)

`1.0` known territory · `0.8` some unknowns, no fundamental risk · `0.5` spike first (rendering work
where you will not know it looks right until you try).

### Personal axes: never divided by effort

- **Learning**: `1` nothing new → `5` read about but never built → `13` frontier stretch.
- **Fun**: `1` chore → `5` enjoyable → `13` can't wait. **Fun is about the user.** An agent scoring an
  issue it filed leaves Fun at its honest estimate and never writes `fun_by` (§3.4).

---

## §2 — Tags (metadata, not scored)

- **Kano**: `table-stakes` / `performance` / `delighter`. The top of the rank must always carry
  table-stakes coverage.
- **Tech-tree edges**: `blocked_by: [N, …]` / `blocks: [N, …]` (bare numbers; `#N` is accepted). An
  open blocker excludes the issue from the picker; a closed one does not (§3.6).
- **External blockers**: `blocked_by_external: ["…"]`, free text for what the tech tree cannot name (an
  upstream release, a vendor fix). Blocks unconditionally. Omit the field when there is none. Not a way
  to park unwanted work; fix the axes instead.
- **Areas**: `areas: ["Renderer", "Vulkan/RHI"]`, the areas the work will touch, from the §1
  *Discovery* list. The input of the discovery term; `lint` fails on an open issue without it or
  with an unknown name. `set N areas=[Renderer,Vulkan/RHI]` writes it as a JSON list (an older
  copy of the script cannot parse bare names) and does not re-stamp `scored:`, because naming
  areas is not a re-check of the axes.
- **`scored: YYYY-MM-DD`**: the day the block was last checked against reality. Written by
  `issue_scores.py set`, never by hand (§6.4).
- **`fun_by`**: the user's ruling on a Fun ≥ 8. `user` = confirmed Pull, `agent` = declined (the Fun
  stays a tie-break). Written only by `issue_scores.py pull` (§3.4).

---

## §3 — The picker (`issue_scores.py rank`)

```
Score = Confidence × (CoD + Discovery) / Effort  (+ Goal lift)
```

Discovery is the §1 computed term (0 with `--no-discovery`). The goal lift is 0 except for a goal
issue once a north star is set (§3.10), and is added to the score itself. Rows sort by group first, then within each group by: normal-value before low-value (§3.5), Score
descending, Learning, Fun, issue number.

| Group | Rows | Pickable |
|---|---|---|
| 0 | Pull-override (§3.4) | yes |
| 1 | startable | yes |
| 2 | needs triage: work merged since the last score (§6.4), or labelled `close-recommended` | no, triage first / the user closes it |
| 3 | blocked (§3.6) | no |

`next:` is the first row of group 0 or 1, unless the Tier-1 slot (§3.7) or the large-item slot
(§3.8) is due; it says why when it departs from the printed order.

**`close-recommended`** is the label a sweep adds next to its evidence comment when it proves an
open issue done (start-work §2b). The row is flagged `close?`, sorts into group 2 and never becomes
`next:`. `lint` lists these issues as waiting for the user, which does not fail `lint`. The user closes
the issue, or removes the label if the evidence is wrong.

### §3.4 — Pull-override

`Fun ≥ 8` **with `fun_by: user`** pulls an unblocked issue into group 0. Only the user grants it:
`python scripts/issue_scores.py pull <N>…`. A decline is recorded too, with `pull --off <N>…`
(`fun_by: agent`), so the question is asked once. `olo-command-guards` refuses `pull`, and any
`gh issue create|edit` whose text or body file sets `fun_by: user`, unless the user approved it in the
conversation (`# OLO_USER_APPROVED`). A Fun ≥ 8 the user has not ruled on is only a tie-break,
flagged `PULL?`, and `lint` lists it until they answer.

### §3.5 — Low-value floor

`CoD < 6` sorts below every normal-value row in its group and is flagged `low-value:<CoD>`. It demotes,
never hides. A confirmed Pull overrides the floor. `rank --no-low-value-floor` drops this step only.

### §3.6 — Blocker state is resolved, never assumed

The picker resolves every distinct `blocked_by` number on each run (one batched GraphQL call, not
cached). A closed issue or merged PR does not block. A partially delivered tree prints `blocked:` with
only the open edges. A blocker whose state cannot be read keeps blocking, flagged `blocked?:<N>` with a
counted banner: assuming open hides work, and assuming closed invents work out of a network error.

### §3.7 — Tier-1 slot

Tier 1 is start-work's force-multiplier work: CI, test infrastructure, MCP and dev tooling. It is
marked with the `tooling` label (`ci-nightly-red` also counts). If none of the last **3** started tasks
(`feature/*` PRs, open or merged) closed a Tier-1 issue or carried the label, the slot is **due** and
`next:` becomes the top startable Tier-1 row. `rank` prints the `tier-1:` candidate and the slot state
every run. If the PR history cannot be read it says so and `next:` follows the plain order. A
`/start-work` batch of two or more always gives one slot to Tier 1.

### §3.8 — Large-item slot

A large item is an issue with `effort ≥ 5`. Effort divides the score, so a large item never reaches
the top of the rank on its own, yet broad structural work finds and fixes the most latent bugs. The
slot is **due** when none of the last **3** started tasks closed a large issue **and** no open
`feature/*` PR closes one. A large item takes a week or more, so one still in review keeps the slot
filled while small tasks start beside it. `rank` prints the `large:` candidate (the top startable
large row) and the slot state every run. When the slot is due and the Tier-1 slot is not, `next:`
becomes the `large:` row. When both are due, Tier 1 takes the single pick and the large slot stays
due for the next one. A `/start-work` batch of two or more gives one slot to the `large:` candidate
while the slot is due. The script cannot see a worktree that has no PR yet. A live worktree whose
HANDOVER names a large issue counts as in flight; the sweep checks for one.

### §3.9 — North star

[north-star.md](north-star.md) states the project goal and names the GitHub milestone that holds its
critical path. Only the user sets it. While `milestone:` is empty, nothing below applies and `rank`
prints `north star: NOT SET`. Once it is set:

- An issue in the milestone is a **goal** issue, flagged `goal`. `rank` prints the top startable one
  as `goal:`, plus the milestone's open, startable and unfiled counts.
- A goal issue scores **Decay ≥ 5**, the §1 "near milestone" rung (8 if it blocks the milestone's
  next demo). `lint` fails on one below it. That is the whole effect on the score: the goal works
  through the existing Decay axis, so there is still one queue.
- The north star's *Critical path* checklist lists what the milestone cannot ship without. `lint`
  fails on an unticked item with no `#N`: the goal must be filed to steer anything.
- A `/start-work` batch of two or more gives one slot to the `goal:` candidate. For a single pick,
  `next:` stays the plain rank, which the Decay rung already lifts.

### §3.10 — Discovery in the rank

The §1 Discovery term enters the score through Stability. How it meets the other rules:

- **Low-value floor (§3.5)** tests the plain CoD. Discovery is arithmetic too, and cannot lift an
  issue over the floor.
- **Tier-1, large-item and goal slots (§3.7–§3.9)** pick their candidates from the rank with
  discovery in it. The slots themselves are unchanged.
- **Goal issues are never pushed down by it.** Discovery favours small items, since yield per unit
  of effort falls with effort, and items in quiet areas. Without a correction, a Renderer goal
  issue sank 7 → 14 under a synthetic milestone
  ([details](issue-scoring-details.md#what-the-discovery-term-was-set-on)). So once a north star is set, a goal
  issue is lifted (`disc:goal`) to the largest discovery bonus among the non-goal issues that
  scored at or below it without discovery. No non-goal issue overtakes a goal issue because of
  discovery, and the lift is no larger, so the goal still works through Decay.
- **An issue without `areas:`** gets 0 and is flagged `areas?`. `rank` prints what the term was
  computed from above the table, and a `[!]` line if merge commits are missing from the clone
  (`git fetch origin`).

---

## §4 — Anchors (reference issues)

| Axis | anchored rungs |
|---|---|
| **Capability** | `3` = #424 (self-contained audio feature) · `8` = #430 (spatial index many subsystems reuse) · `13` = #452 (unblocks rollback netcode + replays) |
| **Effort** | `1` = #424 (routine single PR) · `3` = #430 (multi-day self-contained system) · `13` = #435 (three rendering sub-features) |
| **Craft** | `3` = #457 (visible-but-minor UI polish) · `8` = #459 (impressive gameplay) · `13` = #435, #439 (flagship atmosphere / baked GI) |
| **Stability** | `3` = #446 (real but test-only) · `8` = #454 (prevents save/scene data loss) · `13` = #325 *(closed)* (silently dropped 28 components from every save) |

---

## §5 — Storage: the `olo-score` block

The scores live **in the issue body**, in a fenced `olo-score` block holding only raw inputs:

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
areas: ["Renderer", "Vulkan/RHI"]
scored: 2026-10-15
```
````

The repo is public, so Learning and Fun are public too. There is no committed scores file and no tier
label. [`scripts/issue_scores.py`](../../scripts/issue_scores.py):

- `rank`: the ranked list, `north star:` / `goal:`, `tier-1:` / `tier-1 slot:`, `large:` /
  `large slot:` and `next:`. `--freeze` (§7), `--no-low-value-floor` (§3.5), `--no-discovery`
  (§1 Discovery).
- `yield`: the discovery term's history. The per-area table, each merged task PR's predicted and
  actual yield, and the check: rank correlation of predicted against actual, beside effort alone.
  `--as-of DATE` computes it as of a day, `--pr N` lists one PR's bugs and severities.
- `pulse`: task PRs merged and issues opened/closed over the last 7 and 28 days, open feature PRs,
  and the north-star milestone's progress. `/start-work` prints it in its report.
- `lint`: everything that makes the backlog lie. Unscored issues, missing or unknown `areas:`, merged
  work since the score, scores older than 45 days, unconfirmed Pulls, stale and unresolvable blocker
  edges. Exits non-zero on any.
- `set N axis=value …`: re-score and stamp `scored:` with today. `set N` with no axes re-stamps after a
  check found nothing to change. `set N areas=[…]` alone keeps `scored:`. Refuses `fun_by` and
  `scored`.
- `pull N…` / `pull --off N…`: the user's Pull confirmation or decline (§3.4).
- `apply`: one-time migration from a JSON file.
- `selftest`: the picker's built-in cases. `rank` and `lint` run them first on every invocation.

---

## §6 — Workflow

1. **Issues are born scored.** Both issue templates carry a neutral block; the task-loop hook refuses an
   agent-filed issue without one. A rough score beats none.
2. **Re-score when reality changes**: a dependency lands, a spike resolves unknowns, debt compounds. Use
   `set`, so the date moves with the change.
3. **When the rank disagrees with your gut, fix an input** (an axis or the rubric), never silently
   override it.

### §6.4 — Staleness: a backlog that lies is triaged on sight

`lint` fails on two kinds of stale issue, and `/start-work` clears them before it picks:

- **Merged work since the score.** A merged PR whose branch ends in the issue's number
  (`feature/foo-1561-1566`) or whose title names `#N`, merged after the block's `scored:` date (the
  filing day when unset). The row moves to group 2 with `merged:#PR`. Check the code. If the work is
  done, post the evidence and recommend closure (closing stays the user's call). If something remains,
  re-score it with `set`, which also acknowledges the merge.
- **A score older than 45 days** (`stale:<days>d`). Re-check the issue against the code, then re-score
  it or recommend closure. An unparseable `scored:` is stale and flagged `scored?`.

---

## §7 — Temporary lenses: `--freeze`

`rank --freeze` keeps only issues labelled `performance`, `robustness`, `architecture`, `bug`,
`tooling` or `cleanup`, hiding pure feature work. It filters the candidates and leaves the scores
unchanged. Last used for the 2026-07 feature freeze; opt-in, so plain `rank` is the live lens. A
freeze-eligible issue that does not show up is mislabelled: fix the label.
