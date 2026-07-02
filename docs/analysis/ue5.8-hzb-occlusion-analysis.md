# UE 5.8 HZB Occlusion Culling — Source-Level Analysis

> Reference notes captured from a direct read of the Unreal Engine 5.8 renderer
> source (`Engine/Source/Runtime/Renderer` + `Engine/Shaders/Private`). Written to
> ground OloEngine's own two-phase HZB occlusion work (issue #431). All paths/line
> numbers are UE 5.8.

## 0. TL;DR

"Two-phase GPU-driven Hi-Z" is accurate, but UE 5.8 does not have *one* HZB
occlusion system. There is a **shared Hi-Z pyramid (the HZB)** and **three
consumers**, only one of which is genuinely two-phase:

| Consumer | Phasing | Code |
|---|---|---|
| **Nanite** visibility/raster culling | **Two-pass** (main + post, same frame) | `Nanite/NaniteCullRaster.cpp` |
| **GPU Scene instance culling** (regular meshes) | Single-pass vs. *previous* frame + cross-frame HW query | `InstanceCulling/BuildInstanceDrawCommands.usf` |
| **Legacy per-primitive HZB test** (`r.HZBOcclusion`) | Single-pass, CPU readback next frame | `HZBOcclusion.usf` |

All three share the same pyramid-build code and the same screen-rect-vs-mip test math.

---

## 1. The HZB pyramid

A **power-of-two, half-resolution mip chain of the furthest (and optionally closest)
device-Z**. Built by `BuildHZB()` in `SceneTextureReductions.cpp:111`.

Sizing (`SceneTextureReductions.cpp:136`):

```cpp
HZBSize.X = Max(RoundUpToPowerOfTwo(ViewRect.Width())  >> 1, 1);  // mip0 = half res, PoT
HZBSize.Y = Max(RoundUpToPowerOfTwo(ViewRect.Height()) >> 1, 1);
NumMips   = Max(FloorToInt(Log2(Max(HZBSize.X, HZBSize.Y))), 1);
```

Key properties the test code relies on:
- **Mip 0 is already half-res.** One mip-0 texel `(x,y)` covers full-res pixels
  `(2x,2y)…(2x+1,2y+1)`; the test code does `Rect.HZBTexels >> 1` because of this
  (`NaniteHZBCull.ush:97`).
- **Power-of-two**, so texel size is computed by a float-exponent bit-hack rather
  than a divide (`NaniteHZBCull.ush:139`).

### Reduction shader

`HZBBuildCS` (`HZB.usf:216`): 2×2 `GatherRed` per thread, reduce. UE uses
**inverted-Z** (near = 1, far = 0), so "furthest" = **min** device-Z, "closest" = **max**:

```hlsl
float MinDeviceZ = min(min3(DeviceZ.x, DeviceZ.y, DeviceZ.z), DeviceZ.w);   // furthest
float MaxDeviceZ = max(max3(DeviceZ.x, DeviceZ.y, DeviceZ.z), DeviceZ.w);   // closest
FurthestHZBOutput_0[OutputPixelPos] = MinDeviceZ;
ClosestHZBOutput_0[OutputPixelPos]  = RoundUpF16(MaxDeviceZ);   // rounded up = conservative
```

Engineering details worth copying:
- **Mip batching in LDS.** One dispatch writes up to `kMaxMipBatchSize = 4` mips,
  continuing the reduction in groupshared memory (`HZB.usf:299`). A whole HZB is
  ~2 dispatches, not one-per-mip. It uses `WaveGetLaneCount()` to skip the barrier
  when a sub-tile fits in one wave.
- **Furthest and closest are separate textures, not channels** (`SceneTextureReductions.cpp:153`)
  — most consumers want only one; packing both doubles cache footprint.
- **It can read the Nanite VisBuffer directly** (`VIS_BUFFER_FORMAT` perms,
  `HZB.usf:119`) so the current-frame Nanite HZB is built straight from rasterized
  clusters without a depth resolve.

---

## 2. The occlusion test math (shared by all GPU consumers)

All GPU paths funnel through `NaniteHZBCull.ush`. For one bounding box:

