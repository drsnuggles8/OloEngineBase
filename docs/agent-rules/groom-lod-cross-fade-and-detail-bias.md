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
   the G-buffer outputs agree. The casters discard per shadow texel through flat varyings, with the
   strand pass's frame index in `GroomShadowParams.Modes.y`, in a variant
   (`GroomStrandDepthCrossFade.glsl`, `VSM_GroomDepthCrossFade.glsl`) that `RenderGroomCasters`
   binds only for a fading caster. A discard in the shared caster program would have cost every
   coat its early depth test, every frame. The lane is built once, by `MakeGroomCrossFadeLane`.

3. **The outgoing copy is the same coat, not a second one.** Everything counted per groom skips it
   (`GroomStrandRequest::IsCrossFadeSecondary`): the composition, deformation, simulation,
   coat-shadow and LOD counters, `GroomsSubmitted`, `GroomsDrawn`, the caster tallies
   (`GroomsAskedToCast`, `GroomsCasting`, `GroomsWithoutGeometry`) and the "another coat casts"
   bit. Strands, segments and bytes count it, because they are cost. The
   ray-traced proxy skips it, because the proxy's keys and hysteresis are per entity. Its VSM caster
   key is mixed with a constant, so both footprints are tracked and the outgoing one departs when the
   window closes. A fading caster invalidates its VSM pages every frame of the window.

4. **Run the per-entity Scene work once, then copy the whole primary and restore the partner's own
   fields.** `DeformGroomAgainstSurface` takes the partner. It resolves the partner's streaming
   against the partner's own tier. On the CPU root path it evaluates both tiers' drawn curves in
   one pass over the body, because the result is indexed by base curve. Then the partner becomes a
   copy of the primary, with only its tier and streaming fields restored. Restore that short list;
   do not mirror the fields deformation writes. The first version mirrored them and missed the body
   ID, so the soft intersection faded the outgoing tier at the skin. The roots are shared through
   `CrossFadeRootTransforms`, a view of the primary's heap block, because copying them costs 12 MB
   per fading 200k-strand coat per frame. Read roots through `DrawnRoots()`. Running the simulation,
   the binding history or the streaming-key map a second time would double the solver's rate, zero
   the velocity, or reset TAA every frame.

5. **A hand-over the budget cannot fade is a hard switch, reported by name.**
   `RendererSettings::GroomLodMaxCrossFades` (4) bounds a transition frame, which costs about both
   tiers' GroomPass time. A running window keeps its slot until its last frame. A refused hand-over
   sets `GroomLodDecision::CrossFadeRefused`, which `GroomLodStats::CrossFadesRefused` counts. It
   also reports `CrossFadeBudgetSpent` when nothing more fundamental explains the coat. That reason
   is not a fallback, because the coat is on its tier. A switch back mid-window reverses the window
   instead of restarting it, so the visible mix carries over.

6. **A test that forces a tier holds the cross-fade off; one that walks to it waits the window out.**
   A coat that switches tier fades for `GroomLodCrossFadeFrames`, and under TAA a stochastic coat keeps
   the outgoing tier in its history for longer than that. `GroomSoftIntersectionVisualEvidenceTest`
   forces cards and settles 16 frames, past the 12-frame window, but its first card capture still
   differed from its own repeat by 26k pixels. Its `Capture`, and `GroomWetnessVisualEvidenceTest`'s, which forces tiers the same way, now set the window to 0 for the capture
   and restore it. The LOD evidence fixture, which walks to a tier, runs `6 + GroomLodCrossFadeFrames`
   frames. With six, a capture lands mid-fade and counts the outgoing strands as drawn.

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

9. **Draw a fading coat's pair prepass, prepass, shade, shade.** The two tiers cover the same pixels,
   and a tier whose depth prepass runs after the other tier's shading draw over those pixels loses
   the early depth rejection its own shading relies on. On the dog at the hand-over (Release editor,
   Vulkan and GL alike), the second tier's shading cost exactly its no-prepass cost, and the window's
   GroomPass was 2.2 ms against 0.75 ms for the two tiers alone. A coarser mask (2×2, 8×8) did not
   help, and drawing the strand tier first only moved the cost to the cards. `GroomRenderPass` holds
   the first tier's shading draw (`HeldShade`, with its own bindings) until the partner's prepass
   is down: 0.62 ms going out, 1.15 ms coming back. The hold needs the partner to be the very next
   request, which Scene guarantees and the pass asserts. A held draw whose partner drew nothing is
   replayed before the next coat does anything. Measure a window by the frame its sample
   describes (`olo_perf_pass_timings` `frame.gpuMeasurementFrameId`) and by the strand sub-passes.
   A 120-frame window lasts about a second in a Release editor.

10. **Advance the LOD ladder and the fade window once per frame, not once per publish.** A probe
    bake renders six cube faces between two frames, and each render publishes the grooms. When each
    publish advanced the state, a bake after every frame reset the hold, so the main view never
    handed over. A shorter bake ate the fade window instead. The frame entry points call
    `Scene::BeginGroomFrame`, and the frame's first publish records its decisions. Any later
    publish in the same frame reuses them, and does not compare or sweep per-entity state. Deciding
    again from a copy is not enough: the copy is already one frame ahead, and a view that wants a
    finer tier would open a new window every frame. Pinned by
    `ASecondViewInOneFrameAdvancesNeitherTheLadderNorTheWindow`. Its negative control gives 0
    hand-overs.
11. **A VSM caster that stops fading counts as moved for one frame.** Its cached pages hold the
    dithered silhouette, and on a static coat nothing else redraws them. The outgoing tier's
    departure redraws only its own footprint. Pinned by
    `TheFirstSteadyFrameAfterAFadeRedrawsTheCoatsVirtualShadow` (2 invalidations, against 1 in
    the negative control).

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
