# North star

The goal the backlog is steered toward. Only the user sets it. `/start-work` and
`scripts/issue_scores.py` read it (issue-scoring.md §3.9), and an agent never sets or changes the
milestone or the goal text below on its own.

**Status: set** by the user on 2026-11-21
([decisions on #1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7060665000)).
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
  first slice of the tech tree (about 1950–1965), sandbox only.
- **Target hardware and frame budget:** a GTX 1660 / RX 5600 class PC (6 GB VRAM, 16 GB RAM, 6-core
  CPU) at 1080p, 60 fps on the slice's busiest view: a 95th-percentile frame time of 16.6 ms or less. Photoreal, with graphics presets, and
  heavy optimisation so it looks good on older PCs.
- **Out of scope for the milestone:** Drift / Adrift only: no further Drift islands, keeper, crane or
  other Drift gameplay work. The renderer research items (#654, #1046, #1353, #805) and #437 stay where
  the rank puts them.

Not on the critical path: riding your runs, the multi-resort empire, the scenario campaign, eras after
1965, summer activities.

## Critical path

One line per capability the milestone cannot ship without, confirmed by the user on #1727 (the Steam line on #1819, from AUD-16); the
first line (the project scaffold) follows from the user's layout decision. A confirmed line goes in as
`- [ ] …` at once, so `lint` reports it until it gains its `(#N)` when its issues are filed, and it is
ticked when all of them close. Draft lines nobody has confirmed stay out until the user agrees.

- [ ] Project scaffold: `OloEditor/KestrelProject` with its own `.oloproj`, asset directory and script assembly, buildable and packageable next to SandboxProject, with general project selection (#1730)
- [ ] Real terrain import: Copernicus GLO-30 elevation data turned into a full-scale terrain for one real mountain (about 10×10 km), with the attribution line (#1732)
- [ ] The mountain at strategy-camera distance within budget: terrain level of detail, distant forest, streaming (#1733, #1734)
- [ ] Seasons and climate: a full year, weather drawn from the location's climate (#1735, #1736)
- [ ] Snowpack simulation: depth and quality from snowfall, melt, rain, wind and skier traffic; grooming; snowmaking that depends on cold, dry air; avalanche risk; opening and closing runs (#1737, #1738, #1739)
- [ ] Strategy camera and build tools: lifts with terrain-following towers, trails cut through forest and graded, buildings, cost previews, undo (#1740, #1741, #1742)
- [ ] Lifts from the 1950s: rope tow, T-bar, single and double chair, with capacity, queues, wind holds and breakdowns (#1743, #1744)
- [ ] Guests: thousands of skiers with skills and wants choosing runs on the trail network, riding lifts, crashing, eating, with a satisfaction score; detailed simulation near the camera, cheap simulation far away (#1745, #1746, #1747)
- [ ] Skiers on screen
  - [ ] Skier model and skiing animation (#1748)
  - [x] Crowd rendering with animated level of detail (#1749). At native 1080p, 5,000 stand-ins added 2.876–2.885 ms on the RX 5600 XT. [Benchmark](https://github.com/drsnuggles8/OloEngineBase/actions/runs/45456157390)
- [ ] Staff: lift operators, ski patrol, groomers, simple for the slice (#1750)
- [ ] Economy and reputation: ticket prices, running costs, loans, a finance screen, a star rating that summarises the resort's category reputations and shows what drives them, never an input to guests' resort choice ([G-11](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#g-11), [G-03](../../OloEditor/KestrelProject/docs/gameplay/guests-and-networks.md#g-03)) (#1751)
- [ ] Tech tree and eras: research with unlocks gated by year, 1950 to about 1965, as a rule for the slice only: the full game's era locking stays open ([P-11](../../OloEditor/KestrelProject/docs/gameplay/progression-and-research.md#p-11)) (#1752)
- [ ] Game UI: HUD, build menus, info panels, map overlays, notifications (#1753, #1754)
- [ ] Save and load the whole resort mid-season (#1755)
- [ ] Sound: lifts, crowds, wind, weather (#1756)
- [ ] Performance gate: 1080p60 at the 95th percentile on a GTX 1660 in the busiest view, with graphics presets (#1757, #1758)
- [ ] A packaged build with a main menu and settings (#1759)
- [ ] Steam test distribution, started early rather than after the slice: packaged builds uploaded automatically to a private Steam branch the owner promotes, with lightweight private feedback ([AUD-16](../../OloEditor/KestrelProject/docs/validation/steam-and-feedback.md#aud-16)). The app ID, Steamworks access and credentials are the owner's steps
