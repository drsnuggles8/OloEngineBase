# Groom guide simulation: choosing a solver, and what it cost

Issue #1250 carries `confidence: 0.5` and an explicit instruction: *prototype and measure a
solver/collision approach first, and rescope with evidence if it is rejected.* This is that
measurement. It is deliberately short, and every number in it is produced by a test that can be
re-run rather than by a one-off script:

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=GroomGuideSimulation.*:GroomSimulationVisualEvidence.*
```

## 1. The question

A coat is 200 000 strands. Simulating them is not a budget problem, it is three orders of magnitude
out, so the shape of the answer was never in doubt: simulate a few hundred **guides** and
interpolate the rest. What *was* in doubt is the integrator, because acceptance criterion 1 is
stated as a **tolerance on length across variable frame rate** — and that is a property an
integrator either has structurally or does not have at all.

Three candidates, all implemented, all still in the tree (`GroomSolverModel`), because a measurement
nobody can re-run is a claim:

| Model | How it enforces length | Cost per substep |
|---|---|---|
| `FollowTheLeader` | one root-to-tip **projection**: each particle placed at its exact rest distance from its already-final parent | 1 pass |
| `DynamicFollowTheLeader` | the same projection, plus Müller's velocity correction — the momentum the projection removed is handed back to the particle that lost it | 1 pass + 1 fix-up |
| `PositionBasedDistance` | Gauss-Seidel distance constraints, N iterations | N passes |

## 2. The measurement

`GroomGuideSimulationTest.SolverModelComparison`: four guides of twelve particles, the root swung
through ±0.8 world units at about 1 Hz for two seconds, at three frame rates. Reported is the worst
`|segment| / restLength - 1` seen on **any** frame of the run — a solver that is inextensible only at
the end is not inextensible — and the worst distance from a particle to its groomed rest position,
which doubles as the "did it actually move" control.

Pasted from the test's own stdout; re-run it rather than editing it.

```
[solver] FollowTheLeader            30.0 Hz  worst |stretch-1| = 0.000001  worst rest deviation = 1.295210 m
[solver] FollowTheLeader            60.0 Hz  worst |stretch-1| = 0.000001  worst rest deviation = 0.845557 m
[solver] FollowTheLeader           144.0 Hz  worst |stretch-1| = 0.000001  worst rest deviation = 0.872930 m
[solver] DynamicFollowTheLeader     30.0 Hz  worst |stretch-1| = 0.000001  worst rest deviation = 0.657357 m
[solver] DynamicFollowTheLeader     60.0 Hz  worst |stretch-1| = 0.000001  worst rest deviation = 0.671261 m
[solver] DynamicFollowTheLeader    144.0 Hz  worst |stretch-1| = 0.000001  worst rest deviation = 0.728949 m
[solver] PositionBasedDistance      30.0 Hz  worst |stretch-1| = 0.125875  worst rest deviation = 0.590258 m
[solver] PositionBasedDistance      60.0 Hz  worst |stretch-1| = 0.170356  worst rest deviation = 0.629037 m
[solver] PositionBasedDistance     144.0 Hz  worst |stretch-1| = 0.187289  worst rest deviation = 0.683822 m
```

## 3. What the numbers say

**The two projection models are exact, at every step size.** `1e-6` is the noise floor of the
measurement itself, which takes a square root; the projection has no residual to converge, so there
is nothing for a larger step to leave behind. That is the whole argument for this family, and it is
structural rather than tuned.

**`PositionBasedDistance` is out of contract at every frame rate, by two orders of magnitude.**
0.126 to 0.187 against a declared tolerance of 0.01 — a coat 13 to 19 times stretchier than it says
it is. Four iterations is already four times the per-substep cost of the alternative, for that.

Note what the table does **not** show: a clean "error falls as the step shrinks" law. PBD's worst
figure *rises* slightly with the frame rate (0.126 at 30 Hz, 0.187 at 144 Hz), which is the opposite
of the textbook expectation and is an artefact of the statistic rather than of the solver — the
number is the worst over **every frame of the run**, and a 144 Hz run has nearly five times as many
frames in which to catch the peak. The claim the evidence supports is the flat one: PBD is far out
of contract at all three rates. It is deliberately not dressed up as a step-size law, because a
measurement that is reported as more than it is becomes a fact nobody can reproduce.

**The velocity correction reduces lag.** `DynamicFollowTheLeader`'s worst deviation from the groomed
shape is smaller than `FollowTheLeader`'s at all three frame rates — 0.657 vs 1.295, 0.671 vs 0.846,
0.729 vs 0.873. Returning the momentum the projection removed lets the chain **track** its driver
instead of trailing it, and deviation-from-rest is exactly what trailing looks like. Three out of
three across a 5x span of step sizes, which is why it is asserted rather than described.

Two directional claims were tried before that one and both were wrong on their own numbers, which is
recorded here because the wrong ones are the intuitive ones:

- *"DFTL deviates further, because it moves more."* It deviates **less**. Liveliness and lag are not
  the same axis, and the one this measures is lag.
- *"DFTL keeps swinging longer once the driver stops."* On this fixture it travels about a third as
  far. The FTL figure was 8 m of tip path for a 1.1 m chain in half a second, which is per-step
  chatter and not swing — the metric was measuring the wrong thing, so it was removed rather than
  kept with a loosened bound.

What survives instead is a structural assertion,
`DynamicFollowTheLeaderAtZeroCorrectionIsFollowTheLeader`: at a zero correction coefficient the two
models produce bit-identical state. That is the invariant that justifies them being one code path,
and someone splitting them later has to break it to do so.

### Verdict

`DynamicFollowTheLeader` is the default: exact length by construction, and measurably less lag than
plain `FollowTheLeader` at every rate tested. No approach was rejected in the sense the issue
anticipated — the guide-and-interpolate shape held, and so did the fitted collision proxy — but
`PositionBasedDistance`, which is the *general* answer and the one a reasonable person reaches for
first, **is** rejected as the default on the numbers above. It stays in the tree as the reference the
other two are measured against.

### What I would not claim from this

The correction's effect on *liveliness* — the property the literature actually motivates DFTL with —
was not reproduced on this fixture, in either metric tried. The implementation follows Müller's
velocity update (the sign was re-derived against the paper when the first measurement came out
backwards), and the length guarantee both models share is what criterion 1 rests on, so the default
does not depend on the liveliness claim. But it is the part of this document I am least sure of, and
a fixture with a real groomed coat rather than a straight chain would be the way to settle it.

## 4. Collision: what was measured, and the trade that was chosen

The proxy is **fitted from the bound body's own vertices** rather than authored — one capsule per
bone, from the principal axis of the vertices that bone dominates, cut at percentiles.
`GroomBodyCollider.h` argues the choice; what matters here is the trade inside the substep:

**Collision is resolved BEFORE the length projection.** The consequence is exact length and a
particle that may end a step a fraction of a *segment* inside the proxy;
`m_ColliderPadding` is the shell that buys that back.
`GroomGuideSimulationTest.CollisionKeepsGuidesOutOfTheBody` measures the worst penetration against
that declared bound — written as the arithmetic (`segment length + padding`), not as a magic number,
so changing either input moves the bound with it.

The alternative ordering gives exact non-penetration and a visibly stretched strand. A strand that
grazes a capsule by a tenth of a millimetre is not visible at any resolution; a strand that stretches
is the failure criterion 1 is named after. So the ordering is not a detail — it is criterion 1 being
given priority over criterion 2 where the two conflict, and it is stated here so that a future
reader changing it knows what they are trading.

Full individual-hair self-collision was **not** implemented and was not needed for any of the
benchmark motions, which is the scope boundary the issue drew.

## 5. Cost

`GroomSimulationVisualEvidenceTest.SimulationMovesTheCoatOnEveryRenderPath` prints the per-frame
population, which is what the issue's "record cost as guide/render-hair counts change" asks for:

```
[groom-sim] Forward       133251 px moved  guides 200  points 2400  strands 2400 (unguided 0)  contacts 52084  worst |stretch-1| 0.000015 (tolerance 0.0100)  worst rest deviation 1.5844 m
[groom-sim] ForwardPlus   133251 px moved  guides 200  points 2400  strands 2400 (unguided 0)  contacts 52084  worst |stretch-1| 0.000015 (tolerance 0.0100)  worst rest deviation 1.5844 m
[groom-sim] Deferred      133251 px moved  guides 200  points 2400  strands 2400 (unguided 0)  contacts 52084  worst |stretch-1| 0.000015 (tolerance 0.0100)  worst rest deviation 1.5844 m

