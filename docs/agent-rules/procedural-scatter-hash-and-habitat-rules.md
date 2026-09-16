# Procedural scatter: hash quality, habitat rules and the placement signature

Three rules for anything that turns a grid cell into a placed object — foliage, debris, decals,
crowd spawn points. All three were learned on the foliage scatter (issues #1230, #1261, #1254).

---

## 1. Judge a placement hash by the distinct-offset count, not by looking at it

**Fold every input in, THEN avalanche. A hash that mixes its seed in before its only multiply
produces streams that are affine images of each other, and the result looks fine in a screenshot.**

`FoliagePlacement::HashPosition` does this:

```cpp
u32 h = hx ^ hz ^ seed;
h = (h * 2654435761u) >> 16;     // ONE round, seed already folded in
```

The two jitter draws are `seed` and `seed + 7`. Because the seed enters before the multiply,
`hash(c, seed + 7) - hash(c, seed)` is nearly the constant `(7 * 2654435761) >> 16`. Measured over
an 80x80 grid:

| hash | distinct values of `(jitterZ - jitterX) mod 1` | distinct `(jx, jz)` pairs |
|---|---|---|
| `HashPosition` | **32** | 5173 / 6400 |
| `HashCell` (two multiply-xorshift rounds after folding) | **6398** | 6400 / 6400 |

32 distinct offsets means every plant in the layer sits on one of 32 diagonals inside its own cell.
At blade scale that is invisible; at landscape scale it is the repetition an art director notices
and nobody can name.

**The measurement is one loop and it is the test.** Counting distinct offsets between two draws
costs nothing and fails loudly; "the scatter looks random" passes on a 32-diagonal lattice. Pearson
correlation does NOT catch this one — it measures 0.02 here, because a constant offset mod 1 is
uncorrelated. Count distinct values.

**Count them by exact equality, and never bucket first.** Rounding the difference into buckets to
"absorb float noise" makes the BUCKET COUNT the measurement: at `1e-4` there are 10 000 buckets, so
6400 draws collide by the birthday paradox down to ~4727 however good the hash is. That is what the
first version of this test measured — 4787 — and it read exactly like a defective replacement hash.
There is no noise to absorb: these are deterministic pure functions of integers, so two equal
outputs really are the same offset. If you must bucket, check the expected collision count
`B(1 - (1 - 1/B)^N)` against your threshold first.

**Fixing such a hash moves every object, so it is an opt-in flag, never a silent improvement.**
`FoliageLayer::DecorrelatedVariation` defaults to `false`; only newly generated layers get `true`.

## 2. A rule that gates emission is not a rule that moves things — and the registry knows the difference

`FoliageInstanceRegistry`'s **placement signature** (#1261) covers exactly the inputs that map a
cell to a position: the seed, the spacing, the world extent. Everything in it retires every id in
the layer when it changes.

So when you add a field, ask one question: **does it change WHERE, or only WHETHER and HOW BIG?**

| kind of field | in the signature? | what happens on an edit |
|---|---|---|
| habitat band, splatmap mask, clump field | **no** | the excluded objects retire, every survivor keeps its id |
| scale, tint, ground offset, slope sink | **no** | every record reports as *updated*, nothing retires |
| seed, spacing, extent, a different jitter hash | **yes** | every id in the layer retires; these really are different objects |

Getting this backwards is silent both ways. Put a gating field in the signature and every habitat
tweak renumbers the whole layer, so nothing downstream can keep state across an edit. Leave a
moving field out and two different objects share one id, which is the exact lie the registry exists
to prevent.

Ground contact is the instructive case: sinking a plant changes only Y, which is an *attribute*, so
ids survive and the generation counter advances through the state hash. A test asserts that
(`GroundSinkIsAnAttributeEditNotAnIdentityChange`) — because if the sink ever leaks into the
signature the symptom is a performance cliff and a lost-state bug, not a wrong picture.

## 3. Every new placement rule defaults to OFF, and one test says so

A scatter rule that defaults to on rewrites every scene on disk. The house pattern is
`FoliageLayer::TransmissionStrength = 0.0f` (#1234): the new behaviour has to be asked for.

For #1254 that meant 19 new fields all defaulting to inert, and **one test that pins the identity**
rather than a comment that claims it — `ADefaultLayerPlacesExactlyWhereItAlwaysDid` recomputes the
expected position, scale and Y from the *legacy* hash and compares. A suitability of `0.999`
instead of `1.0` would wake up the stochastic emission comparison and start quietly deleting plants
from shipped scenes; only an exact-equality test catches that.

The corollary for a soft gate: **make "off" the multiplicative identity and skip the comparison
entirely when the product is exactly 1.** Do not route the unconfigured case through the same
stochastic test "for uniformity" — that is where the 0.999 comes from.

## 4. Floating roots are a geometry bug with a named fix

An object placed at the heightfield surface touches the ground at exactly one point: its origin. On
a slope the ground under its downhill edge is lower by `tan(slope) * halfWidth`, so that edge is in
the air. Sinking by that amount buries the uphill side instead — which is invisible, where a
floating one is not.

`FoliagePlacement::GroundSinkFor` is that expression, floored at `cos(80 degrees)` so a near-vertical
face cannot sink an object to infinity. Derive the half-width from the object's **final** scale, after
any per-instance scale modulation, or a clump-boosted plant sinks by the layer's nominal width and
floats anyway.

Not covered, and worth knowing: the terrain **renders** through a quadtree LOD whose surface is
piecewise-linear per triangle, while `TerrainData::SampleHeight` is bilinear. The two differ by up
to a quarter of a quad's height variation, and the difference is LOD-dependent. Matching them is a
separate problem from the slope fix above.
