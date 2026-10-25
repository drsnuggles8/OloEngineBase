# Groom LOD cross-fade and detail bias: details (#1612, #1616, #1628)

The stories, measurements and mechanisms behind the rules in
[groom-lod-cross-fade-and-detail-bias.md](groom-lod-cross-fade-and-detail-bias.md).

## 1. Complementary masks from one per-pixel hash

During the window (`RendererSettings::GroomLodCrossFadeFrames`, 12 by default) Scene publishes a
fading coat twice: the primary on the tier the decision selected (`GroomCrossFadeRole::Incoming`,
keeps `h < t`) and a copy on the tier it is leaving (`Outgoing`, keeps `h >= t`), with
`t = frame / (frames + 1)`. `h` is `GroomCoverage::StochasticHash` with segment 0 and
`kGroomCrossFadeSeed`. It must not depend on the segment: the two tiers draw different curves, and
the masks are complementary only if both ask the same question of the same pixel.
`GroomLodCrossFade.TheTwoMasksCoverEveryPixelExactlyOnceAtEveryThreshold` pins it.

## 2. Every pass that rasterises a tier applies the mask

The strand shader discards before its coverage test, so the depth prepass, the shading pass,
velocity and the G-buffer outputs agree. The casters discard per shadow texel through flat varyings,
with the strand pass's frame index in `GroomShadowParams.Modes.y`, in a variant
(`GroomStrandDepthCrossFade.glsl`, `VSM_GroomDepthCrossFade.glsl`) that `RenderGroomCasters` binds
only for a fading caster. A discard in the shared caster program would have cost every coat its
early depth test, every frame. The lane is built once, by `MakeGroomCrossFadeLane`.

## 3. The outgoing copy is the same coat

Everything counted per groom skips it (`GroomStrandRequest::IsCrossFadeSecondary`): the
composition, deformation, simulation, coat-shadow and LOD counters, `GroomsSubmitted`,
`GroomsDrawn`, the caster tallies (`GroomsAskedToCast`, `GroomsCasting`, `GroomsWithoutGeometry`)
and the "another coat casts" bit. Strands, segments and bytes count it, because they are cost. The
ray-traced proxy skips it, because the proxy's keys and hysteresis are per entity. Its VSM caster
key is mixed with a constant, so both footprints are tracked and the outgoing one departs when the
window closes. A fading caster invalidates its VSM pages every frame of the window.

## 4. Run the per-entity Scene work once

`DeformGroomAgainstSurface` takes the partner. It resolves the partner's streaming against the
partner's own tier. On the CPU root path it evaluates both tiers' drawn curves in one pass over the
body, because the result is indexed by base curve. Then the partner becomes a copy of the primary,
with only its tier (representation, card level, visibility step and build) and streaming fields
restored. Restore that short list; do not mirror the fields deformation writes. The first version
mirrored them and missed the body ID, so the soft intersection faded the outgoing tier at the skin.
The roots are shared through `CrossFadeRootTransforms`, a view of the primary's heap block, because
copying them costs 12 MB per fading 200k-strand coat per frame. Read roots through `DrawnRoots()`.
Running the simulation, the binding history or the streaming-key map a second time would double
the solver's rate, zero the velocity, or reset TAA every frame.

## 5. The budget, and what a refused hand-over does

`RendererSettings::GroomLodMaxCrossFades` (4) bounds a transition frame. A running window keeps its
slot until its last frame. A refused hand-over sets `GroomLodDecision::CrossFadeRefused`, which
`GroomLodStats::CrossFadesRefused` counts. It also reports `CrossFadeBudgetSpent` when nothing more
fundamental explains the coat. That reason is not a fallback, because the coat is on its tier. A
switch back mid-window reverses the window instead of restarting it, so the visible mix carries
over.

Halvings share the slots (#1628). A coat walking out crosses up to four halvings and one card edge,
so halving windows are the commoner kind. Giving them a budget of their own would double the worst
transition frame; sharing keeps one number for it. A coat holds at most one window, whatever moved:
a halving during a hand-over's window is a hand-over to a third tier and restarts the window on the
slot it holds. The dearest window is a halving going out, whose outgoing tier has twice the strands
of the incoming one: about 1.5 times the coat's strand cost before the halving. A card hand-over
adds only the cards, a few percent of the strands. Measured on the dog's 1 → 2 halving (Release
editor, Vulkan, median GroomPass ms, three sibling test processes sharing the GPU): 1.57–1.88 ms
on step 1, 3.0–3.2 ms in the window, 1.1–1.25 ms on step 2, on Forward, Forward+ and Deferred alike,
and the same coming back. The window costs the two tiers' sum.

