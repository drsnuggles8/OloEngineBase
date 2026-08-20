# Terrain virtual texturing — the invariants, and what breaks quietly

Applies to: `OloEngine/src/OloEngine/Terrain/VirtualTexture/`,
`OloEditor/assets/shaders/compute/TerrainVT*.comp`,
`OloEditor/assets/shaders/include/TerrainVirtualTexture.glsl`,
`include/TerrainLayerBlend.glsl`, `include/TerrainParamsBlock.glsl`.

Landed with issue #715 slice 1 (fixed grid, uncompressed cache, mip-chained indirection map) and
slice 2 (incremental indirection deltas — §4). Slices 3–4 (adaptive/variable-size virtual images,
BC-compressed cache tiles) are **not** here; read §8 before starting one.

**Read this before changing anything in the loop.** Every defect this subsystem can have is a
*wrong address*, and a wrong address does not look like an error — it looks like the terrain
having a slightly different texture than you expected. There is no frame in which it is obvious
that a page key packed the mip one bit off.

---

## 1. The loop, in the order it runs

All of it inside `TerrainVirtualTexture::Update()`, called once per frame from the terrain update
pass in `Scene.cpp`, **before** the terrain draws are submitted:

1. **Capture** — barrier, copy the feedback SSBO (which the *previous* frame's terrain fragments
   wrote) into the next readback ring slot, fence it, clear the SSBO.
2. **Poll** — any ring slot whose fence has *signalled* is read back and handed to a
   `Tasks::Launch` job that reduces it to a unique page-request list.
3. **Service** — the completed request list drives the page cache: touch what is resident,
   allocate (evicting via LRU) what is not, bounded by `MaxTileBakesPerFrame`.
4. **Bake** — one compute dispatch composites every newly-mapped page's splat blend straight into
   its physical cache tile.
5. **Publish** — when the resident set changed, stamp *this frame's changes* into the indirection
   texture (an eviction included, as an explicit all-zero entry) and re-propagate coarse→fine over
   the descendants of those changes. §4. The whole-map rebuild survives as the fallback for a map
   whose contents are not yet known.

Feedback written on frame *N* is captured on *N+1* and typically consumed on *N+2* or *N+3*. That
latency is the design, not a defect: it is what buys the loop a readback with no stall.

---

## 2. There is no `ClientWaitFence` in this class, and there must not be

