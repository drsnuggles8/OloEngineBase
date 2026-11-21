# Issue Scoring

How we decide what to work on next. Each issue carries raw per-axis scores in its body; the picker
(`scripts/issue_scores.py`) derives a ranking from them, and `/start-work` takes its pick from that
ranking. The model is WSJF / Cost of Delay with *Business Value* replaced by engine-specific value axes,
plus two personal axes (Learning, Fun) that never enter the arithmetic.

**Store the axes, never a derived number.** Score, rank and tier are recomputed on every read; a
stored one goes stale the moment an axis changes. Re-sorting by a single axis ("a Stability day") is
free because the inputs are kept.

The reasons behind each rule are in the [appendix](#appendix--why-the-rules-are-shaped-this-way).

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
- **`scored: YYYY-MM-DD`**: the day the block was last checked against reality. Written by
  `issue_scores.py set`, never by hand (§6.4).
- **`fun_by`**: the user's ruling on a Fun ≥ 8. `user` = confirmed Pull, `agent` = declined (the Fun
  stays a tie-break). Written only by `issue_scores.py pull` (§3.4).

---

## §3 — The picker (`issue_scores.py rank`)

```
Score = Confidence × CoD / Effort
```

Rows sort by group first, then within each group by: normal-value before low-value (§3.5), Score
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
scored: 2026-10-15
```
````

The repo is public, so Learning and Fun are public too. There is no committed scores file and no tier
label. [`scripts/issue_scores.py`](../../scripts/issue_scores.py):

- `rank`: the ranked list, `north star:` / `goal:`, `tier-1:` / `tier-1 slot:`, `large:` /
  `large slot:` and `next:`. `--freeze` (§7), `--no-low-value-floor` (§3.5).
- `pulse`: task PRs merged and issues opened/closed over the last 7 and 28 days, open feature PRs,
  and the north-star milestone's progress. `/start-work` prints it in its report.
- `lint`: everything that makes the backlog lie. Unscored issues, merged work since the score, scores
  older than 45 days, unconfirmed Pulls, stale and unresolvable blocker edges. Exits non-zero on any.
- `set N axis=value …`: re-score and stamp `scored:` with today. `set N` with no axes re-stamps after a
  check found nothing to change. Refuses `fun_by` and `scored`.
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

---

## Appendix — why the rules are shaped this way

**Agent-scored Fun was jumping the queue (§3.4).** On 2026-10-15, five of the seven `Fun ≥ 8` issues
were 2–10 days old, agent-filed (several from one Planet Zoo 2 capture study) and agent-scored. The
ranking's top ten had a median age of 4 days, ranks 51–80 a median of 37, and #1615 (wet coats)
outranked #1584 (Windows CI sccache hitting 0 of 2055 compiles, ~2 h per build). Fun exists to record
the *user's* motivation; an agent rating its own fresh issue 8 was voting it to the front. Of the seven,
the user confirmed #1558 and #1559 and declined the rest.

**Old scores were never revisited (§6.4).** Nothing re-scored an issue after the session that filed it,
and Decay is a fixed number, so an issue's rank kept the enthusiasm of the day it was filed. #1593 sat
at rank 1 for days after #1599 merged its implementation, and each sweep skipped it by hand. The first
`lint` run with these checks found 14 issues with merged work since their score (including #1259,
#1584 and #1594 with delivering PRs merged) and 12 scores older than 45 days. Age alone is not
demoted: it is evidence the score may be wrong, not that it is.

**Force-multiplier work kept losing to features (§3.7).** start-work's tier list says CI, test infra and
tooling compound and should come first, but the score had no notion of tier, so a run of groom features
took every slot. One slot in three is a fixed share that needs no stored state.

**Broad structural work kept losing to small fixes (§3.8).** The PRs merged between 2026-09-15 and
2026-11-20 fixed about 620 bugs in the PR that found them, and most came from broad items: #1694
(renderer ownership, #1330) fixed 30 and filed 6, #1698 (multi-view) fixed 17 and filed 7, #1597 (the
#1259 acceptance scene) fixed 11 and filed 13. A typical small fix found 0–5. No axis counts bugs
that have not been found yet, and effort divides the score, so the backlog's top was all small items.
The slot is the stopgap. A computed discovery term with severity weighting is the follow-up (#1725).

**Discovery feeds the area last worked, so the backlog needs a goal (§3.9).** A renderer sweep files
renderer issues, and the next pick comes from them. On 2026-11-20 most of the 69 open issues were
renderer work, and the top `large:` candidate was an asset for the skin epic, while the stated aim is a
game in 2027. The large slot and #1725 make the loop stronger. The north star is the counterweight,
and it works through the existing Decay rung rather than a new axis.

**Proven-done issues kept ranking (§3, `close-recommended`).** #1687 and #1688 carried "Appears
resolved by #1694" comments from 2026-11-14 and still printed as rank 1 and 2 on 2026-11-20. Every
sweep had to skip them by hand. A comment is invisible to the picker; a label is not.

**The low-value floor (§3.5).** A ratio cannot tell "cheap and worth doing" from "cheap and not worth a
slot". #411 (CoD 5, effort 2 → 1.25) ranked 5th of 13 with no actionable content left; nothing was
mis-scored. CoD answers "is this worth doing at all", which the ratio cannot.

**Blockers are resolved, not trusted (§3.6).** A written `blocked_by` used to block forever. On
2026-09-09 the closed #1123 and #1139 held seven issues out of `rank` entirely, including #1126 and
#1130, the two highest-scoring rows. A blocked row is absent, not low, so nothing in the output
revealed it (#1161).

**External blockers (§2).** #815 sat at rank 7 with an empty `blocked_by` while waiting on two upstream
`repowise` fixes, and every sweep re-derived "not that one" by hand.