## 6. Tests that force or walk to a tier

A coat that switches tier fades for `GroomLodCrossFadeFrames`, and under TAA a stochastic coat keeps
the outgoing tier in its history for longer than that. `GroomSoftIntersectionVisualEvidenceTest`
forces cards and settles 16 frames, past the 12-frame window, but its first card capture still
differed from its own repeat by 26k pixels. Its `Capture`, and `GroomWetnessVisualEvidenceTest`'s,
which forces tiers the same way, now set the window to 0 for the capture and restore it. The LOD
evidence fixture, which walks to a tier, runs `6 + GroomLodCrossFadeFrames` frames. With six, a
capture lands mid-fade and counts the outgoing strands as drawn.

## 7. The detail bias

`ApplyGroomDetailBias` divides the tier edges, the three budget curves, the coat shadow's
`PixelSizeForLod0` and the ray-traced proxy's 192 px tier. The proxy follows so that it keeps
handing over below the raster card edge. The hysteresis, the width cap and the card level's
`SourcePixelSize` are not scaled: the first two are ratios, and the last is a fact about the cooked
bytes. Dividing by exactly 1 is the IEEE identity, so High (1) is bit-identical to no bias.
`GroomLodDetailBias.ABiasOfOneIsBitIdenticalToNoBias` pins the policies and the decisions frame by
frame.

## 8. Judge a fading shadow by its step

On a dense coat the cards' shadow lies inside the strands', so two unmasked casters cast exactly the
strands' shadow. The failure shows as a shadow that holds the outgoing tier for the whole window and
steps at its end. `TheCoatsShadowFadesWithItsColour` asserts that the fade's largest step is under
half the hard switch's. Measured with the caster discard removed, the step is 0.86 of the hard
switch's; masked, it is 0.31.

The first shadow check asserted that the fading coat's shadow stayed between the two tiers'
shadows. It passed, and it also passed with the caster mask removed, because a doubled shadow on
this coat is no darker than the strands' shadow alone. The negative control's survival is what
showed that the check measured nothing. The step is what differs between the two arms.

