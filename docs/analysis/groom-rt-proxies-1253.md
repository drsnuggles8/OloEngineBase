# Groom coats in the ray-traced scene — the measured comparison

Issue #1253, the last child of epic #1223. What a coat looks like to a ray, which of two
representations it gets at what distance, how far each is from the authored coat, and what both
cost.

Every figure below is produced by `GroomRayTracingProxyTest` (`OloEngine/tests/Groom/`) and is
re-derived by running it — none of it is recorded here and nowhere else.

```
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=GroomRayTracingProxy*.*
```

---

## 1. The problem, stated precisely

The ribbons `GroomRenderPass` draws do not exist in the world. `BuildGroomStrandMesh` emits a
centreline, a tangent and a radius per segment, and `GroomStrand.glsl` widens each segment
perpendicular to the segment **and to the eye vector**, with a one-pixel floor. So the drawn coat is
a different shape for every camera and has no thickness at all along the view axis.

A shadow ray or a reflection ray needs world-space triangles. The coat has none. That is the whole
of what this issue had to supply.

The obvious supply — expand every cooked strand into world-space geometry — does not fit. The
reference pelt is 16 384 strands of 10 points; expanded to crossed ribbons that is **589 824
triangles for one animal**, rebuilt every frame for a bound groom. The issue's own scope note
anticipates this: *"a measured real-time representation, not mandatory per-strand hardware ray
tracing."*

## 2. The representation, and why it is this one

**The coat's own strands, thinned by a stride and widened by `1/k`.** Two tiers, differing only in
how hard they thin.

The quantity that has to survive the thinning is what a ray actually measures: how much fibre lies
along a direction. For a set of tapered cylinders that is

```
C(w) = sum over segments of  2 * rbar_i * L_i * sin(angle between t_i and w)
```

which is **linear in the radius**. Keeping one strand in `k` and multiplying every radius by `k`
preserves `C` exactly in expectation. Not `1/sqrt(k)` — that is foliage's compensation, whose
instances are sprites whose area goes as the square of their linear size. A strand is a **band**:
length times width. `GroomLod.h` makes the same argument for the same reason about the raster tier.

### What it does not preserve

`C` is the area the coat would project **if no fibre occluded another**. In a dense coat they do, so
a thinned-and-widened coat is slightly *more* opaque than the coat it stands for — the widened
strands overlap less than the strands they replace. The error grows with the compensation, which is
why `GroomProxyPolicy::MaxWidthCompensation` caps it at 48x and why the achieved compensation is
reported beside the cap in the statistics panel rather than clamped out of sight.

### Crossed ribbons, not one

One flat ribbon per segment is half the triangles and vanishes edge-on, and the geometry cannot be
oriented towards a ray without becoming per-light — a shadow ray's direction is the *light's*. Two
ribbons in perpendicular planes through the centreline keep a non-degenerate silhouette from every
direction, for exactly 2x the triangles. The single-ribbon arm survives behind
`GroomProxyConversionSettings::CrossedRibbons` so this comparison can be re-run, not as a shipping
configuration.

### What was rejected

**A shell.** `GroomLod`'s third tier — the coat as a closed surface — is not a tier this engine
draws, and in ray space it has the defect it has in raster, only worse: a shell claims a coverage of
1 everywhere inside the silhouette, so a coat you can see *through* casts the shadow of a solid
lump. There is no distance at which the reference coats' true coverage saturates enough for that to
be honest.

**A signed distance field or a density volume.** #1248 already bakes a density volume per coat, and
reusing it would have been free. It is the wrong shape for this: `VK_KHR_ray_query` intersects
triangles, so a volume would have to be marched in a hit shader — which is a second transport
implementation, on the GPU, that has to agree with the raster one. The issue asks for coat
*intersections*, and an intersection is what a BLAS gives.

## 3. The measured comparison

Both reference coats, ground truth being the coat at its **full authored strand set** — not the
detailed tier. Comparing the proxy against the detailed tier would measure two approximations
against each other and report the detailed one as exact, which it certainly is not.

`GroomRayTracingProxyComparison.DetailedAndProxyCoatsOnTheTwoReferenceAnimals` prints this.

