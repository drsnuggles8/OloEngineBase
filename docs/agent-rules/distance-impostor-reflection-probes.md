# Distance-impostor reflection probes (issue #705) — the contract, the seams, and the traps

Raymarched per-pixel probe reflections against baked radial-distance cubemaps
(Szirmay-Kalos et al., *Approximate Ray-Tracing on the GPU with Distance
Impostors*, Eurographics 2005 — re-derived for this codebase). Sibling of
[foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md); the
same "impostor" idea pointed at reflections instead of cards.

## 1. One contract file, three mirrors

The encoding contract — three linear radial distances per direction in world
units (front, the first occluder's back, what it hides; RGBA16F, #1804),
fixed `kProbeDistanceResolution` faces, far-plane-as-miss-sentinel,
MAX-downsampled mip chain, march budget — lives in
`OloEngine/Renderer/ReflectionProbeDistanceField.h` and is mirrored by:

- the bake (`ReflectionProbeBaker::CaptureDistanceField` +
  `ReflectionProbe_Distance.glsl`),
- the GLSL raymarch (`include/ReflectionProbes.glsl` — expression-for-
  expression twin of the CPU `RaymarchProbeDistanceField`),
- the contract tests (`ReflectionProbeDistanceFieldTest.cpp`, which also
  regex-pins the GLSL `#define`s against the C++ constants so the twins
  cannot drift silently).

Change any of the constants in one place only and the parity test fails —
that is the point.

## 2. The miss sentinel is a *hit* unless you check for it

The subtle correctness trap of the whole technique: the march declares a hit
at the first inside→outside transition of `|p| < dist(p/|p|)`. Sky texels
store the far plane, so an all-sky (or sentinel-dominated) probe still
produces a "crossing" when `|p|` finally exceeds ~1000 — a **plausible
far-plane hit that shades from the probe's stale sky** instead of missing to
the live sky. The fix is one extra sample: after refinement, a hit whose
stored distance is ≥ `kProbeDistanceMissThreshold` is reported as a MISS.
Both the CPU reference and the GLSL carry this check; pinned by
`ReflectionProbeRaymarch.AllSkyEnvironmentIsAMissNotAFarPlaneHit`.

Rays leaving through a *window* in an otherwise closed room miss for a
different reason: `dMax` (max **finite** stored distance) bounds the march at
`t ≤ |p0| + dMax`, which ends the march long before a sentinel-distance
crossing could happen.

The march bound itself carries a companion trap: the triangle-inequality
bound `t ≤ |p0| + dMax` holds for `|p| = dist`, but the crossing TEST is
`|p| > dist + bias` — the crossing sits a bias *past* the bound, and a
head-on ray across a room then never samples outside and silently misses
(no artifact, just "no reflection there"). Hence `kProbeMarchSlackRel/Abs`
on the bound; pinned by
`ReflectionProbeRaymarch.ConvergesToTheAnalyticHitAcrossASphereRoom`, which
failed on exactly this before the slack existed. Both bugs in this section
were caught by the analytic-room contract tests, not by looking at frames —
a raymarch that silently misses looks identical to "the probe doesn't cover
that pixel".

And the third trap needed the frames: a shading point the probe cannot SEE
(the far side of a captured occluder — e.g. the lower half of an object the
probe looks down on) starts the march already OUTSIDE the impostor surface,
inside the occluder's radial cone. Accepting that first outside sample as
the crossing shades the point with the occluder's shell — on the red-room
evidence scene it drew three dark crescents across the sphere's own
probe-hidden lower half, visually a "new shadow". The march therefore only
accepts a crossing AFTER it has seen an inside sample, skipping leading
outside samples until the ray exits the occluder cone and re-enters the
visible region (`ReflectionProbeRaymarch.OccludedStartSkipsTheOccluderAndHitsTheRealSurface`).
The cheap reject does NOT prevent this case: its max-mip is deliberately
over-admissive, and the occluded point sits within the margins whenever the
reject cone's footprint also covers geometry beyond the occluder.

## 3. Mips are MAX-downsampled, and only the cheap reject reads them

The reject asks "can the probe see this shading point at all": visible iff
`|x−o| ≤ dist + margin`. Sampling that test at a low mip is only sound if
the low mip is an UPPER bound of distance over the footprint — a max-filter.
A box-filtered (glGenerateTextureMipmap) chain under-reports distance at
silhouettes and the reject silently kills valid probes near depth edges.
So the chain is built on the CPU (`BuildNextMaxMip`) and uploaded per mip;
never call `GenerateMipmaps()` on the distance array. The raymarch itself
samples mip 0 exclusively.

## 4. The bake borrows the DDGI caster enumeration — via a sink, not a flag

The distance capture needs the scene's opaque casters without a DDGI volume
being active. `Renderer3D::SetAuxCasterSink` makes the existing
`SubmitDDGICasterIfCollecting` sites ALSO append to a caller-owned vector;
the baker installs it for exactly ONE render (the warm-up render that primes
the resized graph — scene mesh submission is registry-driven, not
CPU-frustum-culled, so one render sees every caster). Two rules:

- clear the sink with a scope guard (a throw inside `RenderScene3D` must not
  leave Renderer3D pointing at a dead vector);
- the sink must NOT feed the DDGI pass itself (gate the pass push on
  `WantsCasters()`), or bake-time geometry doubles up with the frame's own
  traversal.

Skinned meshes are absent from the caster list — probes receive but never
contain dynamic geometry, same receive-only rule as DDGI (ADR 0007).

## 5. Cube arrays: one resolution to rule them all

`samplerCubeArray` forces every layer to one size/format, which is why
`kProbeDistanceResolution` is a constant and not a per-probe setting, and why
the radiance array's spec is derived from the probes' *prefilter* maps
(always the same 128 RGBA32F because every bake uses the default
`IBLConfiguration`). A probe whose prefilter disagrees is skipped with a
warn-once, not resized. `glCopyImageSubData` copies prefilter → array layer
per mip and requires identical formats — that constraint is what pins the
array format to the prefilter's.

The per-cluster assignment is a u32 BITMASK per froxel (bit i = probe i
overlaps), not an offset/count list like lights — 32 probes max makes the
mask the simpler and raceless structure (`atomicOr` in shared memory, one
write per cluster). `ReflectionProbeCull.comp` mirrors LightCulling.comp's
frustum math on the same 32×18×24 grid; the probe UBO carries its OWN copy
of the slice parameters so probes keep working when Forward+ is off (mask
falls back to all-ones when the grid was not dispatched).

## 6. Shading integration: replace the prefiltered COLOR, not the BRDF

The seam in `PBRCommon.glsl` is the two `textureLod(prefilterMap, R, …)`
fetches. The probe blend replaces that prefiltered colour
(`mix(globalPrefiltered, probeBlend.rgb, coverage)`) and feeds it through
`calculateIBLPrefiltered` / `calculateCombinedAmbientPrefiltered` — the BRDF
split (F·envBRDF.x + envBRDF.y) is untouched, so probe pixels and sky pixels
stay photometrically consistent. SSR needs NO changes: it composites later
by lerping over the lit colour, so the deferred/forward ambient term IS the
SSR-miss fallback — the SSR → probe → sky ladder emerges from pass order.

`Scene::ApplyReflectionProbeOverride` keeps only its DIFFUSE half when a
global environment exists (`Renderer3D::OverrideGlobalIrradiance`); it still
swaps the full trio when there is no global IBL (indoor-only scenes need the
probe's BRDF LUT + prefilter for the IBL path to light at all). Overriding
the prefilter map with a global environment present would make the
probe-miss fallback the dominant probe's own radiance instead of the sky.

## 7. The samplerCubeArray slots are slot-based ON PURPOSE (bindless)

`include/ReflectionProbes.glsl` declares bindings 14/15 as plain
`layout(binding = N)` in every variant. That is the DDGI-atlas pattern:
`ReflectionProbeArray::BindForShading` publishes both slots through
`HeapBinding::PublishTextureOffsetAndBind`, which stages the heap offset AND
always issues a real bind — so the bindless-route deferred/forward shaders
and the slot-only MSAA variant all read the same binding without per-shader
`#ifdef OLO_BINDLESS` branches, and no `OLO_HEAP_TEX_CUBE_ARRAY` macro or
typed cube-array null had to be added to the heap.
`BindlessShaderPipelineTest::SlotAlwaysReceivesARealBind` records the two
slots as that exception.

## 8. Known limitations (documented, not bugs)

- The layers describe one occluder per direction (#1804). Two occluders in a
  row put the second one's gap back to solid, and densely self-occluding
  scenes (forests) still alias. Probe placement matters.
- Thin geometry between the shading point and the reflected surface can
  bracket on a silhouette texel; the refined hit lands on the discontinuity
  and smears. Inherent to the technique at reflection-quality budgets.
- The dominant-probe irradiance override is still per-camera (pop between
  probes for DIFFUSE); only the specular path is per-pixel.
- Terrain and water keep the global-IBL reflection source (their ladders
  were left untouched); wiring them is mechanical if ever needed.

## 9. Layered distances: read texel centres, inpaint hidden hits, swap the map (#1804)

**A ray point is solid in `[d0, d1]` or beyond `d2`; the gap between is free.**
With one distance per direction, everything radially behind an occluder read
as solid, and floor rays passing behind the corridor pillar hit its "shadow":
the reflection stretched 31 px. The bake peels the two extra layers
(`CaptureDistanceField`, three passes per face). `d2 = d1` means "nothing
behind", which keeps walls solid all the way out.

**Sample the distance array nearest**, set on the texture object
(`RenderCommand::SetTextureSampling` when `ReflectionProbeArray` creates it;
CPU `SampleLayersNearest`). A bilinear read across a silhouette blends the
occluder's layers with the wall's into a solid shell where neither surface is;
the stretch survived the layers until the reads were nearest. A per-bind
`RHI::SamplerDesc` is not enough: GL's slot route ignores it
(`RendererAPI::BindTexture`). Snapping each read to its texel centre in GLSL
works too, but doubled the probe's GL Forward cost.

**A hit on a hidden `d2` surface borrows radiance from a nearby direction at
the same distance, never from the occluder.** The probe's radiance in that
direction is the occluder's, and the last visible point on the ray is usually
a different surface (the corridor's mirror floor, which reflects the pillar).
The march searches rings of directions around the hit and takes the first
whose front distance is within tolerance of the hit's
(`kProbeInpaintRingCount` and friends); with none, it falls back to the
visible point stepped off the silhouette. Both are approximations: a
different surface at a similar distance passes the match, and the fallback
is the last visible side of the ray. Sky texels never match.

**`ReflectionProbeArray` keys uploaded layers by `EnvironmentMap`.** Calling
`SetProbeDistanceField` on a map already resident uploads nothing, and an
empty probe set keeps its slots, so deactivating a probe for a frame does not
help. A test that compares two fields gives each its own map
(`ReflectionProbePillarSilhouetteTest` bakes twice); every arm rendered the
first field until it did.
