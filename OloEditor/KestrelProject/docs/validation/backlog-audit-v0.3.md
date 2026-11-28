# KESTREL: requirements-to-code and backlog audit v0.3 (EXP-17 / AUD-18), integrated and applied

Date: 2026-11-28, revised the same day after an independent review of PR #1873 (§11). Master at `09f2aa738`. This version merges the local v0.2 audit with the owner's v0.3 addendum
(both kept outside the repository, unchanged, as working inputs). Where they conflict, the addendum wins. The owner
decisions are public on [#1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7241796309) and
recorded as AUD-21 to AUD-28.

**Status:** the tracker changes in §3 and §4 are live on GitHub. The documentation changes in §6, and this file, arrive
with PR #1873.

Evidence classes in this document:

- **Source/PR evidence:** verified in code or in a merged PR.
- **Tracker state:** read from or written to GitHub on 2026-11-28.
- **Proposal:** engineering direction, not yet built.

Nothing here is human playtest evidence. No 20,000-skier performance has been demonstrated: no guest simulation exists
yet.

---

## 0. Summary

- **Owner decisions recorded publicly.** Q1–Q15 are in
  [#1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7241796309) and become design records
  AUD-21 to AUD-28 (§6). The headline change: the slice needs **at least 20,000 concurrent, individually simulated
  skiers** (it was 5,000). The full game aims at about 100,000–200,000 per large region. Hardware, the p95 16.6 ms
  target, the 2027-06-30 date and the rest of the slice scope are unchanged.
- **Tracker.** 29 new milestone issues (#1841–#1869) replace v0.2's placeholders N1–N12 and add 17 justified splits
  and gates. 27 existing issues were amended. #1747 was rewritten under G-17. #1740 and #1749 had their
  evidence-backed boxes ticked.
- **Dependency graph.** Every edge is reciprocal (0 errors on live data) and has no cycles. Each edge was reviewed
  against acceptance (§4.2).
- **Premature picks are fixed before the 2026-12-02 KESTREL-first switch.**
  - #1741 now waits on the construction lifecycle (#1850), content definitions (#1843) and the piste network (#1845).
  - #1735 waits on the clock (#1842) and the RNG domains (#1841).
  - #1756 waits on lifts (#1743) and guests (#1745).
- **Startable KESTREL work** (rank, 2026-11-28): #1757 (stabilization priority), #1731, #1753, #1846, #1732, #1748,
  #1733, #1734. #1847 is also open but waits on an owner step. **#1731 alone gates the simulation track**; see §7.1.
- **North star** (branch): a new *Simulation foundation* line, the camera-independent guest line, the 20k target,
  ski school, visible non-destructive avalanches, the Steam Playtest app, and real issue numbers on every line.
  `lint` now reports no unfiled critical-path item.

---

## 1. Verified implementation status (unchanged since v0.2)

A re-check on 2026-11-28 found no change to code or issues since v0.2. No milestone issue was updated after
2026-11-27, and master is still `09f2aa738`. v0.2 §1 remains the detailed source-referenced status; the conclusions
that drive the backlog are:

| Area | Status (source/PR evidence) |
|---|---|
| Native module (#1731) | Absent from checkout, worktrees, branches and PRs. `Kestrel.oloproj` has no module field. |
| Construction | C# prototype from PR #1784: unconditional full-refund undo, a refund rule built into the engine `IBuildCommand` contract, redo charges the old quote, no dependency checks, balance not saved. No committed money/undo/redo test. |
| Time | Engine fixed step 60 Hz, at most 15 steps per frame, which slows the simulation and drops nothing. Tops out near 15× real time against about 8,760× for one simulated year per real hour. KESTREL needs its own tick (T-11). |
| RNG / replay | One engine stream (#452), no domains, state not saved. No digest, no command log. |
| Save | `kSaveGameFormatVersion` 54, exact-version only, no migration. |
| Guests / network | None. The navmesh and CrowdManager exist; no piste graph or named routes. |
| Crowd rendering | #1749 / PR #1788: 5,000 stand-ins added 2.876–2.885 ms GPU on the RX 5600 XT. **Historical**; not a 20k simulation result. |
| Content | Five hard-coded C# building literals; no schema, IDs or fingerprint. |
| Headless | OloServer `--smoke-test` only. |
| Steam / feedback | Dry-run publishing script and workflow; no feedback, telemetry or upload path. |

---

## 2. Owner decisions applied (AUD-21 to AUD-28)

| Record | Question | Effect on the backlog |
|---|---|---|
| AUD-21 | Q1, Q9 | 20k slice minimum and a 100k–200k ambition. Scale evidence required from #1743, #1745, #1842, #1845, #1848 onward. #1747 is integration, not first discovery. #1757 measures integrated work from the start. #1869 is the final 20k gate. |
| AUD-22 | Q10 | 100 % experimental default refund in #1850, on the amount actually paid. Test-run costs are not refunded. |
| AUD-23 | Q11 | #1741: lift crossings are allowed when a conservative 3D clearance check passes, with explicit reasons when it fails. |
| AUD-24 | Q12 | #1739 adds visible incidents, patrol response (#1857) and visuals (#1858). Lifts and buildings are never damaged. |
| AUD-25 | Q14 | #1859, a basic functional ski school after CP3. |
| AUD-26 | Q4, Q5, Q7 | Steam Playtest app (#1851), protected publishing (#1853), feedback (#1852, #1854), home-server assessment (#1847), CP3 external gate (#1868). |
| AUD-27 | Q13 | Basic versioned save (#1755) and migration with backup (#1862). |
| AUD-28 | Q8 | Synthetic mountains for CP1–CP3 labs; real terrain (#1732) as soon as available. |
| Q2, Q3, Q6, Q15 | process | Splits filed, foundation line added, efforts recalibrated, #1740/#1749 boxes ticked. |

---

## 3. Issue reconciliation (applied)

### 3.1 New issues

Every new issue is in milestone *KESTREL: vertical slice* with the `kestrel` label and a full `olo-score` block.
Gameplay issues carry AGENT_GUIDE design fields 1–7. Engine and operations issues carry a design-context line.
Acceptance criteria are in each issue body; the main points are in §3.3.

| N | Issue | Title | CP | blocked_by | blocks | cap/craft/stab/decay | effort | conf | fun |
|---|---|---|---|---|---|---|---|---|---|
| N2 | #1841 | KESTREL deterministic core: RNG domains and a canonical state digest | CP0 | #1731 | #1735, #1755, #1848, #1849 | 13/1/8/8 | 3 | 0.8 | 3 |
| N1 | #1842 | KESTREL simulation clock and calendar: pause, five speeds and intelligent advance | CP0 | #1731 | #1735, #1743, #1752, #1849, #1850 | 13/3/3/8 | 5 | 0.8 | 5 |
| N3 | #1843 | KESTREL content definitions: stable IDs, schema validation and a content fingerprint | CP0 | #1731 | #1741, #1743, #1752, #1754, #1845, #1848, #1849, #1850, #1852 | 8/1/3/8 | 5 | 0.8 | 2 |
| N5 | #1844 | KESTREL resort ledger: native double-entry accounts, cash versus profit, capex and loans | CP0 | #1731 | #1738, #1746, #1750, #1751, #1752, #1850, #1859 | 8/1/5/8 | 3 | 0.8 | 3 |
| N6 | #1845 | KESTREL piste network model: corridors, a directed navigation graph and named pistes | CP0-CP1 | #1731, #1843 | #1739, #1741, #1742, #1743, #1745, #1755 | 13/3/3/8 | 8 | 0.8 | 5 |
| N8 | #1846 | Repeatable KESTREL playtest package: versioned build, minimal shell and clean-install check | CP0 | — | #1759, #1851, #1852 | 8/1/3/8 | 3 | 0.8 | 3 |
| N17 | #1847 | KESTREL playtest feedback: assess and provision isolated private ingestion on the home server | CP3 external | — + external | #1868 | 5/1/5/8 | 3 | 0.5 | 2 |
| N7 | #1848 | KESTREL headless runner and scale harness: fixtures, seeds and a 1k-200k population ladder | CP0 | #1731, #1841, #1843 | #1745, #1849, #1865 | 8/1/5/8 | 3 | 0.8 | 3 |
| N13 | #1849 | KESTREL command traces and deterministic replay, with the foundation invariance suite | CP0 | #1842, #1841, #1843, #1848 | #1745, #1852, #1865 | 8/1/8/8 | 3 | 0.8 | 3 |
| N4 | #1850 | KESTREL construction lifecycle and transactions: red/yellow/green, refunds and dependency-safe undo | CP1 | #1842, #1843, #1844 | #1738, #1739, #1741, #1742, #1743, #1746, #1755, #1859 | 8/3/5/8 | 8 | 0.8 | 5 |
| N9 | #1851 | KESTREL Steam Playtest app: first live upload and owner-controlled promotion | CP3 external | #1846 + external | #1853, #1868 | 5/1/1/8 | 2 | 0.5 | 3 |
| N10 | #1852 | KESTREL feedback reporter and private ingestion service, tested against a local fixture | CP3 | #1843, #1846, #1849 | #1854, #1868 | 8/3/1/8 | 5 | 0.8 | 3 |
| N16 | #1853 | KESTREL protected Steam publishing: credential isolation and automated uploads from trusted builds | after CP3 | #1851 + external | — | 3/1/5/5 | 3 | 0.5 | 2 |
| N26 | #1854 | KESTREL opt-in gameplay telemetry: consented, schema-versioned events to private ingestion | CP3 optional | #1852 | — | 5/1/1/5 | 3 | 0.8 | 2 |
| N19 | #1855 | KESTREL trail grading and forest clearing on the full mountain: runtime terrain and forest transactions | CP5 | #1742, #1733, #1734 | #1862, #1869 | 5/5/3/5 | 5 | 0.5 | 5 |
| N20 | #1856 | KESTREL maintenance crews and lift repair | CP4 | #1750 | #1864 | 3/1/1/5 | 2 | 0.8 | 3 |
| N21 | #1857 | KESTREL ski patrol: crash response, opening sweeps and avalanche duties | CP4 | #1750, #1746 | #1739 | 5/3/1/5 | 3 | 0.8 | 5 |
| N22 | #1858 | KESTREL visible avalanche incidents: presentation driven by authoritative incident state | CP4 | #1739 | #1869 | 1/8/1/5 | 3 | 0.5 | 5 |
| N23 | #1859 | KESTREL basic ski school: bounded skill progression, capacity, cost and income | CP5 | #1745, #1746, #1850, #1844 | #1869 | 3/3/1/5 | 3 | 0.8 | 5 |
| N24 | #1860 | KESTREL reputation categories and the star rating summary | CP4 | #1751, #1746 | #1869 | 5/3/1/5 | 3 | 0.8 | 5 |
| N25 | #1861 | KESTREL guest flow overlay and lift queue panel: diagnosing congestion | CP3 | #1754, #1743, #1745 | #1867, #1869 | 5/5/1/8 | 3 | 0.8 | 5 |
| N27 | #1862 | KESTREL full-resort saves and playtest save migration | CP5 | #1755, #1737, #1745, #1855 | — | 5/1/8/5 | 5 | 0.8 | 2 |
| N28 | #1863 | KESTREL guests on screen: render proxies driven by the authoritative guest simulation | CP1 | #1745 | #1865, #1867, #1869 | 5/8/1/8 | 3 | 0.5 | 5 |
| N12 | #1864 | KESTREL lift disruptions: wind holds, breakdowns and repair | CP4 | #1743, #1735, #1856 | #1869 | 3/3/1/5 | 2 | 0.8 | 3 |
| N14 | #1865 | KESTREL CP1 First Descent: integrated playable checkpoint and lab | CP1 | #1741, #1742, #1743, #1745, #1753, #1754, #1848, #1849, #1863, #1744 | #1866 | 13/5/1/8 | 3 | 0.5 | 5 |
| N15 | #1866 | KESTREL CP2 First Business: integrated playable checkpoint and lab | CP2 | #1865, #1746, #1750, #1751 | #1867 | 8/5/1/8 | 3 | 0.5 | 5 |
| N11 | #1867 | KESTREL CP3 First Fun Prototype: congestion loop, technically playtest-ready | CP3 | #1866, #1861, #1863, #1744, #1755 | #1868 | 13/8/1/8 | 5 | 0.5 | 5 |
| N18 | #1868 | KESTREL CP3 first external Steam playtest: deployment and feedback readiness | CP3 external | #1867, #1851, #1852, #1847 + external | — | 8/3/1/8 | 2 | 0.5 | 5 |
| N29 | #1869 | KESTREL slice performance gate: busiest view with 20,000 active simulated skiers at p95 16.6 ms on the target class | CP6 | #1747, #1863, #1744, #1748, #1758, #1757, #1756, #1732, #1736, #1738, #1752, #1864, #1855, #1858, #1859, #1860, #1861 + external | — | 3/1/1/8 | 3 | 0.5 | 2 |

**Mapping from v0.2:**

- N1–N12 keep their v0.2 meaning.
- N2 was split so the clock and digest can start in parallel: N2 is the digest and RNG domains (#1841), N13 the
  traces and replay (#1849).
- N10 was split per addendum §3.10: N10 is feedback and ingestion (#1852), N26 telemetry (#1854).
- New issues:

| N | Issue | Purpose |
|---|---|---|
| N14 | #1865 | CP1 checkpoint |
| N15 | #1866 | CP2 checkpoint |
| N16 | #1853 | Protected publishing |
| N17 | #1847 | Home-server hosting assessment |
| N18 | #1868 | CP3 external readiness |
| N19 | #1855 | Grading and forest, from #1742 |
| N20 | #1856 | Maintenance and repair, from #1750 |
| N21 | #1857 | Patrol, from #1750 |
| N22 | #1858 | Avalanche visuals |
| N23 | #1859 | Ski school |
| N24 | #1860 | Reputation, from #1751 |
| N25 | #1861 | Flow and queue overlay, from #1754 |
| N27 | #1862 | Full saves and migration, from #1755 |
| N28 | #1863 | Render proxies for guests |
| N29 | #1869 | Final 20k performance gate |

### 3.2 Amended issues

Each amended issue has a dated section, *Amendment 2026-11-28 (EXP-17 / AUD-18 v0.3)*, that wins over the original
text it conflicts with. Its score block is re-stamped. Titles changed where scope moved: #1735, #1739, #1742, #1743,
#1747, #1750, #1751, #1754, #1755.

| Issue | Title (current) | blocked_by | blocks | effort | conf | decay |
|---|---|---|---|---|---|---|
| #1731 | KESTREL: native game module, a per-project C++ target for game simulation (ADR 0027 §4) | — | #1841, #1842, #1843, #1844, #1845, #1848 | 5 | 0.8 | 8 |
| #1732 | Real terrain import: Copernicus GLO-30 elevation into a full-scale 10×10 km terrain, with attribution | — | #1869 | 3 | 0.8 | 5 |
| #1733 | Full-scale terrain at strategy-camera distance: LOD and streaming for a 10×10 km mountain within budget | — | #1855 | 5 | 0.5 | 5 |
| #1734 | Distant forest: conifer forest across a 10 km mountain via impostors within budget | — | #1855 | 5 | 0.5 | 5 |
| #1735 | Climate and weather: a year's weather drawn from the location's climate, with correlated local fields | #1842, #1841 | #1736, #1737, #1864 | 3 | 0.8 | 5 |
| #1736 | Seasonal look: snow line, meadows and larch colour across the year on terrain and foliage | #1735, #1737 | #1869 | 3 | 0.5 | 5 |
| #1737 | Snowpack simulation: per-cell depth and quality from snowfall, melt, rain, wind and skier traffic | #1735, #1754 | #1736, #1738, #1739, #1862 | 8 | 0.8 | 5 |
| #1738 | Grooming and snowmaking: groomer passes and snow guns that need cold, dry air | #1737, #1750, #1850, #1844 | #1869 | 3 | 0.8 | 5 |
| #1739 | Avalanche risk, visible incidents and run opening: danger from the snowpack, patrol response, closures with reasons | #1737, #1850, #1845, #1857, #1754 | #1858 | 3 | 0.8 | 5 |
| #1741 | Lift building tool: a line from valley to top station with terrain-following towers | #1843, #1850, #1845 | #1744, #1865 | 5 | 0.8 | 8 |
| #1742 | Trail authoring tool: corridors, branches, named pistes and validation | #1850, #1845 | #1855, #1865 | 5 | 0.8 | 8 |
| #1743 | 1950s lifts simulated: capacity, throughput by skill mix, queues and operating cost | #1842, #1843, #1850, #1845 | #1744, #1745, #1750, #1756, #1861, #1864, #1865 | 5 | 0.8 | 8 |
| #1744 | 1950s lifts on screen: haul rope, moving chairs and T-bars, stations | #1743, #1741 | #1865, #1867, #1869 | 3 | 0.8 | 5 |
| #1745 | Guests: skiers with skills and wants choosing runs and lifts on the trail network | #1743, #1845, #1848, #1849 | #1746, #1747, #1751, #1756, #1859, #1861, #1862, #1863, #1865 | 8 | 0.5 | 8 |
| #1746 | Guest needs: crashes, eating and rest, and a satisfaction score | #1745, #1850, #1844 | #1747, #1857, #1859, #1860, #1866 | 3 | 0.8 | 5 |
| #1747 | Guest simulation at slice scale: 20,000 concurrent skiers within budget at every speed, camera-independent | #1745, #1746, #1757 | #1869 | 8 | 0.5 | 5 |
| #1748 | Skier character and skiing animation: carve, snowplough, fall, get up, ride a lift | — | #1869 | 3 | 0.8 | 5 |
| #1750 | Staff core: lift operators, wages and automated coverage | #1743, #1844 | #1738, #1856, #1857, #1866 | 2 | 0.8 | 5 |
| #1751 | Economy: pass prices, demand, running costs, loans and finance data | #1745, #1844 | #1860, #1866 | 3 | 0.8 | 5 |
| #1752 | Tech tree and eras: research with year-gated unlocks, 1950 to about 1965 | #1842, #1843, #1844 | #1869 | 3 | 0.8 | 5 |
| #1753 | Game UI foundation: HUD, info panels and notifications for a management game | — | #1754, #1759, #1865 | 3 | 0.8 | 8 |
| #1754 | Game UI: build menus, the terrain overlay layer, and slope and difficulty overlays | #1753, #1843 | #1737, #1739, #1861, #1865 | 3 | 0.8 | 8 |
| #1755 | Basic save and resume of a KESTREL resort: versioned authoritative state | #1841, #1850, #1845 | #1759, #1862, #1867 | 3 | 0.8 | 5 |
| #1756 | Resort sound: lifts, crowds, wind and weather | #1743, #1745 | #1869 | 3 | 0.8 | 5 |
| #1757 | Performance gate: a busiest-view benchmark of the KESTREL slice on GTX 1660 / RX 5600 class hardware | — | #1747, #1758, #1869 | 3 | 0.8 | 5 |
| #1758 | Graphics presets for the KESTREL target: a preset that holds 1080p60 on GTX 1660 class hardware | #1757 | #1869 | 3 | 0.8 | 5 |
| #1759 | Packaged KESTREL build with a main menu and settings | #1846, #1753, #1755 | — | 3 | 0.8 | 5 |

Closed issues:

- **#1740:** all 3 boxes ticked from PR #1784.
- **#1749:** all 4 boxes ticked from PR #1788.

Each carries a note that this is historical acceptance only: no later-decision compliance and no human validation.
#1730 is unchanged.

### 3.3 Significant acceptance changes

**Simulation and authority**
- **#1731:** the shared native command and state interfaces. C# submits commands and never writes authoritative
  state. Effort 3→5.
- **#1747:** rewritten. 20,000 concurrent skiers with authoritative state at every speed, within the budget agreed
  in #1757. Digest invariance across camera, presets, speed and headless. 50k–200k stress runs report results
  without pass or fail. No population ceiling and no silent reduction. The original camera-tier scope is kept in a
  collapsed block.
- **#1745:** a representative 20k headless load from the first working version. A probabilistic, safety-bounded
  route choice; return routes; imperfect information; day visitors only. Effort 5→8, confidence 0.8→0.5.
- **#1743:** core operation only (disruptions moved to #1864). A test-only staffing fixture until #1750. Early
  scale evidence. Effort 3→5.

**Construction**
- **#1741:** the prototype contract is replaced by #1850; assisted towers (C-03); AUD-23 crossings. Effort 3→5.
- **#1742:** the authoring tool only. It no longer waits for forest (#1734); grading went to #1855. Effort 3→5.

**Seasons, snow and avalanches**
- **#1735:** climate and weather only; the calendar went to #1842.
- **#1737:** EXP-06 benchmark instead of a mandated cell size; mass conservation; camera independence; snow
  overlays. Effort 5→8.
- **#1739:** AUD-24 visible incidents with patrol (#1857), no infrastructure damage, risk overlay.

**Staff, economy and UI**
- **#1750:** lift operators, wages and automated coverage only. No longer waits for grooming or guests. Effort 3→2.
- **#1751:** the ledger went to #1844 and the rating to #1860.
- **#1754:** build menus, the overlay layer, slope and difficulty overlays; the other overlays were redistributed.

**Saves, performance and packaging**
- **#1755:** a basic versioned save over registered state, rejecting an incompatible save without changing it. The
  2 s / 100 MB budgets and migration moved to #1862.
- **#1757:** integrated measurement from the start with at least 20k active skiers, a separate metrics list, and a
  rebaselined 3 ms hypothesis. It closes on harness, budget table and baseline; the final gate is #1869.
- **#1758:** presets never change the digest; measured on #1757's current scene.
- **#1756:** the voice budget at 20,000 guests (the original line was also edited).
- **#1759:** the final menu and settings; the early shell went to #1846.

---

## 4. Scores and dependencies

### 4.1 Score changes and rationale (issue-scoring.md)

| Change | Issues | Rationale |
|---|---|---|
| Effort up | #1731 3→5, #1741 3→5, #1742 3→5, #1743 3→5, #1737 5→8, #1745 5→8, #1747 5→8 | Approved recalibration (Q6) plus the scope added by the decisions: command interfaces, lifecycle validation, scale evidence, adaptive snow benchmark, 20k design. |
| Effort down | #1750 3→2 | Patrol, maintenance and groomers split out. |
| Confidence down | #1745 0.8→0.5, #1747 kept 0.5 | 20k individual simulation on a 6-core CPU is the slice's largest technical risk; spike-level. |
| Decay 5→8 | #1741, #1742, #1743, #1745, #1753, #1754 | They block CP1, the milestone's next demo (§1 Decay: "8 if it blocks the milestone's next demo"). All CP0/CP1 new issues are scored 8 for the same reason. |
| New-issue efforts | #1850 8, #1845 8, #1843 5, #1844 3 | Addendum §3.9: v0.2's N4 (5), N5 (2) and N3 (3) were optimistic once atomic terrain and forest rollback, double-entry with depreciation, and canonical fingerprints were added. |
| Fun | all new ≤ 5 | Fun ≥ 8 needs your `pull`; v0.2's 8 on the CP3 checkpoint was lowered to 5 (honest estimate, no `fun_by`). |

### 4.2 Semantic dependency review

Every edge was checked against the consuming issue's acceptance. The rules applied:

- **Start versus close.**
  - An edge is added when acceptance needs the capability to start or to verify.
  - Integration evidence that only a checkpoint can show belongs to the checkpoint issue (#1865, #1866, #1867), not
    to an edge.
  - Examples: lifecycle labels in the UI are verified in CP1, and guest coupling for snow traffic is verified at CP4.
- **The clock, digest and replay are split three ways.**
  - #1842 (clock) and #1841 (digest, RNG) both start after #1731.
  - The clock's own speed-invariance test compares full state snapshots.
  - The canonical-digest suite is in #1849, which needs both of them and the runner (#1848).
- **Construction and replay (#1850 and #1849).** Construction uses #1731's command interface and needs no trace to
  start or close. Replay of construction traces is CP1 evidence (#1865).
- **Save registration.**
  - Systems register state through the module's save interface (#1731, OloHeaderTool reflection).
  - #1755 adds versioning and compatibility rules and depends on the foundation state it demonstrates (#1841, #1850,
    #1845).
  - Lifts and guests register in their own issues; the integrated save is verified in CP3.
  - This removes the hidden cycle "N1 saves through #1755 / #1755 saves N1".
- **Staffing (addendum §3.2).**
  - #1750 (operators, wages) needs only lifts (#1743) and the ledger (#1844).
  - Repair (#1856) is the dependency of lift disruptions (#1864), not all staff.
  - Patrol (#1857) feeds #1739.
- **Piste authoring (addendum §3.3).** #1742 and #1845 run on synthetic terrain without #1734. Real-terrain grading
  and clearing (#1855) depends on #1733 and #1734.
- **Overlays.**
  - The engine overlay layer stays in #1754, which is needed at CP1.
  - Snow and avalanche overlays moved to #1737 and #1739, so CP1 does not wait for CP4 systems.
- **Missing edges added during review:**
  - #1744 → #1741 (rope geometry).
  - #1743 → #1845 (station nodes).
  - #1849 → #1848 (headless replay).
  - #1852 → #1849 (trace attachments).
  - #1759 → #1755 (load in the final menu).
- **Final performance gate.** #1757's acceptance closes on harness and baseline. The 20k busiest-view gate had no
  owner, so #1869 was filed for it.

### 4.3 Final graph (live, by depth; blockers inside the milestone)

- **Depth 0:** #1731; #1732; #1733; #1734; #1748; #1753; #1757; #1846 (N8); #1847 (N17)
- **Depth 1:** #1758 ← #1757; #1841 (N2) ← #1731; #1842 (N1) ← #1731; #1843 (N3) ← #1731; #1844 (N5) ← #1731; #1851 (N9) ← #1846
- **Depth 2:** #1735 ← #1842, #1841; #1752 ← #1842, #1843, #1844; #1754 ← #1753, #1843; #1845 (N6) ← #1731, #1843; #1848 (N7) ← #1731, #1841, #1843; #1850 (N4) ← #1842, #1843, #1844; #1853 (N16) ← #1851
- **Depth 3:** #1737 ← #1735, #1754; #1741 ← #1843, #1850, #1845; #1742 ← #1850, #1845; #1743 ← #1842, #1843, #1850, #1845; #1755 ← #1841, #1850, #1845; #1849 (N13) ← #1842, #1841, #1843, #1848
- **Depth 4:** #1736 ← #1735, #1737; #1744 ← #1743, #1741; #1745 ← #1743, #1845, #1848, #1849; #1750 ← #1743, #1844; #1759 ← #1846, #1753, #1755; #1852 (N10) ← #1843, #1846, #1849; #1855 (N19) ← #1742, #1733, #1734
- **Depth 5:** #1738 ← #1737, #1750, #1850, #1844; #1746 ← #1745, #1850, #1844; #1751 ← #1745, #1844; #1756 ← #1743, #1745; #1854 (N26) ← #1852; #1856 (N20) ← #1750; #1861 (N25) ← #1754, #1743, #1745; #1862 (N27) ← #1755, #1737, #1745, #1855; #1863 (N28) ← #1745
- **Depth 6:** #1747 ← #1745, #1746, #1757; #1857 (N21) ← #1750, #1746; #1859 (N23) ← #1745, #1746, #1850, #1844; #1860 (N24) ← #1751, #1746; #1864 (N12) ← #1743, #1735, #1856; #1865 (N14) ← #1741, #1742, #1743, #1745, #1753, #1754, #1848, #1849, #1863, #1744
- **Depth 7:** #1739 ← #1737, #1850, #1845, #1857, #1754; #1866 (N15) ← #1865, #1746, #1750, #1751
- **Depth 8:** #1858 (N22) ← #1739; #1867 (N11) ← #1866, #1861, #1863, #1744, #1755
- **Depth 9:** #1868 (N18) ← #1867, #1851, #1852, #1847; #1869 (N29) ← #1747, #1863, #1744, #1748, #1758, #1757, #1756, #1732, #1736, #1738, #1752, #1864, #1855, #1858, #1859, #1860, #1861

**Checks on live data (2026-11-28):**
- 56 open milestone issues, 0 cycles, 0 reciprocity errors.
- Every open milestone issue has the `kestrel` label and passes master's `parse_block` (Decay ≥ 5, `areas` set).
- Every edge points at an open milestone issue.
- External blockers are on #1847, #1851, #1853 and #1868.

---

## 5. Integrated playable checkpoints

Checkpoints set no dates. Each checkpoint carries scale evidence: a representative lab load plus a 20,000-guest run on
a scalable synthetic network, and above that, achieved-rate stress reports.

| CP | Player loop | Gate issue | Core issues | Technical evidence | Human question |
|---|---|---|---|---|---|
| **CP0 Foundation** | none | (none) | #1731, #1842, #1841, #1843, #1844, #1845, #1848, #1849; #1846 and #1757 in parallel | module refuses invalid commands; digest and replay invariance suite; ledger invariants; 20k synthetic overhead numbers; package installs clean | none |
| **CP1 First Descent** | build a surface lift and a piste, test (yellow), open (green); guests arrive as recognisable skiers, ride visibly operating lifts, descend, return; pause and speeds | #1865 | #1850, #1741, #1742, #1743, #1745, #1753, #1754, #1863 (functional stand-in), #1744 | first-descent lab + 20k run; traces replay; digest coverage of ledger, network and lifecycle; EXP-01 pacing numbers and proposed minimum useful rates | Owner-local: is watching guests use what I built satisfying, and is the pace right? |
| **CP2 First Business** | pass prices, demand with no-visit option, costs and wages, one investment decision | #1866 | #1751, #1746, #1750 | season reconciles; price-to-demand test | Owner-local: do I understand why I made or lost money? |
| **CP3 First Fun Prototype** | a queue forms; diagnose with the panel and overlay; fix with a second lift or branch; closures reroute; save and resume | #1867 (technical), #1868 (external) | #1861, #1755 (with save migration) | congestion lab + 20k run; interactive p50/p95/p99 with simulation active in #1757 | **First external Steam test:** do players understand why the queue formed, and does fixing it feel good? |
| **CP4 Difficult Winter** | weather, snowpack, grooming, snowmaking, holds, breakdowns, patrol, visible avalanche incidents | (none) | #1735, #1737, #1738, #1739, #1858, #1864, #1856, #1857, #1860 | snow mass conservation, camera independence, climate statistics | Are setbacks fair and manageable? |
| **CP5 Whole Year, real mountain** | 1950 start through a year on the imported mountain; research to about 1965; ski school; full save | (none) | #1732, #1733, #1734, #1855, #1736, #1752, #1859, #1862 | full-resort save/replay; terrain and forest budgets | Does a year hold interest; does progression feel earned? |
| **CP6 Vertical slice** | the north-star slice | #1869 | #1747, #1748, #1756, #1758, #1759, #1853, and every CP4/CP5 system through #1869's edges | p95 ≤ 16.6 ms busiest view with ≥ 20k active skiers and every slice system doing measured work, on the target class; owner-approved EXP-01 throughput rates met; presets do not change digests | Owner review against the north star |

**Readiness at CP3.**
- #1867 is *technically playtest-ready*: the owner can test it locally, with no Steam account or hosting needed.
- #1868 is *external deployment readiness*. It needs a Steam Playtest upload (#1851), working feedback submission
  (#1852), approved private collection (#1847), and the owner's privacy notice and promotion.
- Telemetry (#1854) is desirable but does not block.

---

## 6. Documentation changes (branch, not committed)

| File | Change |
|---|---|
| `docs/process/north-star.md` | Status line cites the 2026-11-28 revision. Slice and hardware bullets add the 20k target and the 100k–200k ambition. New *Simulation foundation* line (#1731, #1842, #1841, #1849, #1843, #1844, #1848). Camera-independent guest line with ski school (#1745, #1746, #1747, #1859). Visible non-destructive avalanches (#1858). Real issue numbers on every line, including Steam (#1851, #1853, #1852, #1854, #1847, #1868). Checkpoint paragraph (#1865–#1867). Date and hardware unchanged. The historical 5,000 stand-in benchmark on the #1749 line is kept. |
| `docs/adr/0027-…md` | §4: the target now reads 20 000, noting that it was 5 000 at acceptance. The "revisit B" measurement uses 20 000 agents. |
| `KestrelProject/docs/gameplay/guests-and-networks.md` | AUD-21 (scale), AUD-25 (ski school); contract sentence. |
| `…/gameplay/construction.md` | AUD-22 (refund default), AUD-23 (crossings); contract sentence. |
| `…/gameplay/snow-and-weather.md` | AUD-24 (visible incidents); contract sentence. |
| `…/validation/steam-and-feedback.md` | AUD-26 (Playtest app, credentials, hosting, CP3 gate); contract sentence. |
| `…/gameplay/time-and-history.md` | AUD-27 (tester saves); contract sentence. |
| `…/gameplay/geography.md` | AUD-28 (synthetic mountains); contract sentence. |
| `…/validation/production-checkpoints.md` | Checkpoint plan paragraph with CP issue links. |
| `…/DECISIONS.md` | AUD-21 to AUD-28 rows; G-13, W-10 and V-21 dispositions note their refinements; a provenance line. |
| `…/OPEN_QUESTIONS.md` | EXP-03 cites AUD-22; EXP-15 cites AUD-26. No question was closed. |

**Search for 5,000 references.** These were checked: docs, ADRs, the KESTREL design docs, AGENTS.md, CLAUDE.md,
`.claude/`, every open issue body and every milestone issue's comments.

| Remaining reference | Why it stays |
|---|---|
| #1749 benchmark (north star, PR #1788) | Historical evidence. |
| #1747's collapsed original scope | History. |
| ADR 0012 "5,000 lines" | Unrelated. |

The archive is unchanged (`sha256sum -c SHA256SUMS`: OK). `check_docs_consistency.py` passes.

---

## 7. Development sequence

1. **Now to 2026-12-01 (stabilization):** #1757 stays the priority. The tracker is already safe for the switch.
2. **From 2026-12-02, CP0:**
   - #1731 first among KESTREL work (see §7.1 below), then #1842, #1841, #1843, #1844 and #1845 in parallel, then
     #1848 and #1849.
   - In parallel: #1846 (package), #1753 (HUD), #1732 (licence and import), #1733 and #1734 (measurements), #1748
     (assets), #1757 baselines.
   - Owner, in parallel: Steam onboarding, and the hosting inventory for #1847.
3. **CP1:** #1850, #1741, #1742, #1743, #1745 (with 20k labs from day one), #1754, #1863 (stand-in skiers), #1744, then #1865. Owner-local test.
4. **CP2:** #1750, #1751, #1746, then #1866. Owner-local test.
5. **CP3:** #1861, #1755 (with save migration), then #1867. With #1851, #1852 and #1847 done: #1868, the first external Steam
   test.
6. **CP4 and CP5:** the climate and snow chain (#1735 → #1737 → #1738 / #1739) can start as soon as #1842 and #1841
   land, ahead of CP3. Real terrain enters gameplay as soon as #1732 lands.
7. **CP6:** #1747, #1869, presets, sound, final menu, protected publishing.

### 7.1 Risk: #1731's rank

After the switch, #1731 ranks fifth among startable KESTREL issues (score 4.35, below #1753, #1846, #1732 and #1748),
because effort 5 divides its value. It alone gates 6 foundation issues. I did not inflate its axes; the scoring doc
says to fix inputs honestly. To make it the first KESTREL pick, you can name it a `priority:` in
`development-policy.md`. The goal and large-item slots may also pick it. That is your call (§9).

The critical chain is long: #1731 → #1850 → #1743 → #1745 → #1865 → #1866 → #1867. Any slip in #1731 or #1745 moves
every checkpoint.

---

## 8. Verification results

| Check | Result |
|---|---|
| `issue_scores.py lint` (branch north star) | Exit 1, KESTREL-clean. No unfiled critical-path item. 5 unrelated non-KESTREL findings at the post-review rerun: merged-since-score on #1353 (PR #1817), #1625 (PR #1824) and #1589 (PR #1828); #1617 score 46 days old; #1823 stale edge to closed #1807. Left untouched as out of scope. |
| `lint` (master's copy and north star) | Also reports the unfiled Steam line, because master's north star is unchanged until the branch merges. |
| `issue_scores.py rank` | Exit 0. Milestone: 56 open scored, 0 critical-path items unfiled. `next:` / `goal:` #1757. KESTREL startable: #1753, #1846, #1732, #1748, #1731, #1757, #1733, #1734 (#1847 waits on an owner step). |
| Master's `rank` against live bodies | Exit 0. New bodies parse under master's parser. |
| `issue_scores.py pulse` | 64 task PRs in 28 d. Milestone 3/59 closed. Critical path 1/21 ticked. |
| Reciprocity, cycles, labels, milestone (live) | 0 errors. |
| `check_docs_consistency.py` | Pass. |
| Archive hashes | OK. |

Before and after the switch:

| | Startable KESTREL issues |
|---|---|
| Before | #1731, #1757, #1732, #1733, #1734, #1735, #1741, #1748, #1753, #1756 |
| After | #1731, #1757, #1732, #1733, #1734, #1748, #1753, #1846 |

The three premature picks left the set and the playtest package joined it.

---

## 9. Owner actions and external blockers

1. **Commit and PR** the documentation branch (needs your go-ahead). Until it merges, master's `lint` still reports
   the old Steam line.
2. **Steam:**
   - Finish partner onboarding: bank and tax details, kept out of every repo, issue and log.
   - Create or confirm the Steam Playtest app and its IDs, and a narrowly privileged build account (#1851).
   - Later, approve the protected publishing environment (#1853).
3. **Feedback host:** an inventory session and authorisation before anyone touches the home server (#1847). Approve
   the collection categories, privacy notice, retention and access before any external data is collected (#1847,
   #1868).
4. **Optional:** make #1731 the KESTREL-first `priority:` (§7.1). Confirm Fun ≥ 8 with `pull` on any issue you are
   keen on (e.g. #1867).
5. **Human validation** owed at CP1 and CP2 (owner-local) and CP3 (external testers). Agents set `playtest-ready` at
   most.

## 10. Deviations, open conflicts and failed operations

**Deviations from the addendum**

- **More issues than v0.2.** 29 new issues instead of 12, using the authorised splits:
  - N2 split into a digest issue and a replay issue.
  - CP1 and CP2 got gate issues of their own.
  - #1869 is the final performance gate.
  - #1863 owns the render binding for guests, which no issue owned before.
  - Overlays were redistributed.
- **Lab fixtures.** These belong to the checkpoint issues rather than to separate lab issues, so the runner (#1848)
  stays small and early (addendum §3.5).
- **Interim edges.** Interim safety edges were set first on #1741, #1735 and #1756, then replaced by the final ones.
  No transient window existed where premature work was startable.

**Conflicts**

No new design conflict was found that needs escalation. One judgement call for you: whether #1731 should be a
`priority:` (§7.1).

**Failed operations**

None in the tracker work. One local script failed on a shell heredoc length limit before running and was redone with
file edits. No partial writes and no duplicate issues: creation was idempotent by title and state file.

**Not done, by scope**

- No gameplay code.
- No builds.
- No Steam upload or account change.
- No home-server access.
- No data collection.
- No commit, push or PR.

---

## 11. Corrections after the PR #1873 review (2026-11-28)

An independent review of PR #1873 and the 59 milestone issues raised seven findings. Each was verified against the
live issues and the checkout; all seven were valid and are fixed. No finding was rejected.

| # | Finding | Fix (live) |
|---|---|---|
| 1 | Save migration came after the first external test (#1862, CP5). | #1755 now owns the versioned migration framework and a representative migration test, before #1868. #1862 keeps full-resort blobs, terrain and forest deltas and large-save performance. Exceptional breaking changes stay allowed with communication and backups. |
| 2 | #1869 did not depend on the full slice workload. | #1869 now waits on real terrain (#1732), seasonal look (#1736), grooming and snowmaking (#1738), avalanche incidents (#1858), terrain and forest edits (#1855), ski school (#1859), disruptions (#1864), economy and reputation (#1860), overlays (#1861), research (#1752), the production character (#1748), presets and the harness (#1758, #1757). Per-system activity counters must show each system doing work. #1757 stays unblocked. |
| 3 | Remaining semantic dependencies. | Hard edges added where acceptance needs the capability: #1848 ← #1843 (fingerprint), #1845 ← #1843 (rating data), #1752 ← #1842 (calendar year), #1862 ← #1855 (real deltas), #1747 ← #1757 (CPU allocation). Interfaces instead of edges: #1844, #1845 and #1850 register state with the module's registry and their digest coverage is verified at CP1 (#1865); #1850 exposes lifecycle state through a query that #1845's availability interface consumes, with end-to-end exclusion verified at CP1. No #1845/#1850 edge, so no cycle. #1758's preset digest check moved to #1869's acceptance. |
| 4 | Throughput could pass by reporting. | #1869 requires the owner-approved EXP-01 minimum useful simulation rates as well as the 16.6 ms budget, as separate requirements, and carries an external blocker until the rates are approved. #1865 proposes the rates. #1747 states that reporting is not passing. No values were invented. |
| 5 | CP1 had no recognisable skiers or lifts. | #1863 moved to CP1 with a minimal functional stand-in and no longer waits for #1748; #1748 owns the production character and replaces the stand-in. #1865 now depends on #1863 and #1744. |
| 6 | Docs said the audit was outstanding; VISION lacked the scale; the audit was not in the repo. | README status updated, VISION carries AUD-21's scale, EXP-17 notes the completed initial reconciliation, and this audit is committed as `validation/backlog-audit-v0.3.md`. The archive is unchanged. |
| 7 | #1853's isolation was underspecified. | #1853 requires a separately trusted execution environment, never a persistent runner that ran untrusted PR code, plus verified artifact provenance (attestation or signed checksum). The mechanism stays an owner-approved infrastructure decision. |
