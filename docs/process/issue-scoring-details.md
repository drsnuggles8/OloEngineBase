# Issue Scoring: details

The stories, measurements and derivations behind the rules in [issue-scoring.md](issue-scoring.md).
Each section names the rule it explains.

---

## Why the rules are shaped this way

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

## What the discovery term was set on

The numbers behind the §1 *Discovery* term and §3.10 (#1725), from `issue_scores.py yield` on
2026-11-21: 101 merged task PRs with the template sections (2026-09-05 to 2026-11-20), 639 found
bugs, 543 of them with an estimated severity. Each PR is predicted from the PRs merged before it;
87 had enough history before them to be scored.

- **Yield grows far slower than effort.** Over the 79 PRs with a known effort, yield ≈ 10.2 ×
  effort^0.39: the mean was 14 severity points at effort 1, 17 at effort 3 and 49 at effort 8.
  Hence `effort^0.5`. A broad item finds more bugs than a small one but fewer per unit of effort,
  so the term does **not** carry broad items up the rank on its own. Whether the large-item slot
  (§3.8) is still needed is the checkpoint question for #1725.
- **Discovery runs out.** The model's rank correlation with the actual yield was 0.29; effort alone
  scored 0.13 and the model without staleness 0.15, so the staleness factor carries the skill. The
  constants were chosen from a grid on this check; uncapping staleness changed it by 0.002. #1694
  (renderer ownership) found 113 severity points. Across its merge, Renderer's expected yield per
  `effort^0.5` fell from 12.0 to 10.2, Scene/ECS (7 % of the PR) held at 20.0 to 20.1, and the
  other areas rose with the level.
- **Breadth did not predict yield.** The number of areas a PR mainly worked in (each 20 % or more of
  its directories) had a rank correlation of −0.10 with its yield, so an issue's factor is the mean
  of its areas, not their sum.
- **The level.** Uncalibrated, the predictions came to about half of what was found; the run-time
  level (×1.91 on this history) scales them so a prediction reads in found severity points.

**The goal check.** Two synthetic milestones of five open issues each, goal issues at Decay 5, 60
startable rows. Renderer issues (#654, #1143, #1344, #1353, #1497): with the term and no lift,
#1344 fell from 7th to 15th, #1143 from 27th to 32nd and #654 from 44th to 47th; with the §3.10 lift
none fell (5th, 24th, 41st). Issues in quiet areas (#434, #437, #1046, #1132, #1714): the term alone
moved none of them down, and the lift moved them up by 3 to 5 places. The constants and the reason
for each are at the top of the discovery section in `scripts/issue_scores.py`.