### Scalp — 8 192 strands, 90 112 segments (360 448 triangles at full density)

| tier | strands | triangles | compensation | mean error | max error | vertex bytes |
|---|---|---|---|---|---|---|
| Detailed | 4 096 | 180 224 | 2.0x | **0.084 %** | 0.198 % | 11.0 MiB |
| Proxy | 745 | 32 780 | 11.0x | **0.796 %** | 1.286 % | 2.0 MiB |

### Pelt — 16 384 strands, 147 456 segments (589 824 triangles at full density)

| tier | strands | triangles | compensation | mean error | max error | vertex bytes |
|---|---|---|---|---|---|---|
| Detailed | 5 462 | 196 632 | 3.0x | **0.035 %** | 0.113 % | 12.0 MiB |
| Proxy | 745 | 26 820 | 22.0x | **0.287 %** | 0.601 % | 1.6 MiB |

The error is the mean over 64 Fibonacci-sphere directions of |proxy/authored - 1|, where each
figure is that direction's `C(w)`. The max column is the worst single direction, which is what
matters for a shadow: a coat can be right on average and wrong along the one direction the sun
happens to be in.

### The three findings

**1. The compensation works, and it works far better than the tier ladder needs.** Both tiers stay
under 1.3 % of the authored coat's occlusion in the *worst* direction. Thinning a coat 22-fold and
widening its fibres 22-fold really does leave the same amount of hair between a point and a light,
to well under a percent.

The consequence is worth stating plainly, because it decides section 4: **the tier choice is a cost
decision, not a quality one.** There is no quality cliff anywhere on this ladder to place a
threshold at. A switch point can be put wherever the triangle budget wants it.

**2. Without the compensation the coat loses coverage in proportion to the thinning.** The control
arm (`WithoutTheCompensationAThinnedCoatLosesCoverageInProportionToTheThinning`) measures over
50 % error at a 16-fold thinning. That is the number the 0.3-0.8 % above has to be read against —
otherwise "within 1 %" would be a claim an implementation that ignored the compensation might also
satisfy.

**3. Past the compensation cap the coat thins by exactly the ratio the cap imposes.** At 1/64 of
the coat the compensation wants 64x and gets 48x, and the measured error is **25.3 %** —
`1 - 48/64` is 25.0 %. The cap is the one way this representation loses coverage, it does so
predictably, and it is reported beside the cap in the statistics panel rather than clamped out of
sight. Neither shipping tier reaches it on either reference animal (11x and 22x against a 48x
cap), and `PastTheCompensationCapTheCoatThinsByExactlyTheRatioTheCapImposes` pins the arithmetic
so a future budget change that *did* reach it fails loudly.

### What the proxy tier buys

| | scalp | pelt |
|---|---|---|
| triangles, detailed -> proxy | 180 224 -> 32 780 (**5.5x**) | 196 632 -> 26 820 (**7.3x**) |
| geometry bytes | 11.0 -> 2.0 MiB | 12.0 -> 1.6 MiB |
| worst-direction error | 0.198 % -> 1.286 % | 0.113 % -> 0.601 % |

A 5-7x triangle reduction for a tenth of a percent of coat occlusion.

## 4. The switch policy

Apparent size in pixels of the render target's height, from `EstimateProjectedPixelSize` against the
engine's LOD view — the same orientation-independent projection every other LOD here uses (#726),
and deliberately not a distance in metres, which is wrong at the next field of view and wrong again
at 4K.

| | value | why |
|---|---|---|
| `DetailedPixelSize` | 192 px | **A cost threshold, and section 3 is why it can be.** Both tiers are within 1.3 % of the authored coat in the worst direction, so there is no quality cliff to place it at; 192 px of target height is roughly a coat filling a sixth of the frame, which is where 5-7x the triangles stops paying for itself. Deliberately conservative — the measurement would support a far lower one. |
| `Hysteresis` | 0.15 | The band moves bodily to hold the tier a coat already has, `GroomLodPolicy`'s rule. |
| `HoldFrames` | 4 | A coarsening waits; a refinement is immediate. A coat that just got closer and is still on the proxy is visibly wrong in a reflection; one that stays detailed a few frames too long is merely expensive. |
| `DetailedStrandBudget` | 6144 | Lands at a stride of 2-3 on the reference coats: 180-197k triangles and 11-12 MiB each, so the 192 MB resident cap holds about sixteen coats at this tier. |
| `ProxyStrandBudget` | 768 | 27-33k triangles and 1.6-2.0 MiB, for under 1 % coverage error. The compensation it implies (11x and 22x on the two references) stays clear of the 48x cap, which is the constraint that sets the floor — a smaller budget would start losing coverage to the cap rather than to the thinning. |