A halving's shadow step is a fifth of the hand-over's (0.0035 in the ground's light against 0.0177),
because the width compensation keeps the coverage. The test judges it against the series' own quiet
frames (0.0001), not against the hand-over's 0.012 floor. Masked, the faded halving's largest step
is 0.33 of the hard one's; with the caster discard removed it is 0.69, and the test fails.

## 9. Draw a fading pair prepass, prepass, shade, shade

The two tiers cover the same pixels, and a tier whose depth prepass runs after the other tier's
shading draw over those pixels loses the early depth rejection its own shading relies on. On the
dog at the hand-over (Release editor, Vulkan and GL alike), the second tier's shading cost exactly
its no-prepass cost, and the window's GroomPass was 2.2 ms against 0.75 ms for the two tiers alone.
A coarser mask (2×2, 8×8) did not help, and drawing the strand tier first only moved the cost to the
cards. `GroomRenderPass` holds the first tier's shading draw (`HeldShade`, with its own bindings)
until the partner's prepass is down: 0.62 ms going out, 1.15 ms coming back. The hold needs the
partner to be the very next request, which Scene guarantees and the pass asserts. A held draw whose
partner drew nothing is replayed before the next coat does anything. Measure a window by the frame
its sample describes (`olo_perf_pass_timings` `frame.gpuMeasurementFrameId`) and by the strand
sub-passes. A 120-frame window lasts about a second in a Release editor.

## 10. Once per frame, not once per publish

A probe bake renders six cube faces between two frames, and each render publishes the grooms. When
each publish advanced the state, a bake after every frame reset the hold, so the main view never
handed over. A shorter bake ate the fade window instead. The frame entry points call
`Scene::BeginGroomFrame`, and the frame's first publish records its decisions. Any later publish in
the same frame reuses them, and does not compare or sweep per-entity state. Deciding again from a
copy is not enough: the copy is already one frame ahead, and a view that wants a finer tier would
open a new window every frame. Pinned by `ASecondViewInOneFrameAdvancesNeitherTheLadderNorTheWindow`,
for a hand-over and for a halving. Its negative control gives 0 hand-overs.

## 11. A VSM caster that stops fading

Its cached pages hold the dithered silhouette, and on a static coat nothing else redraws them. The
outgoing tier's departure redraws only its own footprint. Pinned by
`TheFirstSteadyFrameAfterAFadeRedrawsTheCoatsVirtualShadow` (2 invalidations, against 1 in the
negative control), for a hand-over and for a halving.

## 12. Only a coat that can reach the view opens a window

The LOD advances for every coat by distance, and the view cull comes after it. Before the cull moved
ahead of the fade, a coat behind the camera that changed tier took a slot, and a visible coat was
refused. A coat out of every view has its window closed on its tier, its visibility step included.
Pinned by `ACoatOutOfViewTakesNoCrossFadeSlot` (negative control: 0 fading frames, 1 refused).
Coats whose window runs on into the frame are decided first, and each takes its slot only once it's
known to be in view. A count taken before the loop kept a slot for a coat that left the view that
frame. Pinned by `AFadingCoatThatLeavesTheViewGivesItsSlotToAVisibleOne`.

## 13. A halving is a hand-over

When a coat's visibility step moved, the strand build switched from one `MaxStrands` to half of it
in one frame. The width compensation keeps the coverage, but the strands' positions all change at
once. On the `GroomLodVisualEvidenceTest` dolly, after #1612 had faded the card hand-over to
0.7% a frame, the halvings were the largest steps on the walk: 3.0–3.7% of the coat's
area-normalised light. So `GroomCrossFadeState` carries a visibility step on each side, and a step
change opens the same window, with the outgoing copy on the previous step's `MaxStrands`.

Three details make it correct rather than merely working:

- **The state keeps the step the coat was built on, not the strands it builds.** The outgoing copy
  then rebuilds exactly last frame's build, which is in the geometry cache. Normalising every step
  that covers all the curves to step 0 would have keyed the copy on the full `m_MaxRenderStrands`
  and rebuilt the coat on the window's first frame.
- **A halving that thins nothing opens nothing.** On cards, or on a coat with fewer curves than
  `m_MaxRenderStrands`, several steps build every curve. `GroomCrossFadeStrands` (the budget and the
  tier's curve count) makes two such steps one tier, and the state follows the step without a
  window. Otherwise each would hold a slot for 12 frames while drawing one picture twice.
  `AHalvingThatThinsNothingOpensNoWindowButMovesTheStep` pins it.
- **A coat starts settled on the tier it draws** (`SettledGroomCrossFade`). The default state is
  strands at full budget, and a population budget can put a new coat on a coarse step from its first
  frame. From the default state it would fade in from strands it never drew. The out-of-view close
  settles the step for the same reason.

## What was measured

On the `GroomLodVisualEvidenceTest` coat (stochastic composition, TAA, a dolly out through the
220 px hand-over), the hard switch steps the coat's area-normalised light by 3.4–3.9% in one frame.
The 12-frame cross-fade's largest step is 0.81–0.88% on Forward, Forward+ and Deferred, and both
runs arrive at the same place: the total change agrees within 0.4%. Without TAA the largest step is
0.70%, with no hole and no doubled coat. A transition frame's GroomPass costs about the sum of the
two tiers'. `GroomLodCrossFade[Off|NoTAA]_GL_<Path>_Dolly.png` are the film strips.

The same walk crosses two halvings: 1 → 2 on the strands and 2 → 3 on the cards (#1628). The
largest frame-to-frame step within each halving's window:

| path | strands 1 → 2, hard | faded | cards 2 → 3, hard | faded |
|---|---|---|---|---|
| Forward | 3.19% | 0.52% | 2.15% | 0.84% |
| Forward+ | 3.25% | 0.50% | 2.39% | 1.13% |
| Deferred | 3.46% | 0.66% | 1.94% | 0.86% |
| Deferred, MSAA 4 | 3.62% | 0.64% | 2.46% | 1.13% |
| Forward, no TAA | 3.21% | 0.50% | 2.40% | 0.84% |

The no-TAA hard column is master's binary on the same walk, where the halvings had no window. Without
TAA no window frame leaves the two tiers' band (no hole, no doubled coat). With no slot, the
hand-over and both halvings hard-switch and are counted as refused (3).
`GroomLodHalvingFade[Off|NoTAA]_GL_<Path>[_Msaa4]_Dolly.png` are the film strips.
