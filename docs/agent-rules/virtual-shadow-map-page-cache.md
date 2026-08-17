# Virtual Shadow Maps: the page cache has four invariants, and breaking one is invisible

> §5 is the one to read first if VSM renders **no shadow at all** while every
> diagnostic says the system is up — it is a render-graph caching trap, not a VSM
> bug, and it applies to any pass with a runtime toggle.

Applies to: `OloEngine/src/OloEngine/Renderer/Shadow/VirtualShadowMap.{h,cpp}`,
`OloEditor/assets/shaders/include/VirtualShadow*.glsl`,
`OloEditor/assets/shaders/compute/VSM_*.comp`, `OloEditor/assets/shaders/VSM_Depth*.glsl`

Introduced with issue #702. Read this before touching the page table, the clip
projections, or anything that decides which clip level a world position belongs
to.

The system's whole value comes from **not redrawing pages**. Everything below is
a way that "not redrawing" quietly becomes "not redrawing the right thing" — and
the shared property of all four is that the frame still renders, the tests still
pass, and the shadows are wrong in a way that looks like a different bug.

---

## 1. The producer and the consumer must pick the SAME clip level

`VSM_MarkRequiredPages.comp` decides which page to request. The lit pass decides
which page to sample. If those two disagree by one level, the sample lands on a
page nobody asked to have backed, `vsmPageIsAllocated` returns false, and the
surface reads **fully lit**.

Not black — *lit*. So the symptom is "the shadow is missing here", which reads as
a culling bug, a bias bug, or a light-direction bug. It is none of those.

Both sides therefore call **one** function, `vsmClipLevelForDistance` in
`include/VirtualShadowCommon.glsl`, whose C++ twin is
`VirtualShadowMap::SelectClipLevel`. Do not inline the heuristic anywhere, and do
not "optimise" one caller's copy.

Two consequences that are easy to undo by accident:

- **The marker marks the selected level AND the next coarser one**, because the
  sampler falls back to coarser levels (see §4). A fallback level that is itself
  unmarked gets evicted, and the graceful blur becomes a hole.
- **`u_VSMCameraPosition` must be the same position on both sides.** The marker
  gets it from the mark pass; the lit pass gets it from the same globals block.
  Uploading a different one in either place shifts the concentric rings relative
  to each other, and the disagreement is worst at the ring boundaries — which
  looks exactly like a clip-level seam and sends you to §3 instead.

---

## 2. The depth range must not follow the camera

Every clip level is built with the same light-space depth range, anchored at the
**render origin** — never at the camera:

```cpp
glm::ortho(minX, minX + extent, minY, minY + extent, -depthRange, +depthRange) * lightView
```

Two separate things break the moment that range tracks the camera, and they look
like different bugs:

- **Cached pages become garbage.** A page drawn last frame stores a depth in the
  old range. Move the range and the stored number now means a different world
  distance, so every cached texel silently shifts. On screen: shadows that
  breathe or detach as you walk toward them. Nothing in the page table is
  corrupt — the *interpretation* is.
- **Clip levels stop agreeing, and that draws a seam.** "No visible seams at
  clip-level boundaries" is an acceptance criterion, and the property that
  delivers it is that levels differ *only* in XY extent. A fragment that switches
  level between frames, or between neighbouring pixels, must compare against the
  same stored number.

`VirtualShadowMap.AllClipLevelsAgreeOnTheDepthOfAWorldPoint` pins the second and
would catch most versions of the first.

The corollary: `VirtualShadowMapSettings::DepthRange` is a **scene-scale**
setting, not a quality dial. Changing it invalidates every cached page, which is
why `SetSettings` sets `m_FullInvalidate` when it moves.

---

## 3. Toroidal addressing needs BOTH offsets, not just the delta

A slot's owner is decided by the world page index modulo the table resolution, so
a page that stays put keeps its slot while the frustum scrolls underneath it.
`VSM_FreeWrappedPages.comp` has to release exactly the slots whose owner changed:

```
slot s held  originOld + ((s - originOld) mod RES)
slot s holds originNew + ((s - originNew) mod RES)
same page  <=>  0 <= r - d < RES,  where r = (s - originOld) mod RES, d = originNew - originOld
```

**The offsets stored in the UBO are taken modulo the table resolution**, so a move
of exactly one whole table width is indistinguishable from no move at all. The
kernel needs `PrevPageOffset` *and* the unwrapped `PageDelta`; deriving the
predicate from the current offset alone is a silent no-op for that case, and the
scrolled-in band inherits the outgoing pages' texels.

The failure is directional and slow: shadows correct in front of you, stale
behind, worst when the camera moves fast. Easy to blame on the one-frame marking
lag (§4), which it is not.