The readback is the part the issue named as the thing to de-risk, and the engine has been bitten by
exactly this shape before (#707's DDGI relocation was a per-frame mid-frame readback stall).

What makes it non-blocking is **`IsFenceSignaled` — a poll, never a wait**. A slot the GPU has not
finished is simply left for a later frame; `glGetNamedBufferSubData` only ever runs on a slot whose
fence has already signalled, so it is a memcpy rather than an implicit `glFinish`.

Three ways to break this, all of which still produce a correct image:

- Swapping the poll for `ClientWaitFence(slot, someTimeout)`. Now the render thread waits for the
  GPU every frame, which is the stall the ring exists to avoid.
- Reading the feedback SSBO **directly** instead of the ring slot. The GPU is still writing it, so
  the read serialises against the terrain draws *and* decodes a half-written frame.
- Dropping the ring to one slot. Capture on *N* would have to be read on *N*, so the poll would
  never be signalled in time and the loop would silently never converge — the terrain stays at its
  coarsest mip forever and nothing logs anything.

A persistent mapping is **not** an option for the readback side: `AllocatePersistentUploadStorage`
maps `GL_MAP_WRITE_BIT` only (see `GPUCacheBacking`'s note), so reading through it is undefined.
`RHI::MemoryResidency::DeviceToHost` + `ReadBufferSubData` is the supported path.

**Two orderings in this loop are NOT the order you would assume, and both are guarded:**

- `PollReadback` walks the ring from `m_NextReadbackSlot`, not from index 0. That is the oldest
  slot still in flight, so the walk is in capture order; array order would hand the newest analysis
  to an older capture on a wrapped ring.
- `RetireAnalysis` adopts by **sequence number**, not by position. Analyses are launched in capture
  order but complete in whatever order the task scheduler finishes them, so a slow older one can
  land on a later frame than a fast newer one. Without the guard it overwrites the newer request
  list with a camera pose that has already moved on — a wasted frame of bake budget and stats
  counters that go backwards. A superseded analysis is still erased, just not adopted.

---

## 3. The coarse-mip fallback is the whole anti-pop mechanism, and it has THREE parts

Acceptance criterion 2 ("no visible page pop under normal movement") is not a nicety bolted on the
side. It is these three things, and removing any one of them turns a page arriving into a visible
flash rather than a gradual sharpening:

**(a) The indirection map has a real mip chain, and the fill kernel propagates down it.**
`TerrainVTIndirectionFill.comp` runs from the coarsest level to the finest, one dispatch per level
with a barrier between, and overwrites every texel whose alpha is 0 from the level above. Batching
those dispatches lets a level inherit a half-written parent. Writing alpha 255 on an *inherited*
entry stops the next finer level inheriting, one step short.

**(b) The page-local coordinate is evaluated at the RESIDENT page's mip, not the requested one.**
`oloVTVirtualToPhysicalUV` divides `PagesWide` by `exp2(ind.Mip)` — the mip that came *out of* the
indirection texel. Using the requested mip instead makes every fine lookup read the coarse page's
top-left corner, and the terrain shows a repeating stamp of one page. This is pinned by
`TerrainVirtualTexture.ACoarserResidentPageIsAddressedAtItsOwnMip`.

**(c) The chain must terminate in something resident.** `VTFeedbackAnalyzer::Analyze` inserts the
1×1 coarsest page *first, at the maximum count*, before any camera-driven request — including on a
frame where the terrain drew nothing at all. Without it the chain ends at an unmapped texel, which
reads as tile (0,0): whatever page happens to live there, shown across the whole terrain.

The analyzer also requests each wanted page's **parent**, so the entry a lookup falls back to while
the fine page is still baking is itself resident rather than two levels coarser.

`TerrainVirtualTexture::IsReadyForShading()` gates the shader's VT branch on the pinned page being
resident. Turning the branch on before that samples a zeroed cache, which is not "blurry" — it is
"wrong, plausibly".

---

## 4. Publishing is a DELTA, and three things keep it equal to the rebuild

Slice 1 republished the indirection map by rebuilding it: clear every mip, re-stamp every resident
page, re-propagate every level. Slice 2 replaced that with the reference's delta list
(`VTIndirectionDelta`), which writes only the texels that changed and re-propagates only their
descendants. The rebuild is still there — it is the only thing that can define a map whose contents
are *unknown* rather than merely stale — but it now runs on two occasions and no others: the first
publish after `Configure()` (texture storage starts undefined), and a delta that outgrew its upload
buffer. **Anything else reaching the rebuild is a bug in the delta**, and
`Stats::m_IndirectionFullRebuilds` is how you notice.

The delta is *equivalent* to the rebuild, not merely cheaper, and three rules are what make that
true. Each one breaks quietly:

**(a) An eviction is an ENTRY, not an absence.** With the clear pass gone, "page P is no longer
resident" has to be written as an explicit all-zero texel — byte-for-byte what the clear kernel used
to leave. A delta that only ever adds mappings leaves P's texel pointing at a physical tile some
other page now owns, and the terrain renders a patch of the wrong material rather than an error.
There are **two** sources of unmapping and only one of them notifies: the LRU eviction listener, and
`Invalidate()`, which calls `GPUPagedCache::DeallocateObject` — and *that does not fire the listener*.
`Invalidate()` therefore writes its own unmaps by hand, before it clears `m_Resident`.

**(b) The propagation still runs, over the DESCENDANTS of every change.** A texel that changed at mip
*m* invalidates a 2^k × 2^k block at every finer mip *m−k*, because any of those texels may have
inherited from it. `VTIndirectionDelta::GetFillRect` derives that with one coarse→fine walk: the rect
at level *l* is (level *l+1*'s rect, doubled) ∪ (the texels that changed at *l* itself). Skipping it,
or stopping one level short, reintroduces exactly the pop the mip chain exists to prevent — and it
presents as a streaming bug, not an indirection bug. The reference has this wrong in one place: its
`AVT_WriteMissingPixels` dispatch is guarded on `maxTouchedMip > 0`, so a frame whose only change is
at mip 0 never repairs an unmapped mip-0 texel.

**(c) One entry per texel, last write wins.** Two updates to the same texel inside one dispatch race,
and the loser is not predictable. That is the whole reason the reference's structure carries an index
map, and it is why the delta does too.

### The bounding box is most of the remaining cost, and that is the measured surprise

The fill rectangle is a **bounding box, not an exact set** — two changes at opposite corners of a
level cover it entirely. The degenerate case therefore costs what slice 1 cost unconditionally, so
the delta path can never be the slower one. What the arithmetic suggests, and what actually happens,
are not the same thing:

| 256 pages wide (the sandbox scene's config) | texels touched per publish | best GPU sample |
|---|---|---|
| whole-map rebuild | 174,905 (fixed) | 0.064 ms |
| delta | 17,053 – 32,763 | 0.029 ms |

Measured on an RTX 4090, Debug, by `TheDeltaPublishesTheSameFrameAsTheFullRebuild` with its map
temporarily raised to 256 pages; published on ~55% of frames. **~10× the texels of the ideal, because
LRU evictions scatter.** The pages arriving are camera-local, but the pages *leaving* are wherever the
least-recently-used ones happen to be, so the union of "what changed" is routinely spread across the
level and its bounding box is a large fraction of it. A delta whose fill is only ~5× cheaper than a
rebuild is doing what it was written to do; it is the box that is loose, not the delta.

Two things follow. The publish is **dispatch-bound at these sizes** — 0.064 ms for 175k texels on a
4090 is not bandwidth — so the win that did show up (2.2× at 32 pages wide, 2.3× at 256) comes from
issuing ~7 dispatches instead of ~18, not from writing fewer texels. And the obvious refinement,
tracking N rectangles per level instead of one, would trade those dispatches back: up to one per
change per level. **Do not "fix" the bounding box without measuring the dispatch count you are buying
it with** — at this scale it is very likely a regression, and the absolute numbers above (both well
under a tenth of a millisecond) are the reason this was left alone.

What slice 2 does buy unconditionally is that the cost stops being a function of
`VirtualPagesWide²` on every residency change — which is what makes slice 3's larger and
variable-size virtual images affordable at all.

**Two negative controls, because the equivalence test is worthless if it cannot fail.**
`TheDeltaProducesTheSameMapAsAFullRebuildOverRandomTraffic` drives both paths over a randomised
insert/evict sequence and compares the resulting maps texel-for-texel; on its own that proves nothing
about how sensitive the comparison is. `TheEquivalenceCheckFailsWhenTheDeltaOmitsItsUnmaps` and
`TheEquivalenceCheckFailsWhenThePropagationStopsOneLevelShort` deliberately publish a broken delta and
require the comparison to *reject* it. If you weaken either rule, those two are what tell you the
first test has stopped watching.

**And that whole set compares a CPU model of the three kernels, not the kernels.** Slice 2 changed
two of them, so the GPU half is
`TerrainVirtualTextureVisualEvidenceTest.TheDeltaPublishesTheSameFrameAsTheFullRebuild`: the same
scene published both ways, against a floor measured from a second delta-path run through the identical
invalidate-and-reconverge cycle. `OLO_TERRAIN_VT_FULL_REBUILD` is the lever it flips (settable at
runtime by name, from the editor console, `olo_cvar_set`, or the terrain panel's checkbox), and it is
the first thing to reach for when the terrain looks wrong: if forcing the rebuild fixes the frame, the
bug is in this section; if it does not, it is somewhere else entirely.

**The two kernels share one std430 header and neither declares it for the other.**
`TerrainVTIndirectionWrite.comp` and `TerrainVTIndirectionFill.comp` are separate programs reading the
same SSBO (binding 81), whose C++ twin is `VTIndirectionHeader`. A member added to one declaration and
not the other is a silent misalignment of `b_VTUpdates` — not a link error, because there is no link
between them. Three places, one layout.

## 5. Two servicing rules, and only one of them is the pin

`VTServicePageRequests` (in `TerrainVirtualTextureTypes.h`, so the headless test drives the real
function rather than a transcription) has two passes with one non-obvious rule each. **They do
different jobs, and conflating them is how the first version of this code shipped a pin that did not
hold.**

**Pass 2's allocation cap is the pin.** Allocations are bounded by `min(bakeBudget,
cacheTileCount - touchedThisFrame)`. Without that second term, a frame whose allocations outnumber
the untouched tiles walks the LRU order all the way round and evicts a page it touched moments
earlier — including the pinned coarsest page. The invariant it buys is worth stating plainly:

> **A page that is resident AND requested this frame is still resident after servicing.**

**Pass 1's reverse order is about retention, not the pin.** `LRUPolicy::OnAccess` moves to the front
and `TrySelectVictim` walks from the tail, so touching a priority-ordered list front-to-back leaves
the highest-priority page nearest the victim end. That matters once the camera moves on and the page
*stops* being requested: reversed, the pages that mattered most survive longest. It does nothing for
a page still being requested — enough allocations push anything to the tail, because every allocation
also moves to the front.

The distinction was found by a negative control, not by reading: with the cap in place, undoing the
reversal still passed the pin test. Two tests now cover the two rules separately —
`ServicingNeverEvictsAPageTheSameFrameAskedFor` and
`ThePinnedPageSurvivesACacheSmallerThanTheWorkingSet` for the cap,
`HighPriorityPagesOutliveLowPriorityOnesAfterTheCameraMovesOn` for the order. **If you change one
pass, run the other's test before assuming it still holds.**

`GPUPagedCache::Touch` was added by this work (#715) because #704's substrate had no way to record a
cache *hit*: `AllocatePages` **appends** the pages requested, so calling it to record a hit grows the
allocation every frame.

---

## 6. The bake↔shade coordinate mapping is an exact inverse, including the border

The shading side maps page-local `[0,1)` to physical texel `border + local * pageTexels`. The bake
therefore has to fill tile texel `j` with the content at page-local
`(j + 0.5 - border) / pageTexels` — which goes **negative** inside the border, and past 1 on the far
side. That is correct and load-bearing: the border holds the true neighbouring content, which is
what lets the cache be linearly filtered without bleeding across a page seam.

Two ways to get this subtly wrong, both of which produce faint seams rather than an obvious break:

- Growing the page's UV rect by the border and then mapping `[0,1)` across the *whole tile*. The
  interior then shrinks as the border grows, so the shading side's `border + local * pageTexels`
  addresses slightly the wrong texels.
- Clamping the sampled terrain UV to `[0,1]` for the *layer arrays*. Only the terrain's own maps
  (heightmap, splatmaps) clamp — they do not tile, so a border running off the terrain edge must
  clamp rather than wrap. The layer arrays tile by design.

`PhysicalUVLandsInsideTheOwningTileInterior` and `BorderedRectExtendsThePageByExactlyTheBorderDensity`
pin both.

**The physical cache has no mip chain, and must not get one.** A mip chain over an atlas blurs
neighbouring tiles into each other, which is exactly what the border exists to prevent. Minification
is handled by picking a coarser *page*, not a coarser texel.

---

## 7. Things that invalidate the cache, and are easy to forget

A baked tile is a function of the height field, the splatmaps, the layer arrays **and the terrain's
world transform** (the triplanar projection is world-anchored). Nothing about changing any of those
tells the cache its contents are stale, so each has to say so:

| Change | Where the `Invalidate()` lives |
|---|---|
| Material texture arrays rebuilt | `Scene.cpp`, in the `m_MaterialNeedsRebuild` block |
| Auto-material splatmap regenerated | `Scene.cpp`, in the `m_AutoSplatNeedsRebuild` block |
| Terrain entity transform moved | `TerrainVirtualTexture::Update`, via `MatricesDiffer` |
| Height field replaced (`Regenerate()`) | `Update()` runs unconditionally and clears `IsReadyForShading()` when the heightmap handle is momentarily invalid |

**`Update()` is called even when its inputs are missing, and that is deliberate.** It is the only
thing that clears `IsReadyForShading()`. Guarding the call site on "is the heightmap valid?" — the
obvious-looking shape — leaves last frame's `ready` standing for the tick or two a `Regenerate()`
takes, and the submit path goes on shading from tiles baked against the height field that was just
discarded. It also owns clearing the feedback buffer, which has to happen every frame whether or not
anything was baked.

**On the world-anchored bake and the floating origin:** the bake takes the terrain's ABSOLUTE model
matrix while the draw's `u_Model` is made render-relative by `UploadModelInstance`. That looks like a
disagreement and is not — the fragment path adds `u_RenderOrigin` back inside every
`triplanarSample*` before tiling (`Terrain_PBR.glsl`), so both sides project from the same absolute
position. Changing either half without the other is the way to break it.

A sculpt that rewrites the height field goes through the material/auto-splat path today. **If a
future sculpt path writes the height field without touching either flag, it needs a fourth entry
here** — the symptom would be terrain whose lighting follows the new shape while its albedo keeps
the old one, which reads as a shading bug rather than a cache bug.

`MatricesDiffer` is an explicit epsilon compare rather than `glm`'s `operator!=` — an exact float
comparison is forbidden here (`cpp-coding-quality.md` §2), and a last-bit difference re-baking the
whole cache would be a per-frame stutter.

---

## 8. The traps around it

- **`Update()` runs in the terrain update pass, NOT as a render-graph pass.** It branches on a
  runtime toggle, and a render-graph `Setup()` that does the same is frozen in by the frame-graph
  fingerprint cache — enabling VT after the first frame would silently do nothing. See
  [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) §5. Do not "tidy" this into a
  render-graph pass without reading that first.
- **No new UBO binding.** The UBO namespace has exactly one free slot under the GL 4.6 minimum of
  84, and this feature deliberately did not spend it: the two compute stages read their per-dispatch
  parameters from a fixed header at the front of their own SSBO, and the shading-side parameters ride
  the existing `UBO_TERRAIN` block as three appended `vec4`s. Keep it that way.
- **Adding a `TEX_*` slot is also a shader edit.** #715 added two, which moved
  `HEAP_IMAGE_SLOT_BASE` 68→70 *and* the heap-offset table 19→20 `uvec4`s. Both are hand-written
  literals in `include/BindlessHeap.glsl` and in `BindlessHeapGpuTest.cpp`'s inline copy. Pinned by
  `BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout`.
- **Everything binds through the `HeapBinding` seam, including the compute kernels.** For
  `Terrain_PBR.glsl` / `Terrain_GBuffer.glsl` that is required — they are on the bindless route, so a
  plain `BindTexture` records no offset and the sampler reads black (`glsl-shaders.md` §5c). For the
  four compute kernels it is the *sanctioned spelling* rather than a requirement: their declarations
  are slot-based, so the seam takes its fallback and issues a real bind, and the RHI boundary ratchet
  (`RHIBoundaryRatchet.RawGLOutsideTheBackendOnlyEverShrinks`) counts raw `RenderCommand::BindTexture`
  / `BindImageTexture` sites and will not let new ones in. **Bind the program before the resources**
  either way: the seam forks on the program *in flight*, so binding first would ask the question of
  whichever program happened to be bound last. There is no `FlushOffsets()` in the class because the
  fallback stages nothing — converting the kernels later means adding one per dispatch, and a
  per-iteration one in `PublishIndirection`'s loops.
- **`TerrainParams` (binding 10) is declared in exactly one place now.** It used to be hand-copied
  into thirteen — every stage of three terrain shaders, the four voxel shaders, and
  `TerrainGpuDrivenVertex.glsl`. `ShaderUBOSizeConsistency.CrossStageUBOLayoutAgreesWithinShader`
  pins every stage of one shader to the same block layout (glLinkProgram rejects a disagreement), so
  appending a member to twelve of thirteen copies is a link failure somewhere unobvious. Edit
  `include/TerrainParamsBlock.glsl`.
- **The splat blend now has three evaluators** — forward, deferred, and the tile bake — sharing
  `include/TerrainLayerBlend.glsl`. The *sampling* is deliberately not shared: the two fragment paths
  use implicit derivatives while the bake computes an explicit LOD from the page's texel density (it
  has no derivatives, and the explicit LOD is what makes a baked tile alias-free). The threshold
  constant `OLO_TERRAIN_TRIPLANAR_SLOPE` **is** shared: a tile baked planar and shaded as if
  triplanar shows a hard band exactly where the two disagree.

---

## 9. What slices 1–2 do not do

Stated so the next slice does not rediscover it as a bug:

- **The virtual image is a uniform grid** over one terrain's UV space. Per-region density needs the
  shared power-of-two atlas allocator tracked as **#718**, whose title already names VT as a
  consumer. Slice 3.
- **Cache tiles are uncompressed RGBA8.** BC compression needs the GPU compressor from item 3 of
  **#624**. Slice 4.
- **Streamed terrain keeps the splat path.** A streamed terrain's tiles each carry their own
  material and their own `[0,1]` UV space, so one uniform virtual image cannot address them. The gate
  is `!terrain.m_StreamingEnabled` in `Scene.cpp`, and the editor panel says so.
- **One indirection fetch per pixel, no trilinear blend across mips.** A page-mip transition is a
  density step rather than a cross-fade. The reference blends two fetches; that belongs with the
  adaptive slice, where the request list already carries both levels.
- **A floating-origin rebase invalidates the whole cache** (the transform moves), rather than
  re-projecting it. Correct, but it costs a full re-bake at the rebase.
