# Adding a technique: the selection is a value, not another `if`

**Rule.** When a subsystem grows a second way of computing the same number, make the choice a
**pure function returning a value plus the reason it is not what was asked for**, and put the
counters next to it. The shader may still branch on the routing the CPU published — the
structural fallback below needs exactly that — but it must not SELECT the technique or own the
reasoning about why it did not get one. And do not add a row to a path enum.

The value has three parts, and all three are load-bearing:

1. **the effective technique** — what this thing actually gets;
2. **the reason** — an enumerated one, ordered most-fundamental first, with a *sentence* per
   entry, not a token;
3. **the resource index it was granted**, if the technique is budgeted (a mask channel, an atlas
   tile, a history slot) — with a named sentinel for "none", never a bare `-1`.

Then the caller records every decision into a stats block, and the counters are what a user reads
when the feature does not appear to work.

---

## Why the branch is the wrong shape

`include/DeferredLightingShared.glsl` used to read:

```glsl
if (VSM_ENABLED != 0) { shadow = vsmShadowFactor(...); }
else                  { shadow = calculateCascadedShadowFactorCSM(...); }
```

That is fine for two arms selected by one flag. Ray-traced shadows (#1056) made it three arms
selected by three unrelated flags — a settings enum, a device capability, and whether a render
graph node actually produced a texture this frame. Written as a third `if`, the interesting
question becomes unanswerable from anywhere: *this light asked for ray tracing and did not get it,
why?* The shader cannot say, because by the time it runs the reason has been collapsed into a
uniform. The CPU cannot say either, because nothing on the CPU made the decision — the shader did.

Making it a value moves the decision to the one place that can explain it, and it moves the
decision **off the GPU**, where it becomes testable on a machine with no GPU. That matters more
than it sounds: the fallback arm is the arm every CI runner takes, so the arm CI can cover best is
exactly the one that most needs covering.

## Why not a new row in the path enum

`RenderingPath` reserved a commented-out `HybridRT` row for exactly this feature, and promoting it
was tempting because the row was already there. It is still wrong, for two reasons:

- **Combinatorial.** Shadow technique is orthogonal to the G-Buffer strategy. Folding it in makes
  the enum `Deferred x {shadow map, VSM, ray traced} x {future reflection tiers}`.
- **It changes the meaning of existing predicates.** There were ~70 `Path == RenderingPath::Deferred`
  tests in the tree, and every one of them means *"is there a G-Buffer"*. A `HybridRT` path has a
  G-Buffer, so all seventy would silently start answering "no" — with no compile error anywhere.

Virtual Shadow Maps made the same call earlier and live on `ShadowSettings`. Follow that.

If you decline a reserved row, **leave it commented out with the reason** rather than deleting it.
A deleted row reads as a question nobody asked.

## Order the reasons, and let the first match win

Order the enum most-fundamental first — a thing that trips an earlier guard would trip later ones
too — and report the **first** match. Then a "dominant reason" summary names something the user can
actually fix rather than the last symptom in the chain.

Two reasons that look alike must stay apart. `RayTracingUnavailable` ("this GPU cannot ray trace")
and `AccelerationStructureEmpty` ("nothing has been built yet, which is normal on frame one") are
both zero ray-traced lights, and conflating them turns a first-frame warm-up into a hardware
diagnosis.

Do **not** count "it was never requested" as a fallback. A scene that simply does not use the
feature would otherwise report every light as a failure.

## Make the fallback structural, not a flag

The strongest version of this: arrange it so the routing that turns the new technique **on** is
written by the code that knows it succeeded, and defaults to off everywhere else.

In #1056 the shadow UBO's routing lanes go up with the rest of the shadow data set to *inactive*,
and only `RayTracedShadowPass::Execute` — after its draws — turns them on. Every way the pass can
fail therefore leaves the lighting shader on the raster branch by construction, rather than by
someone remembering to reset a flag on each of six early-return paths.

Two smaller instances of the same idea, both worth copying:

- **Clear the mask to "fully lit", not to zero.** A visibility target cleared to black makes every
  uncovered pixel fully shadowed, so the pass's failure mode is a black screen. Cleared to white it
  is an unshadowed one, which is the failure you want.
- **Bind a white 1x1 when there is no mask.** A dangling sampler is undefined behaviour, not a zero
  read. Bind something valid always, and let the *routing* — not the binding — decide whether it is
  sampled.

## Hash the new gate into the graph fingerprint

**If the technique decides whether a render-graph resource is DECLARED, it is topology,
and topology is cached.** The render graph keeps a fingerprint of the state its node
`Setup()`s depend on and skips rebuilding when it has not changed. A technique flip that
is not in that fingerprint arms the pass, declares the mask, and still leaves the cached
topology in which the node declared nothing — so the node stays **culled**, `Execute`
never runs, and the counters that were supposed to explain all this report a truthful,
useless zero. The feature does not fail; it is simply absent.

This is not hypothetical, and the near-miss is instructive: `HashPassState` looks like it
covers a pass, and it does hash the pass pointer *and* `IsReadyForExecution()` — enough
to make the mistake invisible in review. Its own comment says per-pass **enabled** state
is folded in separately, next to the other feature flags. A technique living on
`ShadowSettings` rather than on `PostProcessSettings` reaches none of that by default.

Symptom to recognise: the lever reads back "on", the pass appears in the frame breakdown,
`culled: true`, `declaresResources: false`, and every counter is zero. It cost a live
editor bisect on #1056; no headless test can see it, because the fingerprint only matters
once a graph has been built and cached.

**So: when the choice gates a resource declaration, add it to the fingerprint in the same
commit that adds the gate.**

## Ratchet the untouched path with bytes, not eyes

The claim "turning the new technique on changes nothing where it cannot run" is testable exactly,
and should be: render the scene twice, once per technique setting, on a device that cannot serve
the new one, and assert the two frames are **byte-identical**. A stray uniform, a re-bound texture
unit, a cleared target or a re-declared graph resource each move a pixel.

Pair it with a contrast assertion on the same capture, or two identical black frames pass.

## Checklist

- [ ] The choice is a pure, `constexpr`-evaluable function of gathered inputs — no renderer state
      reached from inside it.
- [ ] Its inputs come from what the frame **actually resolved**, never from what it expects to.
- [ ] Every reason has a distinct, non-empty sentence (test it; a `ToString` returning `"unknown"`
      is a counter nobody can act on).
- [ ] Counters separate "failed to deliver" from "never asked".
- [ ] The enabling routing is written by the success path and defaults to off.
- [ ] The choice is hashed into the render-graph fingerprint if it gates a resource declaration.
- [ ] A byte-identity ratchet covers the untouched path.
- [ ] The log line fires on a **change of reason**, not per frame.

---

## Appendix: the #1056 shape, for reference

`Renderer/Shadow/ShadowTechnique.h` holds the whole seam and nothing else:

| Piece | What it is |
|---|---|
| `ShadowTechnique` | `ShadowMap` / `RayTraced`. The raster tier is permanent (#979), not a migration. |
| `ShadowTechniqueFallbackReason` | Eight ordered reasons, each with a sentence. |
| `ShadowTechniqueInputs` | Six gathered booleans + the request. |
| `SelectShadowTechnique(inputs, channelsTaken)` | The pure decision. |
| `ShadowTechniqueStats` | `RayTracedLights`, `FallbackLights`, `ByReason[]`, `DominantFallbackReason()`. |

`ShadowTechniqueSelectionTest.cpp` covers every reason with one input knocked out at a time, so the
reason asserted on is the one that input owns and not a second failure riding along.