Two more traps in the same area:

- **Use a POSITIVE modulo.** Frustum origins go negative the moment the camera
  crosses the render origin, and C++'s `%` keeps the sign — which addresses the
  page table out of bounds rather than wrapping it. Pinned by
  `VirtualShadowMap.NegativeCameraCoordinatesStillProduceInBoundsPageOffsets`.
- **The HPB is reduced in WRAPPED space**, so `VSM_CullCasters.comp` must
  translate a caster's page footprint by `PageOffset` *before* the pyramid
  lookup. Testing an unwrapped rect against a wrapped pyramid reads other pages'
  dirty flags — a false negative there culls a caster that should have drawn, and
  the missing shadow moves as the camera does.

---

## 4. Marking runs one frame behind, and the fallback is the feature

`ShadowPass` is the first node in the graph, so the scene depth buffer that page
marking needs does not exist yet. Marking is therefore its own late node
(`VirtualShadowMapMarkPass`) and the next frame's shadow pass consumes what it
marked.

So a surface disoccluded *this* frame has no fine page. The sampler walks to
coarser clip levels — large, almost always resident — so the worst case is one
frame of blurrier shadow rather than none. **That fallback loop in
`vsmShadowFactor` is the anti-pop-in mechanism, not an error path.** Deleting it
as "dead code for a case that shouldn't happen" reintroduces exactly the pop-in
the issue set out to remove.

The ordering that makes the lag safe is spelled out in `VirtualShadowMap.h` and
is worth restating for the one step that is not obvious:

> `VSM_EndFrame.comp` clears `VISITED`, and it must run **after** the raster and
> **before** the marking pass. `VISITED` is the LRU signal `VSM_FindFreePages`
> reads; clear it after the marker instead and every resident page looks
> unreferenced, so the pool evicts itself completely every frame and nothing is
> ever cached.

That last one is worth measuring rather than reasoning about: the editor's
Renderer Settings panel shows `pages drawn / resident`. A static scene with a
still camera should settle to **`pages drawn` ≈ 0**. If it sits near
`pages resident` instead, the cache is not caching and the ordering is why.

---

## 5. A render-graph pass's `Setup()` is CACHED — it must not branch on a setting

This one is not really about VSM. It is about `RenderGraph::BuildFrameGraph`, and
VSM is simply the feature that found it. Read it before writing any pass that a
user can toggle at runtime.

`BuildFrameGraph(fingerprint)` returns early when the fingerprint matches the last
successful build and nothing marked the graph dirty. Everything a pass declares in
`Setup()` — its reads, its writes, any handle it stashes — is therefore computed on
the frame the topology was last *rebuilt*, and is then reused verbatim for every
frame after it. **The fingerprint does not include your pass's settings.**

`VirtualShadowMapMarkPass::Setup` originally opened with

```cpp
m_SceneDepth = {};
if (!VirtualShadowMapActive())
    return;              // <-- frozen in for the life of the topology
```

which is correct exactly once: on the first frame after the graph is rebuilt. Turn
VSM **on afterwards** — which is what the editor's checkbox does, and what any test
that renders CSM first and VSM second does — and `Setup` never runs again.
`m_SceneDepth` stays invalid, `Execute` early-outs on it every frame, no page is
ever marked, so nothing is ever allocated and nothing is ever rasterized.

The frame then renders **completely unshadowed, with no error anywhere**, and every
piece of evidence points somewhere else:

- `IsActive()` is true, the physical pool exists, the VRAM is allocated;
- the lit pass *does* enter the VSM branch (the debug tint proves it);
- the page table reads as "resident" wherever the sampler looks, because the
  residency view is showing you *last* topology's pages;
- and the same build produces a **perfect** shadow in any test that enables VSM
  before the first frame.

That last property is the tell, and it is worth naming because it is what makes
this bug so expensive: the feature works or does not work depending on **when the
setting was flipped relative to the last graph rebuild**, which is not a variable
anyone thinks to control. Two tests exercising the identical code path disagreed
for four rounds of debugging.

**The rule:** declare resources in `Setup()` unconditionally, and gate the *work*
in `Execute()`, which does run every frame. A read the pass may not use costs one
graph edge. The same reasoning already applies to `IsEnabled()` — it is consulted
at topology-build time, so it must answer "could this pass ever run" (here:
`m_ShadowMap != nullptr`), never "is the feature on right now".