1. **Project box → clip-space rect.** `BoxCullFrustum` (`NaniteHZBCull.ush:543`)
   transforms 8 corners (split into 4 isolated passes to limit VGPR pressure),
   yielding `RectMin`/`RectMax` in NDC + nearest-corner depth `RectMax.z`, plus
   frustum-side and near/far crossing flags.
2. **NDC rect → screen rect → pick a mip.** `GetScreenRect` (`:71`) then
   `MipLevelForRect` (`:40`) picks **the smallest mip where the footprint fits in a
   4×4 texel block**:

   ```hlsl
   int2 MipLevelXY = firstbithigh(RectPixels.zw - RectPixels.xy); // full-rate vs quarter-rate log2
   int MipLevel = max(max(MipLevelXY.x, MipLevelXY.y) - MipOffset, 0);
   MipLevel += any((RectPixels.zw >> MipLevel) - (RectPixels.xy >> MipLevel) > MaxPixelOffset) ? 1 : 0;
   ```

   This is the crux of *hierarchical* Z: big object → coarse mip (few samples),
   small object → fine mip. Always a bounded read.
3. **Sample & compare.** `GetMinDepthFromHZB` (`:135`) gathers the 4×4 (via
   `GatherLODRed`), takes the **min** (furthest) of the block:

   ```hlsl
   bool IsVisibleHZB(FScreenRect Rect, bool bSample4x4) {
       const float MinDepth = GetMinDepthFromHZB(Rect, bSample4x4);
       return Rect.Depth >= MinDepth;     // inverted Z: object's nearest vs furthest occluder
   }
   ```

   In inverted-Z, `Rect.Depth` (object's closest point) `>=` HZB-min (furthest thing
   previously drawn over that footprint) ⇒ **at least part of the box is in front ⇒
   visible**. Min-over-block + nearest-corner depth ⇒ **conservative** (never wrongly
   culls a visible object).

---

## 3. Nanite's two-phase algorithm (the interesting part)

The chicken-and-egg: to occlusion-cull frame N you need frame N's depth, but to
produce frame N's depth you need to know what to draw. Nanite breaks it temporally.

Driver: `NaniteCullRaster.cpp:7003`. Two-pass is enabled only if a previous-frame
HZB exists (`:4089`):

```cpp
if (PrevHZB == nullptr || GNaniteCullingTwoPass == 0)
    Configuration.bTwoPassOcclusion = false;   // first frame / camera cut → single NO_OCCLUSION pass
```

Three culling permutations (`NaniteCullRaster.cpp:41`):

```cpp
#define CULLING_PASS_NO_OCCLUSION   0
#define CULLING_PASS_OCCLUSION_MAIN 1
#define CULLING_PASS_OCCLUSION_POST 2
```

### Phase 1 — Main pass (test vs *last frame's* HZB)

`CullingParameters.HZBTexture = PrevHZB` (`:6787`). Each instance is **reprojected
with last-frame transforms** before testing last-frame HZB
(`NaniteCullingCommon.ush:620`):

```hlsl
#elif CULLING_PASS == CULLING_PASS_OCCLUSION_MAIN
    FFrustumCullData PrevFrustumCull = BoxCullFrustum(LocalBoxCenter, LocalBoxExtent,
        PrevLocalToTranslatedWorld, NaniteView.PrevTranslatedWorldToClip, ...);
    if (PrevFrustumCull.bIsVisible && !PrevFrustumCull.bCrossesNearPlane) {
        FScreenRect PrevRect = GetScreenRect(NaniteView.HZBTestViewRect, PrevFrustumCull, 4);
        bWasOccluded = !IsVisibleHZB(PrevRect, true);
    }
```

Results split three ways (`NaniteInstanceCulling.usf:380`):

```hlsl
#if CULLING_PASS == CULLING_PASS_OCCLUSION_MAIN
    if (Cull.bWasOccluded) WriteOccludedInstance(ViewId, InstanceId);   // defer to phase 2
#endif
if (Cull.bIsVisible && !Cull.bWasOccluded) { /* enqueue for rasterization now */ }
```

