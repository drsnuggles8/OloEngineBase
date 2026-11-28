# North star

The goal the backlog is steered toward. Only the user sets it. `/start-work` and
`scripts/issue_scores.py` read it (issue-scoring.md §3.9), and an agent never sets or changes the
milestone or the goal text below on its own.

**Status: set** by the user on 2026-11-21
([decisions on #1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7060665000)), and revised by the user on 2026-11-28
([decisions on #1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7241796309): the 20,000-skier slice target, the
Simulation foundation line, camera-independent guests, a basic ski school, visible non-destructive avalanche incidents and a
separate Steam Playtest app).
Emptying `milestone:` turns the steering off: the picker then ranks as if this file did not exist.

```olo-north-star
milestone: "KESTREL: vertical slice"   # the GitHub milestone holding the goal's critical path
```

## What setting it does

- Issues in the milestone get the `goal` flag in `rank`. `rank` names the top startable one on its
  `goal:` line.
- `lint` fails on a milestone issue whose Decay is below 5 (the scoring doc's "near milestone"
  rung) and on a critical-path item below that has no issue yet.
- A `/start-work` batch of two or more gives one slot to the `goal:` candidate.
- `issue_scores.py pulse` reports how many of the milestone's issues are closed and how many
  critical-path items are ticked.

## Why it exists

Bug discovery feeds whichever area was worked last. A renderer sweep files renderer issues, the next
sweep picks from them, and the backlog grows in that area. On 2026-11-20 most of the 69 open issues
were renderer work, while the stated aim is a game in 2027. Nothing in the score knew that. This file
is the counterweight: it states what "forward" means, so the picker can tell critical-path work from
engine work that is merely good.

## The goal

- **Game:** KESTREL (codename; no public title yet), a photoreal ski resort management game on real
  mountains. You design and run each resort through full seasons, unlock lifts and technology as
  history made them available, from 1950 to today (a historical tech tree, in the style of Two Point
  Museum and Hollywood Animals; whether eras lock stays open,
  [P-11](../../OloEditor/KestrelProject/docs/gameplay/progression-and-research.md#p-11) and
  [EXP-20](../../OloEditor/KestrelProject/docs/OPEN_QUESTIONS.md)), and build a group of resorts, moving
  to new regions of the world (in the style of Planet Zoo and RollerCoaster Tycoon). Sandbox first, plus a scenario campaign. Riding your own runs is a
  nice extra, not core. Full seasons, summer included. It lives in this repo as
  `OloEditor/KestrelProject` ([ADR 0027](../adr/0027-kestrel-lives-in-the-engine-repo-as-a-second-project.md)).
- **Milestone:** *KESTREL: vertical slice*, internal, mid-2027 (GitHub milestone due 2027-06-30). One
  real mountain in an unnamed Austrian or Swiss valley, a 1950 start, one full year of seasons, the
  first slice of the tech tree (about 1950–1965), sandbox only, with at least 20,000 concurrent, individually simulated
  skiers on a busy day ([AUD-21](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#aud-21); the target was
  5,000 before 2026-11-28). The full game's engineering ambition is about 100,000–200,000 concurrent guests in a large
  region: a scalability target, not a measured capability.
- **Target hardware and frame budget:** a GTX 1660 / RX 5600 class PC (6 GB VRAM, 16 GB RAM, 6-core
  CPU) at 1080p, 60 fps on the slice's busiest view with at least 20,000 simulated skiers active: a 95th-percentile frame time of 16.6 ms or less. Photoreal, with graphics presets, and
  heavy optimisation so it looks good on older PCs.
- **Out of scope for the milestone:** Drift / Adrift only: no further Drift islands, keeper, crane or
  other Drift gameplay work. The renderer research items (#654, #1046, #1353, #805) and #437 stay where
  the rank puts them.

Not on the critical path: riding your runs, the multi-resort empire, the scenario campaign, eras after
1965, summer activities.

Integration checkpoints, in the milestone but not capability lines: CP1 First Descent (#1865), CP2 First Business
(#1866) and CP3 First Fun Prototype (#1867), which gates the first external Steam playtest (#1868). CP1 and CP2 are
tested only by the owner, locally.

## Critical path

One line per capability the milestone cannot ship without, confirmed by the user on #1727 (the Steam line on #1819, from AUD-16; the Simulation foundation line on 2026-11-28); the
first line (the project scaffold) follows from the user's layout decision. A confirmed line goes in as
`- [ ] …` at once, so `lint` reports it until it gains its `(#N)` when its issues are filed, and it is
ticked when all of them close. Draft lines nobody has confirmed stay out until the user agrees.

- [ ] Project scaffold: `OloEditor/KestrelProject` with its own `.oloproj`, asset directory and script assembly, buildable and packageable next to SandboxProject, with general project selection (#1730)
- [ ] Simulation foundation (confirmed by the user on 2026-11-28): a native game module whose shared command and state interfaces C# only submits commands to; one simulation clock and calendar with pause, five speeds and intelligent advance; RNG domains and a canonical state digest; command traces and deterministic replay; versioned content definitions with a fingerprint; a double-entry resort ledger; a headless runner that measures a 1k–20k population ladder from the first guest simulation, plus 50k–200k stress runs (#1731, #1842, #1841, #1849, #1843, #1844, #1848)
- [ ] Real terrain import: Copernicus GLO-30 elevation data turned into a full-scale terrain for one real mountain (about 10×10 km), with the attribution line (#1732)
- [ ] The mountain at strategy-camera distance within budget: terrain level of detail, distant forest, streaming (#1733, #1734)
- [ ] Seasons and climate: a full year, weather drawn from the location's climate (#1735, #1736)
- [ ] Snowpack simulation: depth and quality from snowfall, melt, rain, wind and skier traffic; grooming; snowmaking that depends on cold, dry air; avalanche risk with occasional visible avalanche incidents, patrol response and consequences, never damaging lifts or buildings ([AUD-24](../../OloEditor/KestrelProject/docs/gameplay/snow-and-weather.md#aud-24)); opening and closing runs (#1737, #1738, #1739, #1858)
- [ ] Strategy camera and build tools: lifts with terrain-following towers, a piste network of corridors, junctions and named pistes, trails cut through forest and graded, buildings, cost previews, a red/yellow/green lifecycle with transaction-safe undo (#1740, #1850, #1845, #1741, #1742, #1855)
- [ ] Lifts from the 1950s: rope tow, T-bar, single and double chair, with capacity, queues, wind holds and breakdowns (#1743, #1744, #1864)
- [ ] Guests: at least 20,000 concurrent, individually simulated skiers with skills and wants choosing runs on the piste network, riding lifts, crashing, eating and learning at a basic ski school, with a satisfaction score; one authoritative simulation whose outcomes never depend on the camera, presets, speed or headless execution, with level of detail used only for rendering ([G-17](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#g-17), [AUD-21](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#aud-21), [AUD-25](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#aud-25)) (#1745, #1746, #1747, #1859)
- [ ] Skiers on screen
  - [ ] Skier model and skiing animation, driven by the simulated guests through render proxies (#1748, #1863)
  - [x] Crowd rendering with animated level of detail (#1749). At native 1080p, 5,000 stand-ins added 2.876–2.885 ms on the RX 5600 XT. [Benchmark](https://github.com/drsnuggles8/OloEngineBase/actions/runs/45456157390)
- [ ] Staff: lift operators, ski patrol, maintenance crews, groomers, simple for the slice (#1750, #1856, #1857)
- [ ] Economy and reputation: ticket prices, running costs, loans, a finance screen, a star rating that summarises the resort's category reputations and shows what drives them, never an input to guests' resort choice ([G-11](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#g-11), [G-03](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#g-03)) (#1751, #1860)
- [ ] Tech tree and eras: research with unlocks gated by year, 1950 to about 1965, as a rule for the slice only: the full game's era locking stays open ([P-11](../../OloEditor/KestrelProject/docs/gameplay/progression-and-research.md#p-11)) (#1752)
- [ ] Game UI: HUD, build menus, info panels, map overlays, notifications (#1753, #1754, #1861)
- [ ] Save and load the whole resort mid-season, with tester saves kept across ordinary updates ([AUD-27](../../OloEditor/KestrelProject/docs/gameplay/time-and-history.md#aud-27)) (#1755, #1862)
- [ ] Sound: lifts, crowds, wind, weather (#1756)
- [ ] Performance gate: 1080p60 at the 95th percentile on a GTX 1660 in the busiest view with at least 20,000 simulated skiers active, with graphics presets, measured from the start (#1757, #1758, #1869)
- [ ] A packaged build with a main menu and settings (#1846, #1759)
- [ ] Steam test distribution, started early rather than after the slice: packaged builds uploaded automatically to a private Steam branch the owner promotes, with lightweight private feedback ([AUD-16](../../OloEditor/KestrelProject/docs/validation/steam-and-feedback.md#aud-16)). A separate Steam Playtest app, credentials unreadable by ordinary PR execution, and the first external test at the CP3 First Fun Prototype ([AUD-26](../../OloEditor/KestrelProject/docs/validation/steam-and-feedback.md#aud-26)). Partner onboarding, the app ID, credentials and approval of the feedback host are the owner's steps (#1851, #1853, #1852, #1854, #1847, #1868)
