# Groom LOD: the cross-fade across a hand-over, and the global detail bias (#1612, #1616)

The rules for changing how a groom moves between its representations. The ladder itself, the
budgets and the width compensation are in [groom-representation-lod.md](groom-representation-lod.md).

## The rules

1. **Hide a hand-over by drawing both tiers through complementary masks from one per-pixel hash.**
   During the window (`RendererSettings::GroomLodCrossFadeFrames`, 12 by default) Scene publishes a
   fading coat twice: the primary on the tier the decision selected (`GroomCrossFadeRole::Incoming`,
   keeps `h < t`) and a copy on the tier it is leaving (`Outgoing`, keeps `h >= t`), with
   `t = frame / (frames + 1)`. `h` is `GroomCoverage::StochasticHash` with segment 0 and
   `kGroomCrossFadeSeed`. It must not depend on the segment: the two tiers draw different curves,
   and the masks are complementary only if both ask the same question of the same pixel.
   `GroomLodCrossFade.TheTwoMasksCoverEveryPixelExactlyOnceAtEveryThreshold` pins it.

2. **Every pass that rasterises a tier applies the mask, from the same two numbers.** The strand
   shader discards before its coverage test, so the depth prepass, the shading pass, velocity and
   the G-buffer outputs agree. `GroomStrandDepth.glsl` and `VSM_GroomDepth.glsl` discard per shadow
   texel through flat varyings, with the strand pass's frame index in `GroomShadowParams.Modes.y`.
   The lane is built once, by `MakeGroomCrossFadeLane`.

3. **The outgoing copy is the same coat, not a second one.** Everything counted per groom skips it
   (`GroomStrandRequest::IsCrossFadeSecondary`): the composition, deformation, simulation,
   coat-shadow and LOD counters, `GroomsSubmitted`, `GroomsDrawn`, `GroomsAskedToCast` and the
   "another coat casts" bit. Strands, segments and bytes count it, because they are cost. The
   ray-traced proxy skips it, because the proxy's keys and hysteresis are per entity. Its VSM caster
   key is mixed with a constant, so both footprints are tracked and the outgoing one departs when the
   window closes. A fading caster invalidates its VSM pages every frame of the window.

4. **Run the per-entity Scene work once and copy it to the partner.** `DeformGroomAgainstSurface`
   takes the partner. It resolves the partner's streaming against the partner's own tier. On the
   CPU root path it evaluates both tiers' drawn curves in one pass over the body, because the result
   is indexed by base curve. It copies the binding, root inputs, history and guide displacements.
   Stepping the simulation, advancing the binding history or writing the streaming-key map a second
   time would double the solver's rate, zero the velocity, or reset TAA every frame.

5. **A hand-over the budget cannot fade is a hard switch, reported by name.**
   `RendererSettings::GroomLodMaxCrossFades` (4) bounds a transition frame, which costs about both
   tiers' GroomPass time. A running window keeps its slot. A refused hand-over reports
   `GroomLodFallbackReason::CrossFadeBudgetSpent` on its frame and counts in
   `GroomLodStats::CrossFadesRefused`. A switch back mid-window reverses the window instead of
   restarting it, so the visible mix carries over.

6. **A capture after a pose change waits out the window.** The evidence fixture's `Capture` runs
   `6 + GroomLodCrossFadeFrames` frames. With six, a capture lands mid-fade and counts the outgoing
   strands as drawn.

7. **The detail bias divides every pixel threshold once, where Scene assembles the policy.**
   `ApplyGroomDetailBias` divides the tier edges, the three budget curves, the coat shadow's
   `PixelSizeForLod0` and the ray-traced proxy's 192 px tier. The proxy follows so that it keeps
   handing over below the raster card edge. The hysteresis, the width cap and the card level's
   `SourcePixelSize` are not scaled: the first two are ratios, and the last is a fact about the
   cooked bytes. Dividing by exactly 1 is the IEEE identity, so High (1) is bit-identical to no
   bias. `GroomLodDetailBias.ABiasOfOneIsBitIdenticalToNoBias` pins the policies and the decisions
   frame by frame.

8. **Judge a fading shadow by its step, not its depth.** On a dense coat the cards' shadow lies
   inside the strands', so two unmasked casters cast exactly the strands' shadow. The failure shows
   as a shadow that holds the outgoing tier for the whole window and steps at its end.
   `TheCoatsShadowFadesWithItsColour` asserts that the fade's largest step is under half the hard
   switch's. Measured with the caster discard removed, the step is 0.86 of the hard switch's;
   masked, it is 0.31.

## What was measured

On the `GroomLodVisualEvidenceTest` coat (stochastic composition, TAA, a dolly out through the
220 px hand-over), the hard switch steps the coat's area-normalised light by 3.4–3.9% in one
frame. The 12-frame cross-fade's largest step is 0.81–0.88% on Forward, Forward+ and Deferred, and
both runs arrive at the same place: the total change agrees within 0.4%. Without TAA the largest
step is 0.70%, with no hole and no doubled coat. A transition frame's GroomPass costs about the sum
of the two tiers'.
`GroomLodCrossFade[Off|NoTAA]_GL_<Path>_Dolly.png` are the film strips.

## Story

The first shadow check asserted that the fading coat's shadow stayed between the two tiers'
shadows. It passed, and it also passed with the caster mask removed, because a doubled shadow on
this coat is no darker than the strands' shadow alone. The negative control's survival is what
showed that the check measured nothing. The step is what differs between the two arms.
