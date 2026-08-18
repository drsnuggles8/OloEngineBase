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

**Vulkan status, stated precisely, because the imprecise version was wrong twice.**
VSM WORKS on Vulkan, confirmed two ways:

  * `VulkanPassSuite.VirtualShadowMapRunsAFullFrameOnVulkan` drives a real frame
    on a real device — allocator backs pages, raster leaves real depth in the
    pool, zero validation errors, zero unimplemented-facade hits.
  * BY EYE, live in the editor under `--rhi=vulkan` on the **forward** path:
    four poses of `VirtualShadowMapTest.olo`, CSM vs VSM, with the same
    qualitative wins as GL (the thin post casts, contact shadows attach, no
    detached crescents) and the shadows in the SAME places — which is what
    settles the y-flip composition below, a claim about two conventions
    cancelling that only a rendered frame could confirm.

THE CAVEAT, and it will bite the next person: the Vulkan editor's **deferred**
path renders a FROZEN frame — light edits (intensity, CastShadows) never reach
the screen, with VSM out of the picture entirely (issue #823). Verifying any
lighting feature live on Vulkan therefore means `renderpath=forward` first. A
CSM-vs-VSM comparison run on Vulkan-deferred returns byte-identical frames and
reads as "VSM does nothing"; it cost this task a full verification round before
the per-pose CSM==VSM hash check exposed it.

GETTING THERE COST TWO ENGINE FIXES, both invisible to every GL test:

  * `ImageFormat` had no `R32UI` member, so `CreateTexture2DHandle(R32UInt)`
    returned the NULL HANDLE on Vulkan. The whole physical pool did not exist:
    VSM initialised, reported its 91.8 MB, and every imageAtomicMin went nowhere.
    The only surface symptom was one warn-once line per shader —
    `'VSM_ClearDirtyPages' image binding 0 has no staged heap slot`. VSM is the
    first system in the engine to want an R32UI texture, which is why the hole
    sat there unnoticed.
  * VSM restored the previous viewport unconditionally, and `GetViewport()`
    answers `{0,0,0,0}` when nothing has set one — which is the normal case,
    since VSM runs at the top of the frame. GL shrugs; Vulkan raises
    VUID-VkViewport-width-01770 once per frame.

Neither could have been found by compiling, and neither could have been found on
GL. If you extend VSM, run the Vulkan test — it is the only thing in the suite
that executes this code on the other backend.

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

---

## 8. Local lights are a second addressing domain, and four invariants change shape

Issue #703 put point and spot lights on the same page table, the same physical
pool, the same allocator and the same eviction policy. The sharing is the whole
point, and it is also why the invariants above still apply — but four of them
apply *differently*, and each difference has already produced a wrong frame.

A local light owns **layers** (six for a point light's cube faces, one for a
spot) instead of a clip level, and a layer is **mipped**: mip m halves the face's
resolution, chosen per texel from the camera and light distances. So the mip is
to a layer what the clip level is to the directional map — with one extra
property that keeps §2 free: every mip of a layer shares ONE projection, so a
world point's stored depth is the same number at every mip and a fallback between
mips cannot draw a seam. There is deliberately no local `DepthRange` knob.

**§1 restated: the marker and the sampler must agree on the MIP.** Same failure,
same symptom — the sample lands on a page nobody backed, `vsmPageIsAllocated`
returns false, and the surface reads fully *lit*, which looks like the light's
settings rather than the shadow system. Both sides call
`vsmLocalMipForDistances`, both from the same `u_VSMCameraPosition`, and
`VirtualShadowMapLocal.TheMarkerAndTheSamplerCallOneMipHeuristic` asserts neither
side grew a copy. The marker marks the selected mip **and the next coarser one**,
for exactly the reason it marks two clip levels.

**§3 does NOT apply: layers do not wrap.** A clip frustum scrolls under its
cached pages; a light's face does not. It either stands still — and every page
stays valid — or the light moved, and the CPU raises that layer's invalidate
flag. Do not reach for toroidal addressing here; the whole `PrevPageOffset` /
`PageDelta` apparatus has no local counterpart.

**A perspective face cannot be culled the way an ortho level can.** The
directional cull projects an AABB's eight corners, divides by w and compares the
NDC extent against the unit cube — exact, because an ortho projection is affine.
A layer is a perspective, and a corner behind the near plane has `w <= 0`;
dividing by it mirrors the point through the origin and produces a confidently
wrong NDC. `VSM_CullLocalCasters.comp` therefore tests in **clip space against
the six frustum planes, with no divide**, and falls back to the full page grid
for the footprint when any corner is behind the plane. The symptom of getting
this wrong is a caster that vanishes *only when it gets close to the light*,
which reads as a range or attenuation bug. The same trap, with the same fix,
lives in `VSM_InvalidatePages.comp`'s local half.

**The face-UV test is neither a plain test nor a plain clamp,** and both simpler
versions are wrong somewhere:

- a plain range test breaks the **cube seams** — the face is picked by dominant
  axis, so a point on the boundary has a uv that floating point rounds to either
  side of 1.0, and rejecting it means no page, which means *lit*: a one-texel
  unshadowed line along all twelve cube edges;
- a plain clamp breaks **spot cones** — a spot has no face selection, so a point
  inside its range but outside its cone clamps onto the cone edge and marks
  pages for a whole sphere of radius `range`.

`vsmProjectIntoLocal` rejects anything outside by more than 1e-3 of a face and
clamps the rest. Both the marker and the sampler call it, which is what keeps
their answers identical.

### Three touch-points outside the page table that a local light needs

Each of these was missing at first and each produced a silent wrong frame:

- **`ShadowMap::AnyShadowsRequested()` needs the local light count.** With local
  lights on the page table, the atlas entry count is zero by construction — so a
  lamp-lit scene with no sun answered "no shadows requested", `ShadowRenderPass`
  was skipped whole, and nothing was ever allocated or rasterized. An unshadowed
  frame with nothing in the log.
- **`VSM_InvalidatePages.comp` needs a local half.** The cache keeps a lamp's
  pages until something dirties them, so without it a character walking under a
  lamp drags its shadow behind it — and that reads as an animation or
  transform-sync bug long before anyone suspects the shadow cache.
- **The meta table's owner fork must be in ONE function.** A meta entry now
  decodes as a clip-level owner or a layer owner, and three places turn one back
  into a page-table index (the allocator's eviction, the dirty clear, and any
  future consumer). `vsmMetaOwnerPageIndex` is that function. Applying the fork
  in one site and not another releases an unrelated page while leaving the real
  owner pointing at a physical page somebody else now holds — two wrong shadows
  from one mistake.

### Who still reads the atlas, and why that is not a bug list

With local lights on the page table, `u_AtlasEntryCount` is **zero by
construction** — so every remaining `if (entry < u_AtlasEntryCount)` is a dead
branch and its surface silently stops receiving local-light shadows. That makes
"which shaders consume local-light shadows" a question with a real answer, and it
is longer than it looks: the lit paths, the clustered evaluator, BOTH terrain
shaders (the voxel one has no Forward+ path at all, so it needs its own include),
DDGI probe relighting and the froxel fog scatter.

The lit and terrain paths are converted. Two are deliberately NOT:

- `DDGI_Relight.glsl` — probe irradiance receives local light unshadowed;
- `compute/FroxelFogScatter.comp` — light shafts lose local-light occlusion.

Both are compute passes that never call `VirtualShadowMap::BindForSampling` (its
only call sites are the forward draw path and `DeferredLightingPass`), so reading
the page table from them would rely on the VSM buffers happening to still be bound
from the shadow pass — true today, unverified, and precisely the ordering
dependency that breaks silently when someone reorders a frame. Give the pass its
own publish; do not assume the binding survived. Both sites carry the note in the
shader.

### A compute dispatch over (something x layers) can exceed the work-group limit

`GL_MAX_COMPUTE_WORK_GROUP_COUNT` is only guaranteed to be **65,535** per
dimension, and this is the first VSM dispatch whose bounds multiply: the local
invalidation runs over (dynamic casters x active layers), which at the documented
budgets is 1024 x 256 = 262,144. Flattened into X that is four times the
guarantee, and a conforming implementation REJECTS it — silently, in the sense
that the frame still renders and only that frame's local invalidations are lost,
so movers trail their shadows again on that hardware and nowhere else. The
development box (NVIDIA, ~2^31 per axis) cannot show it.

Dispatch it as 2D — one axis per budget — rather than flattening. The directional
kernels need no such care because their second factor is `kClipLevels` (16).

### The Vulkan y-flip is per-instance when the raster is mipped

§7's one-line fork becomes two, and the second one is easy to get wrong by
copying the first. The local raster puts each instance in a
`(LOCAL_RES >> mip)²` corner of a fixed mip-0 viewport, so the composition is
`fragCoord.y_vulkan = rasterRes - fragCoord.y_gl` against **that instance's**
resolution. Flipping about `VSM_LOCAL_VIRTUAL_RESOLUTION` instead is correct at
mip 0 and mirrors every coarser layer about a line outside its own footprint —
writing nothing at all, on Vulkan only.
`VirtualShadowMapVulkanShaders.TheLocalRasterStageFlipsAboutItsOwnMipResolution`
pins both halves.

### The known cost, so it is not rediscovered as a bug

The sub-rect trick bounds the *useful* area, not the *rasterized* one: a caster
projecting outside its face — a ground plane under a lamp is the everyday case,
since the plane runs to the face's horizon — still generates fragments across the
full mip-0 viewport, and they are killed by a bounds test in the fragment stage
rather than by the clipper. That is discarded-fragment cost, not memory traffic,
and it is paid only on frames where the layer actually has dirty pages. Bounding
it properly means one draw per (batch, mip) with a real viewport per mip, which
costs six indirect commands per batch — worth doing if a profile ever says so,
and not before.

### A bias that reads naturally either way will be inverted half the time

`LocalDetailBias` is documented — in the settings struct, in the GLSL, and in the
editor tooltip — as "> 1 pushes toward coarser (cheaper, blurrier) mips", exactly
like `ClipSelectionBias`. It shipped its first draft **dividing** by the bias
instead of multiplying, which inverted it: the slider sharpened where three
comments said it blurred, at a cost nobody would go looking for.

Nothing about the frame reveals it. The default is 1.0, where the two spellings
agree exactly, so every screenshot, every golden and every page counter is
identical. It only bites the person who later drags the slider to make a scene
cheaper and watches it get more expensive.

Two things worth copying for any similar knob:

- **Assert the DIRECTION, not just monotonicity.** `>=` held for the inverted
  formula because the high-bias probe saturated at mip 5.
- **Pick an operating point where nothing clamps**, and assert that too — the
  test now fails if its own coarse probe reaches the top of the range, because at
  that point it can no longer tell an inversion from a clamp.

### Two GLSL habits this issue punished

- **Do not spell one buffer block as two `#ifdef`'d copies.** The `VSMLocalLights`
  block was declared twice — a readonly flavour for the sampler and a coherent
  one for the kernels — and the two drifted by a member within the hour, which is
  a silent std430 layout change for whichever consumer took the shorter branch.
  Macro the qualifier and declare the members once.
- **`glslc` is the fast oracle.** Every shader in this system compiles in
  milliseconds through `glslc -fshader-stage=... -I include`, on both routes
  (`-DOLO_VULKAN=1`), including the `#type`-split graphics ones once you split
  them. It caught four real errors here before a single 30-minute build ran. See
  the memory note *validate-shaders-with-glslc-first*.