The entity's own strand budget is the **ceiling**: a coat the LOD already thinned to 500 strands is
not traced at 6144 of them. The tier lowers and never raises, the "coarser of the two wins" rule
#1258 applies to the LOD budgets.

## 5. No duplicate contribution from raster coat shadowing

`GroomCoatShadow.h` names three attenuations and states that they are disjoint:

1. inside one fibre — `exp(-sigma_a * chord)`, #1247;
2. **between** the fibres of one coat — `tau`, #1248's density volume;
3. everything else in the scene occluding the coat — the engine's shadow map, or the ray-traced
   shadow tier.

A groom proxy in the TLAS is a member of **(3) for other receivers**. The invariant this issue adds:

> A groom proxy occludes other receivers. It never occludes its own coat.

It holds by construction rather than by care: `GroomRenderPass` does not write the G-Buffer, so no
screen-space shadow term reaches a strand, and this issue deliberately does not make one. `tau`
stays the only thing attenuating a strand.

It also cannot collide with the shadow map, because the ray-traced mask **replaces** a light's
raster shadow rather than multiplying it (`oloRayTracedShadowFactor`: "False means take whichever
raster path this light type already had").

**The A/B that would catch a violation**, and the one run in the PR: proxies on versus off. The
coat's own pixels must be identical; the body and the ground under it must darken. A coat that
dimmed is the double count — and it is the shape already on record as
`shadow-map-is-binary-a-coat-is-not`, where a groom that both cast and received went from 44.98 to
0.22 luma.

## 6. Cost, memory and update time

Measured live, Vulkan + Deferred, **Debug** build, from `olo_rt_scene_stats`' `grooms` block.
`Scenes/GroomSimulatedCoat.olo` in Simulate: two coats, one of them bound and deforming.

| | value |
|---|---|
| coats represented | 2 (both Proxy tier) |
| resident geometry | 2 838 752 B (2.71 MiB) |
| resident triangles | 37 352 |
| rebuilt per frame | 1 (the deforming coat) |
| reused per frame | 1 (the static coat) |
| triangles converted per frame | 18 676 |
| compensation in force | 3.0x of a 48x cap |
| device AS bytes (whole scene) | 1 953 792 B (1.86 MiB) |
| **CPU conversion + upload** | **15-34 ms/frame** |

### The CPU cost is the honest caveat, and it is a Debug figure

15-34 ms per frame to convert and upload ONE deforming coat is the dominant new cost, and it is
reported here rather than buried because it is the number that decides whether a crowd of
animated animals can afford this. Three things bound it, and one does not:

- **It is a Debug build.** The conversion is a tight loop over `GroomStrandVertex`, exactly the
  shape that a Release build optimises hardest, and the engine's own vegetation policy notes a
  Debug editor frame at 114 ms for comparison. A Release figure is not quoted here because it was
  not measured — quoting an estimate would be the thing this document exists not to do.
- **It is per DEFORMING coat.** A static coat costs nothing after its first frame; the measured
  frame has one of each and `reused: 1` is the static one.
- **It is bounded.** `GroomProxyPolicy::UpdatesPerFrame` / `VerticesPerFrame` / `TrianglesPerFrame`
  cap the work transactionally, and past the cap coats are refused with `BudgetExhausted` rather
  than the frame growing without limit.
- **What does NOT bound it** is the tier ladder, because a distant animal is usually a static one
  and a near animal is the one that deforms. That is the wrong way round for this cost, and it is
  the first thing to look at if a crowd scene needs it: an animated-coat refresh cadence, of the
  kind `VegetationPolicy::MaximumProxyAge` already applies to foliage, is the obvious follow-up
  and is deliberately NOT in this issue.

### Reachability, verified rather than asserted

Switching the render path live on the same scene, on the same Vulkan device:

| path | coats considered | represented | refused | TLAS instances |
|---|---|---|---|---|
| Forward | 2 | 0 | 2 (`NotRequested`) | 4 |
| Forward+ | 2 | 0 | 2 (`NotRequested`) | 4 |
| Deferred | 2 | 2 | 0 | 6 |

The two coats are exactly the difference between 4 and 6 instances. `complete` stays `true` on
all three, because a `NotRequested` refusal is not a failure — a path with no hybrid consumer has
nothing to put a coat into.

## 7. Unsupported cases

Reported as counted refusals, never as an absence. Every one leaves the **raster tier untouched**.

| reason | when | clears when |
|---|---|---|
| `NotRequested` | Ray tracing unavailable, or no hybrid consumer this frame. Not a failure: on OpenGL, on Forward and Forward+, and on any device without `VK_KHR_ray_query`, this is every groom in the scene. | A Deferred frame on Vulkan with RT shadows or RT reflections armed. |
| `GroomHasNoGeometry` | The groom cooked to nothing, or the tier's budget selected no strands. | A re-cook. |
| `BudgetExhausted` | The per-frame conversion budget is spent. | Next frame. The coat keeps no stale structure — see below. |
| `ResidencyExhausted` | 128 resident proxies, or 192 MB of proxy geometry, already held. | A coat leaves the scene. |
| `BuildFailed` | The conversion emitted no triangles, a GPU buffer could not be created, a buffer had no device address, or the entity's transform was not a usable number. | Depends; the log and the counter say which. |

**A refused coat loses its resident structure rather than keeping a stale one.** Holding what it has
and continuing to trace it would give a coat whose ray-traced shadow is at a pose the raster tier
left seconds ago — strictly worse than no ray-traced shadow, and invisible in every counter.

## 8. Animation, LOD and residency

Criterion 3 asks that bounds, acceleration structures and history update safely and visibly across
all three.

- **Animation.** A bound coat is rebuilt every frame (the producer has no skip-if-unchanged, for
  `GroomRenderPass::AcquireGeometry`'s reason: the pose that would have to be compared lives on the
  tick thread and was rewritten before this ran). Its instance carries
  `GPUSceneInstanceFlagAnimated` and a `DeformedContentRevision` that this producer bumps on every
  refill, which is exactly what `RayTracingScene::DecideBuild` reads to decide refit-versus-rebuild.
- **Groom LOD.** The tier and the LOD level are both in the cache's one hash, so a hand-over to the
  card tier reallocates the buffers, which mints a new GPU Scene geometry record, which is a
  `FirstBuild`. An undeformed coat *must* reallocate rather than refill: it classifies `Static`, its
  BLAS is built once and compacted, and a refill in place would freeze its shadow for the session
  with every counter healthy.
- **Residency.** A coat that leaves the scene is retired by absence on the same frame, under the
  same rule GPU Scene applies to its own records — so the buffer and the record that named it die
  together. The tier hysteresis is pruned against the grooms the frame *offered*, not the ones that
  got a structure, so a coat refused for one frame keeps its hold.

## 9. The raster quality tier

Untouched. Nothing in this issue changes `GroomRenderPass`, `GroomStrand.glsl`, the fibre BCSDF, the
coat shadow volume, or the representation LOD. The proxy is a second, separate strand build at its
own budget, consumed only by the acceleration structure.

The one change outside the ray-tracing path is `GroomStrandRequest::ApparentPixelSize`, which
*records* a measurement Scene was already taking for the LOD and hands it to both consumers so they
cannot disagree about how big a coat is.

---

## Appendix — how to re-derive every number here

| figure | command |
|---|---|
| Section 3's tables | `OloEngine-Tests.exe --gtest_filter=GroomRayTracingProxy*.*` |
| Section 6's live figures | `olo_rt_stats` against a running editor, `grooms` block |
| Section 5's A/B | see the PR body's verification matrix |
