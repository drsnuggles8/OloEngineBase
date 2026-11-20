# North star

The goal the backlog is steered toward. Only the user sets it. `/start-work` and
`scripts/issue_scores.py` read it (issue-scoring.md §3.9), and an agent never sets or changes the
milestone or the goal text below on its own.

**Status: not set.** Until `milestone:` names a GitHub milestone, the picker ranks as if this file
did not exist, and `rank` prints `north star: NOT SET` on every run.

```olo-north-star
milestone: ""   # the GitHub milestone holding the goal's critical path, e.g. "Game: vertical slice"
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

## The goal (the user fills this in)

- **Game:** the game, in two sentences: genre, core loop, the moment that sells it.
- **Milestone:** the first shippable proof (vertical slice, demo, Early Access), and its target date.
- **Target hardware and frame budget:** the slowest machine it must run on and the frame time it
  must hold there. Performance issues are judged against this budget, not the RTX 4090 dev box.
- **Out of scope for the milestone:** engine work that waits until after the milestone.

## Critical path

One line per capability the milestone cannot ship without. A line becomes `- [ ] … (#N)` once the
issue is filed, and is ticked when the issue closes. Draft lines nobody has confirmed stay out until
the user agrees.

<!-- example, delete when filling in:
- [ ] Save and load mid-session, including physics and dialogue state (#N)
- [ ] Holds the frame budget on the target hardware in the slice's busiest scene (#N)
-->