[groom-sim]     30 Hz  worst |stretch-1| 0.000010  worst rest deviation 1.6652 m  steps 2
[groom-sim]     60 Hz  worst |stretch-1| 0.000015  worst rest deviation 1.5844 m  steps 1
[groom-sim]    144 Hz  worst |stretch-1| 0.000047  worst rest deviation 1.3337 m  steps 0
```

Two things in that second block are worth reading carefully rather than skimming.

**The three render paths are byte-identical, and that is expected.** The strand pass is one
path-agnostic forward-style pass by design (#1246), and with only the coat in frame there is
nothing for the paths to differ about. The per-path evidence that carries weight is therefore
`GroomsSimulated` and the stretch figure on each path, not a pixel difference between them --
the same reasoning GroomBindingVisualEvidenceTest states for its own captures.

**Frame-rate independence is a ~20 % spread, not an equality.** Worst rest deviation runs
1.67 m at 30 Hz, 1.58 at 60 and 1.33 at 144. The LENGTH contract holds at all three by three
orders of magnitude, which is what criterion 1 is stated in; the deviation differs because a
30 Hz run samples the driving pose five times more coarsely than a 144 Hz one, so the three
runs are not solving against the same sequence of targets. That is a property of the fixture's
driver, not of the solver, and it is why the test bounds the RELATIVE difference at 35 % rather
than asserting two identical pictures -- an equality assertion there would have been asserting
something untrue, and would have been "fixed" by loosening it until it passed.

The shape of the cost, rather than the absolute Debug-build numbers, is the useful part:

- **The solver is O(guides x points x substeps)** and is bounded by the per-role budget on
  `GroomSimulationComponent`. On the evidence fixture that is 200 guides of 12 particles, so 2 400
  particle updates per substep against a 2 400-strand geometry rebuild in the same frame. No
  profiler capture was taken, so the ratio is arithmetic rather than a measurement -- what is being
  claimed here is the SHAPE of the cost and its bound, not a figure.
- **The interpolation is O(rendered points x 4)** and happens inside `BuildGroomStrandMesh`, which
  a bound groom already runs every frame (#1249). It adds four vector loads and a `mix` per emitted
  point to a loop that was already writing 64 bytes per vertex; it does not add a pass.
- **The collider fit is O(body vertices), once**, cached against the same identity keys the
  binding's compatibility verdict uses. Re-fitting per frame would be the expensive thing, and it is
  the thing that is explicitly not done.
- **Raising the guide budget raises the first cost only.** Raising `MaxRenderStrands` raises the
  second only. They are separately authored for exactly that reason.

Live, on the editor scene (`Scenes/GroomSimulatedCoat.olo`, RTX 4090, Debug), the same coat
reports `2000 of 2000 strands (stride 1), 14000 segments, 3.74 MiB` of strand geometry per
frame -- the buffer a bound groom already refills every frame since #1249. The simulation adds
no buffer of its own on the GPU: the guide displacements are CPU-side and are consumed inside
the same build that writes those 3.74 MiB.

## 6. What this did not answer

- **Release-build timings.** Every number here is from a Debug run on one machine; the comparison
  between models is like-for-like and is what the choice rests on, but the absolute figures are not
  a budget. `docs/guides/renderer-benchmarks.md` is where a real capture would go.
- **Wind.** `ClothWindSystem` exists and a coat should eventually read it. The solver takes a world
  acceleration and nothing here would need to change; it was left out because it is not in any of
  the issue's criteria and would have been an unmeasured feature.
- **Strand-level LOD interaction.** #1252 refactors the strand-geometry cache this writes into. The
  interpolation is a pure function of the guide displacements and the influence table, so it moves
  wherever that build moves; nothing here pins it to the current call site.