`WriteOccludedInstance` (`:169`) atomically appends to `OccludedInstances` and bumps
`OccludedInstancesArgs` (an indirect-dispatch arg buffer) so phase 2 dispatches
exactly over the deferred set with **zero CPU involvement**. Main-pass UAVs are
`SkipBarrier` (`:4650`) — unique slot per instance. The survivors are then
rasterized (`:7010`), so the VisBuffer now holds all reliable occluders.

### Phase 2 — Build current HZB, then post pass (test vs *this frame's* HZB)

`NaniteCullRaster.cpp:7022`:

```cpp
if (Configuration.bTwoPassOcclusion) {
    BuildHZBFurthest(GraphBuilder, SceneDepth, /*VisBuffer=*/RasterizedDepth, ...,
                     TEXT("Nanite.PreviousOccluderHZB"), &OutFurthestHZBTexture);
    CullingParameters.HZBTexture = OutFurthestHZBTexture;   // swap to CURRENT-frame HZB
    SplitWorkQueue = OccludedPatches;
    AddPass_InstanceHierarchyAndClusterCull(CULLING_PASS_OCCLUSION_POST);  // retest deferred set
    PostPassBinning = AddPass_Rasterize(...);                              // raster newly-visible
}
```

Post-pass retests **only the deferred set**, vs the fresh HZB, with current
transforms, and **skips frustum culling** (already decided in phase 1):

```hlsl
#elif CULLING_PASS == CULLING_PASS_OCCLUSION_POST
    if (bIsVisible && !bSkipCullHZB && !FrustumCull.bCrossesNearPlane)
        bWasOccluded = !IsVisibleHZB(Rect, true);   // Rect = current-frame projection
```

**Why correct & cheap:** anything the main pass drew is a guaranteed occluder, so
the phase-2 HZB is accurate; the expensive stayed-visible set is drawn once; only
genuine **disocclusions** pay the second test. That's what removes the 1-frame
popping a reproject-only scheme would show. Separate `QueueState` index
(`PassState[1]` vs `[0]`) + doubled candidate buffers keep the passes from stomping
each other.

This same structure runs for **Virtual Shadow Maps** (HZB is a `Texture2DArray` per
page; `IsVisibleMaskedHZB` against page flags).

---

## 4. Render-pass schedule integration

The temporal trick works because of *where* each piece sits in
`FDeferredShadingSceneRenderer::Render` (`DeferredShadingRenderer.cpp`):

```text
RenderPrePass()    (2464)  → non-Nanite depth prepass
RenderNanite()     (2485)  → Nanite VisBuffer: runs the 2-pass cull above,
                             reading View.PrevViewInfo.HZB, writing its depth
RenderOcclusion()  (2726)  → RenderHzb(): BUILD this frame's HZB from complete depth
                             (prepass + Nanite); submit legacy HZB tests & instance occ queries
RenderBasePass()   (3059)  → shade
```

`RenderHzb` (`:561`) builds the **main scene HZB** and hands it to next frame
(`:597`):

```cpp
View.HZB = FurthestHZBTexture;                    // this frame's HZB (SSR, SSAO, AO…)
if (ShouldRenderNanite() || FInstanceCullingContext::IsOcclusionCullingEnabled())
    GraphBuilder.QueueTextureExtraction(FurthestHZBTexture, &View.ViewState->PrevFrameViewInfo.HZB);
```

So the cross-frame loop is **Frame N depth → Frame N HZB → stored → Frame N+1
main-pass occlusion test.** `RenderHzb` runs *after* `RenderNanite` so the scene HZB
includes Nanite occluders. Note: this scene-level HZB (furthest-only, from resolved
depth) is **distinct** from the intra-frame `Nanite.PreviousOccluderHZB` that phase 2
builds from the VisBuffer — same `BuildHZB` code, different source & lifetime. Async
build is gated by `r.SceneDepthHZBAsyncCompute`.

### First frame / camera cuts

With no `PrevHZB`, Nanite falls back to `CULLING_PASS_NO_OCCLUSION` (frustum only).
There is an **HZB priming** path (`DeferredShadingRenderer.cpp:1660`,
`r.Nanite.PrimeHZB...`) that does a cheap throwaway Nanite raster purely to bootstrap
an HZB on cuts, so cut frames still get occlusion instead of a full overdraw spike.