The diagnostic that separates this from a page-table fault in one frame is
**DebugMode 7** (`Shadow factor`): it renders the number the lit pass actually
receives. A shadow visible there but absent from the beauty frame means the term is
computed and dropped by the caller; no shadow there means the page table or the
raster is at fault. Modes 4–6 (shadow test / stored depth / receiver depth) take
the comparison itself apart, but note they walk their *own* copy of the level
search — so mode 7 disagreeing with mode 4 is itself a finding.

---

## 6. Why the raster looks nothing like the CSM pass

Three deliberate departures, each of which reads as a mistake until you know why:

- **No depth attachment.** The texel a fragment belongs to is decided by a
  page-table lookup, and the rasterizer has already committed to a screen
  position by then. So the pass rasterizes into the clip level's full virtual
  viewport with *no* depth buffer and resolves visibility with `imageAtomicMin`
  into an R32UI pool. R32UI because `imageAtomicMin` has no float form;
  non-negative IEEE floats order as their bit patterns, so the raw bits are a
  valid comparison key.
- **A 4096² R8UI "raster scope" attachment that is never written.** GL needs a
  complete framebuffer to define a render area, and that area must be the
  *virtual* resolution. The physical pool cannot serve: it is a different,
  configurable size, and a smaller attachment silently clips three quarters of
  every clip level away. Draw buffers are `GL_NONE`.
- **One indirect draw per caster batch covers all sixteen clip levels.** The clip
  level travels in the compacted instance record rather than in a per-draw
  uniform, because sixteen draws per batch would dominate the pass's CPU cost.
  It is *not* done with `gl_BaseInstance` — that builtin is unavailable on one of
  the engine's two compile routes (see
  [glsl-shaders.md](glsl-shaders.md) §5c-bis), so the batch's run base travels in
  a uniform both routes can read.

---

## 7. What VSM does NOT cover yet

Static and skinned **mesh** casters only. Terrain, foliage, voxel and
virtualized-geometry casters still render through the CSM path, because each
needs its own VSM depth variant (the fragment stage is a shared include —
`VirtualShadowRasterStage.glsl` — so adding one is small, but the clip level has
to reach the vertex stage and those paths do not use the instance buffer).

`VirtualShadowMapSettings::Enabled` is therefore **off by default**, and a scene
that relies on those caster types must leave it off. This is not a soft
limitation you can ignore: with VSM on, a terrain-heavy scene renders the terrain
completely unshadowed, and it looks like the light is wrong rather than like a
missing feature.

**Vulkan status, stated precisely, because the imprecise version was wrong once.**
The Vulkan branches COMPILE — every VSM shader is built with `OLO_VULKAN` defined
for the Vulkan target env by
`VirtualShadowMapVulkanShaders.EveryVsmShaderCompilesForTheVulkanTarget`. VSM has
never RENDERED a Vulkan frame. Do not shorten that to "both backends are
supported"; the composition below is a claim about two conventions cancelling,
and only a rendered frame can settle it.

Why the test had to exist at all: `OLO_VULKAN` is defined in exactly two places
in the engine — `VulkanShader.cpp` and `VulkanComputeShader.cpp` — so an
`#ifdef OLO_VULKAN` branch is fed to a compiler ONLY when a live Vulkan backend
loads that shader. Nothing drives VSM on Vulkan, so for the whole of #702 those
branches had never been **parsed by anything**. A syntax error in them would have
shipped with the GL suite green. If you add an `OLO_VULKAN` branch to any shader,
assume it is unparsed until you can name the thing that parses it.

The whole fork is one line, and where it sits is the point:

- The clip projection is carried in **two flavours** —
  `ViewProjection` (raw, the math flavour) and `ViewProjectionRaster`
  (`RHI::AdjustProjectionForBackend`, the rasterizer flavour). Only the depth
  raster's `gl_Position` reads the second; the marker, the cull, the invalidator
  and the lit pass all read the first. On GL they are identical.
- `gl_FragCoord`'s origin is bottom-left on GL and top-left on Vulkan, and the
  raster flavour adds a y flip on Vulkan. The two compose to
  `fragCoord.y_vulkan = RES - fragCoord.y_gl`, which
  `VirtualShadowRasterStage.glsl` undoes under `#ifdef OLO_VULKAN`.

Undoing it *there* is what makes the physical pool's **contents** backend-
identical, so nothing downstream forks — not the page lookup, not the sampler,
not a golden image. The tempting alternative (let the pool mirror and flip the
SAMPLE instead, which is how the CSM path handles its own row flip) moves one
fork into three consumers, and each of them fails silently.

If you ever collapse the two matrices into one because "they're the same": they
are the same **on GL**, and the test that catches you is
`VirtualShadowMap.GPUStructLayoutsMatchTheirShaderTwins`, which asserts they sit
at different offsets.
