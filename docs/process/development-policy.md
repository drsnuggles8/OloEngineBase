# Development policy for OloEngine and KESTREL

Use the current phase when selecting new tasks. The [north star](north-star.md) sets the
KESTREL milestone, scope, hardware and deadline. Proposed defaults for owner review on
2026-11-25 follow. The picker reads this block; extending or shortening stabilization
requires an owner-approved change to the date.

```olo-development-policy
{
  "game_led_from": "2026-12-02",
  "stabilization_priority": [1757],
  "optional_rendering": [654, 805, 1046, 1049, 1143, 1152, 1153, 1154, 1155, 1222, 1223, 1224, 1225, 1353, 1525, 1537, 1588, 1589, 1617, 1625, 1651, 1660, 1669, 1798]
}
```

## Stabilization before the transition date

Pick reproducible production defects first: crashes, device faults, corruption, synchronization,
resource lifetime and local/world or ownership contract failures. Use the remaining capacity for
controlled performance measurements, lifecycle coverage, build work and removal of duplicated
production implementations. Start the KESTREL busiest-view benchmark (#1757) early if eligible.
The `stabilization_priority` list names exceptions for that window, and `optional_rendering`
records scope decisions where labels alone would admit discretionary work. Keep both lists limited
to explicit exceptions from the review. Use the GitHub backlog for scope, scores and dependencies.

Initial consolidation candidates are #1653, #1666, #1652, #1538, #1772, #1770, #1769, #1778,
#1773 and #1497. Check live status before picking. Groom optimization, PSO work and dynamic
render-scale work wait for a measured production need or explicit research approval.

At the transition review, record any reproducible serious GPU faults, synchronization errors or
corruption with their owners. Record which required hardware CI cells ran and which skipped,
the status of shared coordinate contracts, lifecycle and memory evidence, and the current support
matrix. Link the controlled Release baseline (#1799) and the KESTREL benchmark (#1757), or name
their hardware, provenance or workload blockers. On the transition date, finish work already
underway through the normal [task loop](task-loop.md). Escalate any genuine KESTREL blocker, start
the smallest required fix and continue unblocked game work. The date still switches new picks.

## KESTREL-first from the transition date

Default to eligible KESTREL vertical-slice work and the engine changes it directly depends on.
Use the dependency order to deliver playable increments. Start with the benchmark and real terrain
and forest (#1732–#1734) as they become startable. Use the measured busiest view on GTX 1660 / RX
5600 class hardware at 1080p to guide the 60 fps budget (p95 at or below 16.667 ms). Compare
production configurations with equivalent quality and record what each configuration actually
ran. Follow [ADR 0027](../adr/0027-kestrel-lives-in-the-engine-repo-as-a-second-project.md) for the
engine/game boundary. Add milestone work only with the owner's approval.

Aim over several weeks for about 70% KESTREL and its required engine work, 20% engine maintenance
and architecture, and up to 10% optional rendering research. Review recent starts and merged PRs
when proposing a slate. Use issue Effort as a rough proxy if totaling allocation, and explain any
departure in the selection report and handover. Critical production fixes can take precedence.
Propose a bounded research task when the owner asks for research and there is capacity. Research
may be zero, and an empty slot can stay empty.

## How the picker applies the policy

- `rank` keeps the [canonical score and rank](issue-scoring.md). `selection:` gives the eligible
  order for new work. The milestone and its scored in-repo prerequisites identify KESTREL work.
  After the transition it leads maintenance within the existing low-value floor. The `goal:` slot
  still requires a milestone issue.
- Maintenance uses the existing freeze and Tier-1 labels. An issue labelled `feature` also needs
  a bug, cleanup or Tier-1 label to qualify as maintenance. The optional rendering list keeps
  discretionary work with those labels out of automatic picks. A measured requirement confirmed
  through the milestone or its dependencies qualifies for KESTREL work after the transition.
  During stabilization, eligible bug fixes lead other maintenance within the low-value floor.
- `priority:` lists confirmed Pulls, bugs labelled `priority: urgent`, and stabilization priorities
  in that order before automatic slots. Mark urgent production work only with evidence and an
  explicit owner decision. All candidates go through the same dependency, triage and registry
  checks. `--freeze` remains an explicit label lens that can narrow the scored pool further.
- `rank` reports the phase and annotates rows with eligibility and exclusions. `--as-of` previews a
  date. Use the current date for actual picks. The shared start-work skill applies the registry
  sweep, batch slot order, overlap checks and handover. Publishing follows the repository gates.