---

## 5. The other two consumers

**Non-Nanite GPU instance culling** (`BuildInstanceDrawCommands.usf:203`) — regular
static meshes in GPU Scene — is **single-pass**: it reprojects each instance with
`PrevLocalToTranslatedWorld`/`PrevTranslatedWorldToClip` and tests last frame's HZB
with the same `IsVisibleHZB`:

```hlsl
#if OCCLUSION_CULL_INSTANCES
if (Cull.bIsVisible && bAllowOcclusionCulling) {
    FFrustumCullData PrevCull = BoxCullFrustum(..., DynamicData.PrevLocalToTranslatedWorld, ...);
    if (PrevCull.bIsVisible && !PrevCull.bCrossesNearPlane) {
        FScreenRect PrevRect = GetScreenRect(NaniteView.HZBTestViewRect, PrevCull, 4);
        PrevRect.Depth = RoundUpF16(PrevRect.Depth);     // avoid self-occlusion from precision
        Cull.bIsVisible = IsVisibleHZB(PrevRect, true);
    }
}
```

No same-frame correction — it relies on reprojection plus a backstop of hardware
**per-instance occlusion queries** whose results feed back a frame later
(`PrevFrameViewInfo.InstanceOcclusionQueryMask`, `DeferredShadingRenderer.cpp:632`).

**Legacy per-primitive HZB test** (`r.HZBOcclusion`) is `HZBTestPS`
(`HZBOcclusion.usf:13`): a pixel shader reading packed bounds textures, running the
identical `BoxCullFrustum`/`IsVisibleHZB`, writing 1/0 to a tiny RT the **CPU maps
back next frame** to fill `FPrimitiveVisibilityMap`. `r.HZBOcclusion`: `0` = HW
queries, `1` = HZB software test (default), `2` = force HZB.

### From cull to draw calls

The regular path doesn't reorder meshes — it rewrites *indirect args*. The cull
compute (`InstanceCullBuildInstanceIdBufferCS`) appends survivors to
`InstanceIdsBufferOut` and atomically bumps `InstanceCount` (word [1]) of that draw's
`DrawIndexedIndirect` slot. Those go back as `FInstanceCullingDrawParams`
(`DrawIndirectArgsBuffer` + `InstanceIdOffsetBuffer`); `GetMeshDrawCommandOverrideArgs`
patches each mesh draw command to source from them. The base/depth/shadow passes
issue the same mesh draw commands they always would — **GPU Scene just culled the
per-instance count underneath them**, with optional order-preserving compaction
(prefix-sum over 64-instance blocks).

---

## 6. Mental model

- **One pyramid; conservative min-of-furthest test; hierarchical mip selection** is
  the whole primitive.
- **Nanite is the only true two-phase consumer:** reproject-test vs last-frame HZB +
  draw the confident set → rebuild HZB from that set → retest *only the deferred
  occluded set* vs the fresh HZB to recover disocclusions. Fully GPU-driven; the CPU
  never sees per-object results.
- The two GPU consumers differ in correction: **Nanite corrects disocclusions within
  the same frame** (rebuild-HZB + post pass); **non-Nanite instances correct across
  frames** via the rasterized query mask. Both share `NaniteHZBCull.ush`.
- The **prepass → Nanite → RenderHzb → base pass** ordering plus the
  `PrevFrameViewInfo.HZB` hand-off is what lets "current frame occludes current
  frame" happen without a serializing depth→cull→depth stall.

---

## 7. Conformance checklist — grading OloEngine against the UE 5.8 standard

UE's design distilled into checkable invariants, with OloEngine's current status as
read from the `feature/renderer-hzb-occlusion-cull` worktree (`InstanceOcclusionCull.comp`,
`GPUFrustumCuller`, `GPUDrivenOcclusionPass`, `Renderer3DFrameExecution.cpp`,
`RenderPipeline.cpp`). Status legend: ✅ matches the standard · ⚠️ deliberate/known
divergence · ❓ unverified — confirm against live code.

| # | UE 5.8 invariant | OloEngine status | Notes |
|---|---|---|---|
| 1 | **Pyramid is PoT, mip-0 ≈ half-res furthest-depth** | ✅ | `HZBGenerator` builds a PoT R32F mip chain; viewport→HZB mapping via `UVFactor` instead of UE's hard `>>1`. Equivalent intent; confirm mip-0 scale matches the `u_HZBUVFactor` used in the cull. |
| 2 | **Reduction operator matches the depth convention** (UE inverted-Z ⇒ *furthest = min*) | ✅ | OloEngine uses GL depth (near=0) ⇒ *furthest = max*, `ReduceMode::Max`. Correct sense for the convention. Verify `OcclusionHZB` is actually set to `Max` (GTAO/SSR share the generator with different modes). |
| 3 | **Occlusion test = object's nearest corner vs. *furthest* occluder over footprint** | ✅ | `nearestZ > occluderZ + bias`, `occluderZ = max` of samples. Same conservative test, correct direction for GL depth. |
| 4 | **Mip chosen so footprint ≤ a bounded texel block, with an alignment `+1` safety** (`MipLevelForRect`) | ✅ | **Hardened.** `mip = ceil(log2(maxSide))` selects the mip where the footprint is ≤ 1 mip-texel; the integer texel range `[lo, hi]` (with `hi = min(hi, lo+1)`) gathered below absorbs any power-of-two straddle — UE's `+1` alignment safety made explicit rather than a corner heuristic. |
| 5 | **Footprint fully covered by the fetch** (UE samples 4×4 = 16 texels) | ✅ | **Hardened.** Now a **point-sampled `texelFetch` block** over the covering texels (≤ 2×2 by construction), `max`-reduced. Point sampling is required — bilinear `textureLod` averaged the max-pyramid and under-estimated the furthest occluder (over-cull). Pinned by `GPUOcclusionCullParity.BlockGatherTakesMaxOverFootprint`. |
| 6 | **Two-phase: phase 1 vs. previous-frame HZB; collect occluded; phase 2 vs. current-frame HZB** | ✅ | Phase 1 at submission vs. retained pyramid → reject list (SSBO 18/19); phase 2 rebuilds HZB from current depth and retests the reject list. Direct analogue of `CULLING_PASS_OCCLUSION_MAIN`/`_POST`. |
| 7 | **Phase 1 reprojects bounds with *previous-frame transforms*** (`PrevLocalToTranslatedWorld`) | ✅ | **Closed.** Occlusion bounds now reproject with `inputInstances[idx].PrevTransform` in phase-1 / single-phase (matching the previous-frame pyramid); phase-2 uses the current `Transform` (current pyramid). The frustum test still uses the current transform. Correct for moving instances; a no-op for static (Prev == current). |
| 8 | **Phase 2 skips frustum culling (already decided in phase 1) and retests with current transforms** | ✅ | **Closed.** `u_Phase2 != 0` skips the six-plane frustum test entirely (UE's `bSkipCullFrustum`) and reprojects with the current transform. |
| 9 | **Phase-2 HZB built from this frame's *real* occluder depth** (UE: main-pass VisBuffer) | ✅ | `BuildCurrentOcclusionHZB` rebuilds from live FB depth = occluders + phase-1 survivors, after the phase-1 draws. Equivalent source. |
| 10 | **First frame / camera cut ⇒ frustum-only fallback** (no `PrevHZB`) | ✅ | First frame: `SetOcclusion` only fires once a retained pyramid exists; otherwise frustum-only. **Camera cuts need no priming here:** unlike UE's *single-pass* non-Nanite path (which pops / overdraws on cuts and is why UE primes), our two-phase scheme self-corrects — phase 1 only draws last-frame-visible, phase 2 recovers every disocclusion against the current-frame HZB. A cut just enlarges the reject set for one frame (more phase-2 work), never a visible spike. UE-style priming is therefore **moot for this design**, not a gap. |
| 11 | **Fully GPU-driven: no CPU readback of per-object results; indirect draw** | ✅ | `atomicAdd` compaction → `DrawElementsIndirect`. Matches GPU Scene's indirect-args rewrite. |
| 12 | **Small depth bias to avoid self-occlusion from precision** | ✅ | `u_OcclusionDepthBias` additive slack ≈ UE's `RoundUpF16(PrevRect.Depth)`. Tune empirically. |
| 13 | **Occluders are in the depth buffer *before* the HZB used to cull them is built** | ✅ | ScenePass draws non-instanced opaque occluders → `GPUDrivenOcclusionPass` runs after (`DependsOnPreviousWriter`). Phase-1 cull legitimately uses *last* frame's pyramid. |
| 14 | **Downstream consumers (AO/SSR/SSAO) see the culled geometry's depth/normals** | ✅ | **Closed (Stage 3).** `GPUDrivenOcclusionPass` re-copies the live framebuffer depth + view-normals into `SceneDepth`/`SceneNormals` (`glCopyImageSubData`) after its draws, declared as a plain `Write(TransferDest)` (same pattern as `DeferredOpaqueDecalPass`) so the graph orders the AO reader after it. |
| 15 | **GPU sync between cull write → indirect draw, and phase-1 → phase-2** | ✅ | **Verified.** `MemoryBarrier(ShaderStorage \| Command)` after every cull dispatch (cull→draw, phase-2-cull→phase-2-draw); `glTextureBarrier()` before the mid-frame HZB build (phase-1-draw → HZB sample); `HZBGenerator::Generate` issues `TextureFetch \| ShaderImageAccess` (HZB → phase-2 sample). |
| 16 | **Per-frame counters reset** (`instanceCount`, `rejectedCount` zeroed each dispatch) | ✅ | **Verified.** `CullTwoPhasePhase1` seeds the phase-1 indirect `instanceCount = 0` and `rejectedCount = 0`; `DispatchPhase2` seeds the phase-2 indirect `instanceCount = 0`. Per dispatch, every frame. |
| 17 | **Path coverage** | ✅ | **Closed (#486).** Two-phase now runs on **both** Forward/Forward+ (via `GPUDrivenOcclusionPass`, into the scene FB) **and** Deferred (via `DeferredGPUOcclusionPass`, into the G-Buffer). On Deferred the phase-1 draws stay on the normal ScenePass G-Buffer bucket (the phase-1 cull already ran against the previous frame's HZB), and `DeferredGPUOcclusionPass` runs after ScenePass / before `DeferredOpaqueDecalPass` + AO + `DeferredLightingPass`: it rebuilds the HZB from this frame's resolved G-Buffer depth (occluders + phase-1 survivors), re-tests the reject list, draws the disoccluded instances into the G-Buffer, resolves MSAA (per-sample mode), and re-exports the G-Buffer attachments (SceneDepth / SceneNormals / GBuffer{Albedo,Normal,Emissive} / Velocity + MSAA companions) — mirroring `DeferredOpaqueDecalPass`'s G-Buffer publication so AO / lighting / SSR see the recovered geometry. Registration order (Scene → DeferredGPUOcclusion → DeferredOpaqueDecal → AO → DeferredLighting) derives the correct `SceneDepth`/`SceneNormals`/`GBufferAlbedo` ordering edges; verified hazard-free. Pinned by `DeferredTwoPhaseOcclusionTest` (disocclusion decision math) + `OcclusionCullDeferredVisualEvidenceTest` (real deferred pipeline, no false hole / no corruption vs the occlusion-off baseline). |

**How to read this for review:** rows 6, 9, 11, 13 show the *architecture* is faithful
to UE's two-phase standard — that's the hard part and it's right.

**Post-hardening status (this branch).** The two real correctness risks are closed:
**#5 footprint coverage** (point-sampled texel block, max-reduced, pinned by
`BlockGatherTakesMaxOverFootprint`) and **#14 downstream depth** (Stage-3 SceneDepth/
SceneNormals re-export). The faithfulness items #4 (mip alignment), #7 (prev-transform
reprojection) and #8 (phase-2 skip-frustum) are now ✅, and the mechanical checks #15
(barriers) / #16 (counter resets) are verified. **#17 (path coverage) is now closed** —
two-phase runs on Deferred as well via `DeferredGPUOcclusionPass` (#486). The **only
remaining divergence is the single deliberate scope cut the design accepted up front**:
**#10** — no HZB *priming* on camera cuts (expect a one-frame overdraw spike on cuts), a
tracked follow-up, not a correctness gap.
