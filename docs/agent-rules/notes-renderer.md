# Subsystem notes — renderer, GL layer, shaders, offline capture

Accumulated gotchas from render-graph passes, the GL wrappers, SSAO/SSR/FSR1, IBL bakes and
offline capture. Reference notes, not failure postmortems — see [README.md](README.md).

Salvaged from worktree-scoped memory (see `docs/process/task-loop.md` Phase 7 for why that is now
the wrong destination).

---

## 1. Offline capture via `Scene::RenderScene3D` — five silently-black traps

Every one of these produces a black result that all CPU/contract tests still pass. Found and fixed
while adding the reflection-probe visual test.

1. **`RenderScene3D` renders into the render graph's own targets, never an externally-bound FBO.**
   Binding your own framebuffer and reading it back gives the cleared (black) buffer. Read via
   `Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor)` — RT0 is the HDR, linear,
   pre-tone-map, pre-TAA radiance, exactly what IBL convolution wants. `ToneMapColor`/`UIComposite`
   are the LDR terminal targets the editor viewport reads.
2. **The first `RenderScene3D` after `OnWindowResize(...)` returns BLACK.** The graph needs one
   throwaway frame to settle into the reallocated targets. To capture cube faces at a probe
   resolution: resize square, render **one warmup frame**, then capture, then restore the size.
   (`RunEditorFrames(cam, 2)` renders two frames for exactly this reason.)
3. **`IBLCache` is keyed by cubemap NAME + dimensions, not content.** A captured cubemap carries a
   constant debug name ("Generated Cubemap"), so its key collides across every bake at the same
   resolution and a hit serves a *previous* bake — even an all-black one. Runtime-varying cubemap
   sources must pass `IBLConfiguration{ .UseDiskCache = false }`. **This one masked the fixes for
   (1) and (2)**: the result stayed byte-identical black because a stale entry from the first broken
   run kept loading.
4. **Force render scale to 1.0 during capture — and restore it with RAII.** `RenderScene3D`
   honours `Renderer3D::SetRenderScale`, so with scale < 1.0 the scene renders into a
   `floor(width*scale)` sub-viewport while a full-texture readback reads the *whole* physical
   texture — you bake a tiny corner image surrounded by un-rendered pixels, with no crash and the
   right buffer size. The old self-bound-FBO path set its own viewport and was immune; the graph
   path is not.

   Save the previous scale, set 1.0, and **restore from a scope guard, not a trailing statement**.
   A capture body is long and has early returns (a failed face, an unresolvable target); a plain
   save-at-the-top / restore-at-the-bottom pair leaks the forced 1.0 into every later frame the
   moment one of them fires. `ReflectionProbeBaker::BakeProbe` currently does the plain form, with
   ~80 lines between set and restore — worth converting when you next touch it. The same guard
   should carry the window-size restoration, which has the identical problem.
5. **Exclude the probe being baked from its own IBL override.** `ApplyReflectionProbeOverride`
   installs the dominant active probe's `m_BakedEnvironment` as the global IBL, so a **re-bake**
   feeds the probe's own previous bake back into the fresh capture — an infinity-mirror artifact
   that compounds every time. Clear `m_Active` for the capture (RAII guard restores it on every
   exit). The first bake is safe on its own; this only bites re-bakes and the editor "Bake" button.

## 2. Drain `glGetError()` before checking it after a readback

A helper doing `glSomeReadback(); if (glGetError() != GL_NO_ERROR) return false;` **must first drain
any pre-existing error** (`for (int g = 0; g < 64 && glGetError() != GL_NO_ERROR; ++g) {}`), or an
error leaked by an unrelated earlier operation in the same context is misattributed and the helper
wrongly reports failure.

Real bug: `OpenGLTextureCubemap::GetFaceData` and `OpenGLTexture2D::GetData` didn't drain. A shadow
pass left a pending `GL_INVALID_OPERATION (0x502)`; the next `IBLPrecompute::ProjectCubemapToSH`
read saw the stale error, returned false, and surfaced as a **spurious all-black cubemap** in
`ProceduralSkyBakeTest` — only in the full suite, and it "self-healed" after one read because
`GetFaceData`'s own `glGetError()` consumed the leak.

> **When a GPU test fails only under suite ordering with an exactly-zero/black readback, suspect a
> leaked GL error misattributed by a readback — not a render bug.**
>
> The technique that cracked it: instrument the suspect path to dump the *inherited* GL state at
> entry and run the full suite. A `glReadPixels` diagnostic *masked* the bug (its `glGetError()`
> cleared the leak) while `glFinish` did **not** — that contrast pinpointed "stale error" rather
> than "timing". Measure inherited state instead of guessing.

## 3. Reassigning `m_RendererID` requires a full teardown of the old handle

Any path that recreates a GL object and reassigns `m_RendererID` (reload / `Invalidate` /
`Finalize`) must first release the previous handle, or every reload leaks the GPU object and
orphans every map keyed by the old id. For a live (non-zero) old id, before `glCreate*` overwrites
it:

- `glDelete*` the old handle **deferred** via `FrameResourceManager::Get().SubmitForDeletion(...)` —
  it may still be referenced by the in-flight frame's command buckets. Mirror the destructor.
- Balance the GPU memory tracker (keyed by the object `this` pointer) — re-`OLO_TRACK_GPU_ALLOC`
  without a matching `OLO_TRACK_DEALLOC` accretes into `m_TypeUsage` on every reload.
- Unregister the old id from every RendererID-keyed side map: `ShaderDebugger`
  (`OLO_SHADER_UNREGISTER`), `GPUResourceInspector::UnregisterResource`,
  `CommandDispatch::InvalidateTextureBinding`.
- Pair `OLO_SHADER_RELOAD_START`/`_END` on the **same (old)** id — START(old)/END(new) leaves the old
  entry stuck at `m_IsReloading = true`.

**Order matters as much as the steps.** Keep the existing handle *and every old-ID registration*
in place while you create and validate the replacement — do not unregister up-front. Only once the
new object is known good: switch `m_RendererID`, re-point the side maps to the new id, then defer
the old handle's teardown (with its matching tracker decrement and reload notification). On a
creation or recompilation failure, change nothing: the old object and all its mappings stay live and
working. Guard the whole teardown on `m_RendererID != 0` so first-time init is a no-op.

> Issue #544 found this independently in **two** wrappers (`OpenGLShader::FinalizeProgram`/`Reload`
> and `OpenGLTexture2D::InvalidateImpl`). Still latent and worth a follow-up:
> `ShaderResourceRegistry` is keyed by RendererID and is **not** re-pointed on reload —
> `ShaderLibrary::ReloadShaders` never re-runs `InitializeResourceRegistry`, so `Find(newId)` misses
> after a reload.

## 4. A raw GL id published into global renderer state must be reset every frame

Issue #505 looked diffuse — victims shifted between runs — but a stack capture showed **all 248
errors came from one bind site**. Raw ids published into `Renderer3D` global state
(`WaterSurfaceDepthTextureID`, `PlanarReflectionTextureID`) were cleared only inside the publishing
pass's `Execute()`, but the graph **culls** that pass on no-water frames, so the stale id survived
while churn deleted the owning framebuffer. Reset publications every frame in
`RenderPipeline::PrepareFrame`.

> **The reusable technique:** the GL debug context is synchronous in Debug, so a
> `std::stacktrace::current()` inside `OpenGLMessageCallback` gives file:line of the exact offending
> call — a multi-day diffuse hunt becomes one grep of the test log. That instrumentation is now
> permanent (ERROR-type messages only).
>
> Gotcha found on the way: **MSVC reports `__cplusplus` as 199711L** (the repo doesn't set
> `/Zc:__cplusplus`), so a `__cplusplus >= 202302L` gate silently disabled the stacktrace path —
> check `_MSVC_LANG` too. Also, `GLStateGuard::ApplyCore`'s restore path could itself bind deleted
> VAO/FBO/program names snapshotted at tick entry; it now validates with `glIs*` first.

## 5. Adding a shader binding is three edits, not one

Defining the `ShaderBindingLayout::UBO_*` / `TEX_*` constant is not enough.
`ShaderReflectionBinding.AllProductionShaderBindingsMatchCppLayout` reflects every production
shader and checks each binding against `IsKnownUBOBinding(binding, name)` /
`IsKnownTextureBinding(binding, name)` — `switch` statements matching the index **and** a name
substring pattern. A binding with no `case` fails with "binding N (name='…') is not recognised".

So: the constant, an `IsKnownUBOBinding` case, and an `IsKnownTextureBinding` case (as applicable)
whose pattern matches the GLSL block or sampler name. Inserting a `TEX_*` slot before
`TEX_SHADER_GRAPH_0` bumps that base — fine, all consumers use the constant.

`ShaderUBOSizeConsistencyTest` is gentler: it only checks blocks in its `kKnownBlocks` map, so a
pass-local UBO is simply skipped there.

## 6. There is no geometry shader stage

The pipeline (`#type` markers, shaderc→SPIR-V) supports only `vertex`, `fragment`, `tess_control`,
`tess_evaluation`. There are **zero geometry shaders** anywhere in the project.

Any feature wanting single-pass layered rendering via a geometry shader writing `gl_Layer`
(single-pass cubemap IBL convolution, single-pass point-light shadow cubemaps, layered VR) would
first need the compiler extended. `gl_Layer`-from-vertex-shader is the only alternative and needs
the non-core `ARB_shader_viewport_layer_array`. **Don't assume layered rendering is available.**

## 7. The IBL bake is serial, fragment-bound and cached — by design

`IBLPrecompute` bakes one pass per face. That is deliberate, not a TODO: the engine renders on a
**single GL 4.6 context** so faces can't be submitted from multiple threads; the bake is
**fragment-bound** (up to 2048 samples/texel) so even layered rendering would only cut CPU
draw/FBO overhead (and layered rendering isn't available — §6); and it is a **cold path**, gated
behind `IBLCache` and run once per unique (env, config).

A former `IBLConfiguration::EnableMultithreading` flag promised a parallel bake and was a permanent
no-op read by nothing. **Don't re-add a "fast/parallel IBL bake" knob** — there is no safe
single-context win. If bake latency ever matters, the lever is the SH irradiance path.

> Note `EnableMultithreading` and `UseDiskCache` are **not** hashed into the cache key.

## 8. Adding a pass that writes the G-Buffer after `SceneRenderPass`

- **Downstream consumers sample the *exported* graph textures, not the FBO.** ScenePass copies the
  live attachments into `Scene.SceneDepth`/`SceneNormals` and `GBuffer.{Albedo,Normal,Emissive,Velocity}`
  at the end of its Execute — *before* your pass draws. After adding geometry you **must re-copy**
  those (`glCopyImageSubData`, declared as `builder.Write(handle, RGWriteUsage::TransferDest)`) or
  AO / DeferredLighting / SSR won't see it. Unlike a decal pass, a geometry pass also changes depth
  and velocity, so re-export `SceneDepth` and single-sample `Velocity` too.
- **Ordering is by registration order** for same-resource `TransferDest` writers. Register between
  ScenePass and `DeferredOpaqueDecalPass`/AO. The "registration order changed derived dependency
  result" log lines are order-sensitivity *diagnostics*, not errors.
- **Declare exports unconditionally (handle-gated), never gated on per-frame work count.** Runtime
  toggles flip without forcing a graph rebuild, so Setup may not re-run on the flip frame and the
  pass would be left undeclared. Let Execute no-op instead — a no-op frame passes ScenePass's export
  through unchanged.
- **MSAA draw-target rule:** per-sample lighting with samples > 1 → draw into
  `GBuffer::GetFramebuffer()` (multisample; depth still holds occluders there) then
  `GBuffer::Resolve()`. Otherwise draw into `GetSamplingFramebuffer()`. Build any depth-derived
  resource from `GetDepthAttachmentID()` (resolved single-sample), and issue `glTextureBarrier()`
  before sampling depth you just wrote through the fixed-function pipeline.

> Pre-existing gap: instanced statics in Deferred still select `DefaultForwardShader` when the
> material has no explicit shader — `SubmitGPUCulledInstanced`/`DrawMeshInstanced` don't route to
> `PBRGBufferShader` like `DrawMesh` does — so instanced deferred geometry can shade oddly.

## 9. `RendererSettings` only reach the live graph through `ApplyRendererSettings()`

Nothing derives live state from the config automatically. With default settings (Forward +
`ForwardPlusAutoSwitch`), `ComputeSettingsDerivedDepthPrepass()` returns **true** while the live
`DepthPrepassEnabled` stays **false** until `Renderer3D::ApplyRendererSettings()` runs.

Call sites: the two settings panels, the `olo_renderer_settings_set` MCP tool, and
`EditorLayer::ApplyRendererSettingsToGraph()` at `OnAttach`, `NewScene`, and both scene-load
finalizers.

> **Two gotchas.** There are **two duplicate scene-load finalizers** — `LoadEditorSceneFile`
> (OpenScene / MCP / auto-save pre-answered) and `LoadSceneInternal` (auto-save recovery modal) —
> each independently copying scene settings into the live renderer. Any live-state push must be
> added to **both**. And **ordering matters**: the scene-load apply must run *after* the scene's
> `PostProcessSettings` are copied live, because `ApplyRendererSettings` rebuilds the graph on an
> `ActiveAOTechnique` change and the scene's technique must already be live or the rebuild is
> skipped.

`ApplyRendererSettings` is safe headless (graph rebuild is null-guarded, ForwardPlus setters no-op
until `Initialize()`).

## 10. Shadow pass: stale CSM matrices don't self-skip

`ShadowRenderPass` used to run at full GPU + ×4-cascade CPU cost with **no light casting shadows**
(70–90% of GPU time in zero-shadow scenes).

`Scene::RenderScene3D` only calls `ComputeCSMCascades` when `dirLight.m_CastShadows`, so with
shadows off the CSM matrices are **stale/identity** — but casters were still submitted (the gate was
the *global* toggle, not per-light) and `Execute()` only checked `IsEnabled()` plus non-empty caster
lists. **The per-cascade frustum-skip doesn't fire, because stale/identity matrices still produce a
frustum that intersects the casters.**

Fix: `ShadowMap::AnyShadowsRequested()`, reading the per-frame UBO flags (reset in `BeginFrame()`,
populated during shadow setup, all **before** the caster-submission loops). Both Scene-side gates
and `Execute` AND it in — Scene skips the CPU list build, and the Execute belt is the root-cause
guard so a caster leaking through any path can never paint a stale map.

> The visual test's shadow metric is the **per-pixel on-vs-off luma difference**, not an absolute
> luma band — a band mis-counts grey geometry and the far checkerboard floor as "shadow".

## 11. SSAO: horizon estimators self-occlude flat surfaces

The old horizon-based SSAO measured horizons in camera-elevation space **without subtracting the
surface tangent**, so flat lit ground self-occluded ~50% at any tilted or grazing view — the whole
floor dimmed at radius 0.5 and went pure black at 1.5. `AOApplyPass` multiplies AO into the whole
composited colour in HDR *before* ToneMap, so it survived tonemap as a large visible dim.

The replacement is a **normal-oriented hemisphere obscurance** estimator: a noise-rotated spiral of
taps in the screen-space radius disk, each tap's view-space position reconstructed from depth,
occlusion = how far the tap sits above the pixel's tangent plane past a cosine bias floor, with a
world-space range falloff and a proximity weight. Flat ground ≈ 1 (unoccluded), creases dark. GTAO
is a separate compute path, untouched.

**Assert AO correctness on the AO buffer via `SSAODebugView`, not the lit frame** — the lit
composite shows only a subtle delta once ToneMap compresses the HDR multiply.

> Shaders load at runtime from `OloEditor/assets/shaders/` **and** are SPIR-V cached under
> `assets/cache/shader/opengl/` — delete the cached binary after editing a `.glsl` or it is reused.
> No C++ rebuild is needed for a shader-only change (UBO params living in the test do need one).

## 12. SSR owns its own min-depth HZB

GTAO's HZB graph texture is declared only when GTAO is the active AO technique, but SSR must run
regardless of AO settings — so `SSRRenderPass` holds its own `HZBGenerator` in `ReduceMode::Min`,
generated from scene depth in Execute as an internal texture. `HZB.comp` gained a `u_ReduceOp`
uniform (0 = max/GTAO, 1 = min/SSR); the max path is byte-identical so GTAO is unchanged. A shared
dual-channel pyramid would force HZB generation whenever *either* is on.

**The traversal must reproduce v1's reflection, only faster — not "improve" it.** The marcher is a
screen-space DDA stepping the **view** parameter `t` (so depth is sampled at v1's granularity,
matching v1's grazing reflections) while sizing each empty-cell skip in **local screen pixels** so a
skip never overshoots a cell. Two pitfalls: a view-space depth-plane clamp can't progress the ray in
screen space → periodic banding; an exact-1px screen step (McGuire-Mara) under-samples depth at
grazing → loses the reflection band v1 shows.

Parity was confirmed by restoring the original shader via `git show HEAD:…` and diffing rendered
frames — identical, including v1's inherent grazing stipple.

## 13. FSR1: three port bugs and the reduced-size-target wiring

Porting EASU/RCAS to GLSL:

1. **RCAS luma must be normalised to [0,1]** (`0.5*g + 0.25*(r+b)`), not FSR's "luma×2". The
   peak-range limiter constant `(1, -4)` assumes a ceiling of 1.0; with luma up to 2.0 the limiter
   refuses to sharpen anything brighter than mid-grey. Symptom: sharpening silently disappears on
   bright regions.
2. **The `4*mn-4` contrast-headroom denominator is SIGNED** — negative below mid-grey, positive
   above — so it needs a *sign-preserving* guard. A naive `min(x, -1e-6)` inverts the sharpen on
   everything brighter than mid-grey.
3. **EASU on HDR pre-tonemap misbehaves.** Its edge analysis and negative Lanczos lobe assume
   perceptual [0,1] input, so bright highlights dominate the direction estimate and ring hard. Wrap
   the kernel in a reversible per-channel proxy `t = c/(1+c)` ↔ `c = t/(1-t)`. Dering clamps to the
   local 2×2 envelope (HDR-safe), **not** a `[0,1]` saturate.

The upscale is implemented via **reduced-size targets**, not the DRS "viewport-into-corner" route:
DRS is broken for the deferred path (`DeferredLightingPass`/`SSAORenderPass` set viewport from their
full-size spec and ignore the override; graph-owned FB passes are never reached by
`ApplyRenderViewport`), and making it work needs ~15 scene-path passes made render-scale-aware.

> **Two wiring gotchas.** `ComputeBlackboardFingerprint` must hash `data.PostProcess.Upscale`, or
> toggling upscale never re-declares `EASUColor` / re-sizes the scene band. And EASU's input
> candidate list must **not** include `PostProcessColor` — that alias points at `EASUColor` once
> EASU runs, so EASU would read its own output.
>
> Depth and velocity used to stay reduced, so full-res post passes reading them (DOF, Fog, TAA) got
> bilinear-upscaled reduced depth. `DepthVelocityUpscalePass` (#480) fixed that with a NEAREST
> upscale into `UpscaledDepthVelocity` — see the attachment-drop trap below before trusting it.

**The MRT that came back one attachment short (#772).** `UpscaledDepthVelocity` is declared
`{R32Float depth, RG16Float velocity}` and the pass writes both, but `FramebufferTextureFormat` had
no single-channel float colour member, so `RenderGraph::ToFramebufferFormat` answered `None` for
`R32Float`. The materializer's MRT loop *skips* an unrepresentable attachment rather than failing —
which **re-indexes** every survivor. The pooled framebuffer came back with ONE attachment (the
RG16F velocity, now at index 0), the depth write landed on no attachment at all, and
`CreateFramebufferAttachmentView(…, 0)` — published as `Post.UpscaledSceneDepthTexture` — handed
DOF / Fog / MotionBlur / TAA / ToneMap the **velocity** texture as their depth. Silent on both
backends; only the Vulkan port noticed, and only because its validation layer names unused fragment
outputs. Generalise: **an attachment list is not a set — a dropped entry moves every later index**,
so partial representability is never a safe degradation. The planner now refuses the whole
descriptor (`IsAllocatable` uses `all_of`, not `any_of`) so the pass fails to resolve loudly.

**Fixed, was open:** switching `PostProcessSettings::Upscale` to a non-`Off` mode at runtime used to
produce a fully black frame plus a flood of `GL_INVALID_VALUE … y values exceeds the boundaries`
(id 1281) — GTAO's scratch targets were declared at display res while `AOBuffer` shrank with the
scene band. Fixed in **#504** (every GTAO scratch resource now tracks `sceneBandWidth/Height`). The
*second*, quieter half of the same transition is **#771** — see
[render-pipeline-caches.md](render-pipeline-caches.md).

## 13b. FSR2 temporal upscaling (#684) — what is different from FSR1, and where it hides

FSR2 occupies **exactly the EASU slot**: it turns the reduced-resolution pre-Bloom HDR colour into a
display-resolution one, early, before Bloom/DOF/ToneMap. Everything in §13 about reduced-size
targets, the fingerprint hash and the "must not list `PostProcessColor` as an input" rule applies
unchanged. `FSR2Color` is the Temporal spelling of `EASUColor` — **exactly one of the two is ever
declared in a frame**, and `Bloom`'s candidate list carries both so name resolution finds whichever
ran.

**The build is Windows-only and that is structural, not laziness.** `cmake/fsr2.cmake` fetches
JuanDiegoMontoya/FidelityFX-FSR2-OpenGL and forces `OLO_WITH_FSR2` OFF everywhere else, because (a)
`ffx_fsr2_gl.cpp` uses `wcstombs_s` / `GetModuleHandleA` / `<Windows.h>`, and (b) the shader
permutations are compiled by `tools/sc/FidelityFX_SC.exe`, a **prebuilt Win32 binary** with no Linux
build. `Platform/OpenGL/OpenGLTemporalUpscaler.cpp` self-guards to a stub, so the pass, its settings
and its policy still compile (and are still tested) on every platform. Two traps in that CMake:
`GIT_SUBMODULES ""` is load-bearing — upstream registers a **multi-gigabyte** `cauldron-media` art
repo — and `SOURCE_SUBDIR` points at a nonexistent directory on purpose, so `MakeAvailable`
populates without configuring upstream's DX12/Vulkan sample apps.

**The failure modes are all "plausible image", which is why the rules live in
`Renderer/Upscaling/TemporalUpscalePolicy.h` as pure functions with a test (`FSR2PolicyTest`) rather
than inline at the call sites.** In rough order of how long each would survive review:

1. **Jitter divided by the display extent instead of the render extent.** The scene renders into a
   viewport of `renderW × renderH`, so one rendered pixel is `2 / renderW` in NDC. Use the display
   width and the jitter shrinks in proportion to the render scale — sub-pixel coverage collapses,
   every accumulated frame sampled the same geometric point, and FSR2 degrades into a blurry
   bilinear upscale **that still looks like a working temporal upscaler**.
2. **Motion-vector scale sign.** The G-Buffer writes `o_Velocity = (ndcCurr - ndcPrev) * 0.5` —
   UV-space, current-minus-previous. FSR2 wants render-resolution **pixels pointing back** to the
   previous position, so the scale is `(-renderW, -renderH)`. A positive scale reprojects the wrong
   way and every moving object trails; the still frame is fine.
3. **`frameTimeDelta` is MILLISECONDS.** It drives lock decay. Passing seconds makes locks expire
   ~1000× too slowly, which reads as heavy ghosting.
4. **`deviceDepthNegativeOneToOne` must be `false` here — and that is not a bug.** The engine's
   projections *are* GL's `[-1, 1]` NDC ones, so the flag looks like it should be true. But FSR2
   samples the depth **texture**, and a GL depth attachment stores WINDOW depth, which
   `glDepthRange`'s default maps as `z_window = 0.5 * z_ndc + 0.5` — precisely the affine remap that
   separates the GL and D3D projection matrices in z. The texture therefore already holds the
   `[0, 1]` device depth FSR2 assumes. The flag would describe a pipeline whose depth *buffer* holds
   `[-1, 1]`, which nothing here does; and this fork rejects it outright
   (`FFX_ASSERT_MESSAGE(params->deviceDepthNegativeOneToOne == false, "OpenGL depth convention not
   yet supported")`), so passing true is a debug assert and a silently wrong transform in release.
5. **The jitter sequence is the UPSCALER's, not the engine's.** FSR2 derives its phase count from
   the render/display ratio (a 67% scale needs ~2.2× the phases of native). Feeding it the engine's
   fixed Halton-16 TAA sequence under-samples exactly the reconstruction it exists to perform.

**Two things must be forced off while it runs, and both are enable-site decisions in
`RenderPipeline`, not settings mutations:** engine **TAA** (running both means TAA resolves an
already-resolved, already-upscaled image whose velocity buffer describes the pre-upscale frame) and
the late **`UpscalerRenderPass`** RCAS (FSR2 runs its own RCAS on HDR before tone mapping — sharpening
again post-tonemap is a second pass over the same edges and rings on high-contrast silhouettes). The
user's `TAAEnabled` / `CASEnabled` settings are left untouched so they return when the technique
changes.

### A third-party GPU dispatch owns the binding namespace it writes to — FSR2's 36% darkening

**Closed.** The symptom was a frame that lost ~36% of its luminance the moment `UpscalerTechnique::Temporal`
was selected, and the cause was **not in FSR2 at all**. Recorded in full because the false trail cost
two rounds of work and every wrong turn is reproducible.

**The mechanism.** FSR2's GL backend binds its own UBOs, textures, samplers and images to *fixed* slot
indices taken straight from the DirectX register numbers in the shader permutations, and never restores
them. Those indices share a namespace with the engine's. `FSR2_BIND_CB_*` resolves to **3, 4, 5, 11, 12,
14 and 18**; `PBR_MultiLight.glsl` has `MultiLightBuffer` at **5** and `ShadowData` at **6**. The
compute-luminance-pyramid pass's `glBindBufferRange(GL_UNIFORM_BUFFER, 5, ...)` lands exactly on the light
UBO. Because **the engine binds those UBOs once rather than per frame**, a single dispatch unlights every
later frame: the forward shader reads FSR2's constants as its light array and every lit surface collapses
to ambient. Fixed by `FSR2BindingScope` in `Platform/OpenGL/OpenGLTemporalUpscaler.cpp`, which captures and
restores the slots FSR2 can touch around the dispatch.

**Why it reads as a temporal-accumulation bug, and why that is the trap.** Frame 0 is *pixel-correct*
(nothing has dispatched yet) and every frame after is *uniformly* darker by a constant factor, flat
forever. That is precisely the signature of a bad history blend, so the whole first two rounds went into
FSR2's accumulate path. **Every one of these was measured inert, and none of them was ever the problem**:
auto-exposure on/off and with an explicit neutral 1.0 exposure texture, RCAS on/off,
`MOTION_VECTORS_JITTER_CANCELLATION`, the jitter Y sign, the projection-jitter sign, a pinned jitter phase,
`DEPTH_INVERTED`, `HIGH_DYNAMIC_RANGE`, `maxRenderSize` display-vs-render, forward vs deferred,
`RectifyHistory` skipped entirely, the reactive factor zeroed, `fKernelBias` forced to its frame-0 value,
`UPSAMPLE_USE_LANCZOS_TYPE` 2 → 0, and forcing **1:1** with no upsampling at all.

**The four measurements that actually found it.** Each one is cheap; the ordering is the lesson.

1. **Bypass the component under suspicion entirely.** A shader probe that dropped *all* of FSR2's temporal
   work and emitted its own *input* by nearest-neighbour still read **73.3** against a native **115.7**.
   That alone exonerates every line of FSR2 — but only because the probe read the input, not the output.
2. **Build the control you have been assuming.** A spatial-upscaler capture at the *same* 0.667 render
   scale read **116.7**. (The MSAA-fallback frame *looks* like a spatial control and is equally dark — it
   is not one, because MSAA-on-deferred is independently broken here. A control has to be clean.)
3. **Measure the engine's own buffer, with no upscaler in the measurement path.** Reading `SceneColor`
   (683×512) directly gave spatial **65.5** vs temporal **31.9**, and the PNG showed the shape: unlit grid
   lines and the clear colour untouched, every *lit* surface crushed. "Lighting is missing", not
   "an upscaler is dark".
4. **Swap the order.** With the temporal capture moved *before* the spatial one, the **spatial** frame came
   out dark too (116.7 → 74.2). The corruption follows the **dispatch**, not the technique. That is the
   measurement that names the bug class, and it is the one the regression test encodes.

**The general rule.** A third-party GPU dispatch that binds by hard-coded slot index is not a pass — it is
a second tenant in the binding namespace, and it will not clean up. Wrap any such dispatch in a
capture/restore of the slots it can touch, sized from the ranges its shaders actually declare and **clamped
to the driver's reported maxima** (`GL_MAX_IMAGE_UNITS` is 8 on plenty of drivers, well under FSR2's 18;
querying past it is `GL_INVALID_VALUE`, and an empty image slot reports format 0, which
`glBindImageTexture` also rejects). `GLStateGuard` is *not* this tool and says so: it documents that
per-slot texture and UBO bindings are deliberately **not** restored, because for an ordinary engine pass
that is ~185 GL calls of pure overhead. The trade-off inverts for a once-a-frame foreign dispatch.

**Guarded by** `FSR2VisualEvidenceTest.DispatchLeavesEngineBindingsIntactForLaterFrames`, which renders
**no FSR2 frame at the point it measures** — it compares a native frame captured before the dispatch with
an identical one captured after. Any future state FSR2 leaks lands there too. Verified to fail (115.7 vs
74.6) with the restore scope removed; a guard that has never been seen to fail is not a guard.

**FSR2's GL sampler/texture units are its macro binding PLUS 8, and that is the whole reason the first
restore missed them.** `cmake/fsr2.cmake` passes `--stb comp 8 --ssb comp 8` (shift texture and sampler
bindings) with `--sib comp 0 --suavb comp 0` (images at 0), because **NVIDIA exposes only 8 image units in
GL** and the images have to fit under that. So `FSR2_BIND_SRV_*` 5, 6 and 7 land on units **13, 14 and
15** — and the macro maximum of 12 lands on **20**. Reading the macros and believing them is what put the
first version of `FSR2BindingScope` at 0..12; unit 13 is `u_ShadowAtlas`. Do not re-derive the range from
FSR2's side at all: size it from the engine's own slot space, which cannot go stale when the shift
changes. Source: the port's author, <https://juandiegomontoya.github.io/porting_fsr2.html>.

**One genuine upstream defect, and one claim of ours that was WRONG.**

1. **REAL: `rw_prepared_input_color` is declared `layout (rgba16)` — UNORM — over a resource created as
   `R16G16B16A16_FLOAT`.** `glBindImageTexture` is called with `GL_RGBA16F`, so the shader's format
   qualifier does not match the bound image and the store is undefined; a UNORM view also cannot represent
   the signed Co/Cg chroma that buffer carries. Worth a PR upstream — the author explicitly accepts them.
2. **NOT A DEFECT, retracted: "`pointSampler` is created, deleted and never bound."** It is true that
   `executeGpuJobCompute` hard-codes `linearSampler` for every SRV, and the earlier version of this note
   concluded that integer SRVs were therefore incomplete and "every fetch returns zero". That does not
   follow. **`texelFetch` ignores sampler state entirely**, and the only integer SRV,
   `r_reconstructed_previous_nearest_depth`, is read exclusively through it
   (`ffx_fsr2_callbacks_glsl2.h`). The author confirms the design: one linear/edge-clamp sampler for the
   `texture*` paths, `texelFetch` for everything else. `pointSampler` is dead code, not a bug. The claim
   was never measured — it was read off the source and asserted, which is exactly the move the rest of
   this section exists to warn against.

**A build trap that voided a whole round of shader experiments, now fixed.** `add_dependencies` orders
targets but does not make the object embedding the SPIR-V depend on the generated headers, and the
permutation command depended only on the pass `.glsl2` and not on the shared `ffx_*.h`. So editing a shader
could regenerate nothing, or regenerate the SPIR-V and relink while the object kept the OLD blobs. Two
different shader edits produced bit-identical frames (74.576736802867472 to 15 digits) before this was
spotted. `cmake/fsr2.cmake` now sets `OBJECT_DEPENDS` and globs the shared headers. **The tell is a result
identical to the last digit** — that is what a no-op experiment looks like, and it is worth checking for
before believing any negative result. The same tell later exposed that `OLO_RG_DISABLE_ALIASING` and
`OLO_RG_POISON_TRANSIENTS` never reached the test process: both "experiments" returned the previous run's
numbers exactly, so both were discarded rather than read as evidence.

**A polarity correction worth not re-deriving:** `ComputeDepthClip` returns **0 when nothing is
disoccluded** (`fWeightSum == 0` falls through to `return 0.0f`). A depth-clip factor of ~0 across a static
frame is CORRECT, not evidence of a broken reconstructed-depth buffer.


### FSR2 on NVIDIA is a known upstream performance problem — budget for it before promising a win

The port's author measured **FSR2 running "about 3x slower than expected" on an RTX 3070**, attributing it
to high VRAM throughput in the **depth-clip** and **reproject & accumulate** passes. The Vulkan backend's
workaround (disabling FP16 for NVIDIA's accumulate pass) was tried in GL and **did not help**; so did
varying the input colour format. The cause is **unresolved** — the author's guess is that the shader
compiler generates different code per API. **AMD (RX 6800) behaves as expected.**
<https://juandiegomontoya.github.io/porting_fsr2.html#performance>

**Measured here on an RTX 4090, driver 610.88, four years and many drivers later — it is still true.**
`FSR2Perf.UpscaleCostPerMegapixelAcrossOutputResolutions` reports FSR2's own dispatch cost, which is the
only measurement that can answer this (a whole-frame number moves the scene cost at the same time):

| output | FSR2Pass | ms/MPix |
|---|---|---|
| 1280x720 | 0.210 ms | 0.228 |
| 1920x1080 | 0.427 ms | 0.206 |
| 2560x1440 | 0.711 ms | 0.193 |

Almost perfectly linear in output pixels — **0.178 ms/MPix marginal, ~0.05 ms fixed** — so it is
throughput-bound rather than dominated by small-dispatch overhead, which is the reading that would have
excused it. Extrapolated to 4K that is **~1.5 ms on a 4090**, against AMD's quoted ~1.1-1.2 ms at 4K on an
RX 6800 XT. Roughly 3x slower than the hardware class implies, matching the author's figure.

**Two levers were tested and neither helps.** Recorded so nobody re-runs them:

* **FP16 for the accumulate pass.** The backend disables it on NVIDIA ("reduced occupancy and high VRAM
  throughput") and that looked like stale Turing-era tuning worth revisiting on Ada. Re-enabling it made
  FSR2 **3.4-8.9x SLOWER** at every resolution (1080p: 0.42 -> 1.55 ms). The workaround is correct and
  load-bearing, and the result corroborates the author's VRAM-throughput diagnosis rather than
  undermining it.
* **The `rw_prepared_input_color` `rgba16`-over-`RGBA16F` mismatch.** Fixing it is **performance-neutral**
  (within 0.5%). Still worth fixing as correctness — it is undefined behaviour — but it is not a speed
  lever. (Measured properly here; an earlier "inert" verdict on this was taken against BRIGHTNESS during
  the window when the stale-shader build trap could have voided it.)

`useLut` is a third candidate and probably not worth the trip: it is gated on `waveLaneCountMax == 64`
while the GL backend hardcodes that to 0, so the reproject pass always takes the reference Lanczos — but
DX12/Vulkan on NVIDIA (wave32) picks the same path, so it is not a GL-specific regression.

**The consequence for #684's acceptance criterion is real:** "a GPU frame-time reduction at 67%/59%" may
not be achievable on NVIDIA with this backend at all, and that is an upstream problem rather than an
integration one. Anyone picking this up should measure on AMD before concluding the integration is at
fault, and should not promise the win on NVIDIA without re-measuring on an idle machine.

### A state restore that is free in an isolated test can be the most expensive thing in the frame

`FSR2BindingScope` originally swept the engine's whole binding space — 70 texture units x 3 targets plus
83 UBO slots, ~1000 GL calls per dispatch. It looked free when it was written, because the isolated test
that measured it leaves most units EMPTY and rebinding 0 over 0 is a no-op the driver discards.

Measured once the other renderer tests had run first and the units actually held textures, it cost
**0.55 ms at 1080p and 1.67 ms at 1440p** — more than FSR2's own dispatch, and at 1440p more than triple
it. The signature was a per-pass cost that was reproducible at two DIFFERENT values depending only on what
had run before it in the same process: 0.43 ms isolated, 0.97 ms after the visual tests, both stable
across repeats. That is not noise, and it is worth recognising: **if a measurement is reproducible at two
values, the variable is state, not jitter.** It had also been quietly wrecking the whole-frame benchmark,
which was being blamed on GPU contention.

The fix was to size the restore to the slots FSR2 can actually REACH rather than everything the engine
owns. The reachable set is knowable because the shift is ours: `cmake/fsr2.cmake` passes
`--stb comp 8 --ssb comp 8 --sib comp 0`, so textures and samplers land at macro+8 and images at 0-7.
Sizing from a constant in this repo is sound in a way that sizing from upstream's macros was not — that
was the original bug. Narrowed cost is within ~2.5% of not restoring at all, with the shadow-sampler UB
still at zero.

**A caveat the author documents and we inherit:** OpenGL's `[-1, 1]` NDC depth against FSR2's `[0, 1]`
assumption leaves its depth constants "subtly wrong". The author judged the result "imperceptibly wrong",
suggests `glClipControl` as the fix, and left it open. We pass `deviceDepthNegativeOneToOne = false`
because the backend asserts on anything else — see the note in `FSR2RenderPass`.

**`GL_KHR_shader_subgroup` is a hard requirement of the backend**, not a nice-to-have: its
`GetDeviceCapabilitiesGL` returns `FFX_ERROR_BACKEND_API_ERROR` without it (it also accepts any AMD
vendor string as implying support). A device that lacks it fails `Configure`, the upscaler reports
`DeviceUnsupported`, and the pipeline falls back to the spatial path with a log line — so this shows
up as "FSR2 did nothing on that machine", never as a crash.

**MSAA is a hard guard, not a note.** A resolve has already averaged the per-pixel depth and motion
vectors FSR2 reconstructs from, so the output is soft and crawling rather than wrong-looking.

> **The fingerprint must hash the RESOLVED decision (`data.TemporalUpscaleActive`), not the
> requested `Technique`.** The decision can flip without any setting moving — the backend coming up,
> MSAA being switched on — and it is what picks whether `FSR2Color` or `EASUColor` gets declared. Hash
> the setting instead and the graph keeps whichever resource was declared when the decision last
> changed, and the upscale silently stops running. Same rule as §13's `Upscale` hash, one level down.

**Exposure is FSR2's own, deliberately.** `FFX_FSR2_ENABLE_AUTO_EXPOSURE` is set rather than feeding
the engine's metered value, because `ToneMapRenderPass` (which owns auto-exposure) runs **after** the
upscaler — so FSR2's input is genuinely un-exposed HDR, which is the case that flag exists for. Its
exposure also lives in an SSBO, not a 1×1 texture, so the explicit path would additionally be a
frame-late SSBO→texture copy for no benefit.

## 14. GPU timer queries

- **`GL_TIME_ELAPSED` scopes must not nest.** `CommandBucket::ExecuteWithGPUTiming` already owns
  per-draw TIME_ELAPSED scopes inside pass execution, so any frame- or pass-level timer must use
  `GL_TIMESTAMP` pairs (`glQueryCounter`) — which is why `GPUPassTimerPool` is timestamp-based.
- **`GPUTimerQueryPool` only calls BeginFrame/EndFrame inside `ExecuteWithGPUTiming`**, which only
  runs while a capture is active. A one-shot capture can therefore never read its own queries at
  same-frame commit — results need a deferred commit or a non-swap readback.
- **GPU query results resolve 1–3 frames late.** Design readback as "previous resolved frame", never
  same-frame. Both of the above were the root cause of "gpuMs always 0.0" in the MCP perf tools.
- `PacketMetadata::m_DebugName` is a **non-owning `const char*`** — point it at strings whose owner
  outlives the frame (e.g. `Submesh::m_NodeName` inside the `MeshSource`).

## 15. Camera-relative rendering cannot fix stored-position ULP

The feature makes `transform * local` and `VP * worldPos` compute near 0, preserving the object's
*internal* geometry. The ECS still holds **f32 world transforms**, so an object at 2^18 has its
translation quantized to ~0.03 regardless.

**When writing a far-from-origin precision test, place geometry at exactly-f32-representable
coordinates** (power-of-two centre, spacing a multiple of the far ULP, all < 2^19) so position
quantization is zero and the only variable is bake precision — the thing the feature changes. With
that, the 2D sprite test gets far_on == near_ref pixel-for-pixel (RMSE 0.000) with the feature on and
clear degradation off (RMSE 15.3). A first version placed sprites at `center + gx*step` computed in
f32 at 262144 and "failed" purely on the position-ULP floor.

> Renderer2D's line path calls `glLineWidth(2.0)`, which raises `GL_INVALID_VALUE` in a GL core
> profile (only 1.0 is guaranteed) and trips the suite's GL-error listener. Any test drawing
> Renderer2D lines/rects must `Renderer2D::SetLineWidth(1.0f)` first.

## 16. A probabilistic driver crash needs A/B experiments, not the stack

Re-opening a scene at runtime hit `EXCEPTION_ACCESS_VIOLATION` in `nvoglv64!DrvPresentBuffers`, with
a stack through `LoadAndRenderSkybox` → `EnvironmentMap::CreateFromEquirectangular` →
`IBLCache::LoadCubemapFromCache` → `OpenGLTextureCubemap::SetFaceDataMip` → `glTextureSubImage3D`.
Crashed within ~3 reloads; fine at editor startup.

The "recycled texture id" theory (low ids on reload vs high at startup) was a **red herring** —
disproved because leaking the old cubemaps so nothing is ever recycled *still* crashed. Also ruled
out by experiment: fence-sync on scene swap, `glFinish` before rebuild, `GL_PIXEL_UNPACK_BUFFER`
unbind, frame-tagged deferred deletion, and mutable `glTexImage2D` storage.

The actual cause was the **client-pointer `glTextureSubImage3D` upload itself** faulting in the
NVIDIA threaded driver during reload churn. Fix: upload faces through a persistent **PBO**. Verified
120 reloads across 4 relaunches with 0 crashes, versus 3–4 reloads to crash for the control.

> **Don't trust the stack's implied cause for a probabilistic GPU-driver crash.** The stack showed
> the upload; *why* it faulted needed env-gated A/B experiments run across **multiple relaunch
> rounds** — a single 20-reload run is too noisy to distinguish signal.

## 17. A post stage after `UICompositePass` has four consumers, and three of them are not the graph

Adding a stage *between* two existing post passes is a well-trodden path:
declare the resource, register the node, and add the name to the downstream
`ReadFirstValidVersionedInputForPass` candidate lists (glsl-shaders.md §9). A
stage added at the **end** of the chain — after `UICompositePass`, which is where
an accessibility remap has to go so the HUD is adapted too (issue #458) — has a
different footprint, and the graph edits are the easy part.

The trap: **the editor viewport is not the backbuffer.** `FinalPass` presents to
the swapchain, but `EditorLayer::UI_Viewport` draws an `ImGui::Image` of a graph
resource it resolves by name, and that name was `UIComposite`. So a stage placed
after UIComposite is correct on the swapchain the editor never shows and
**invisible in the only surface anyone looks at**. Nothing fails; the frame just
never changes, which reads as "my pass isn't running" and sends you into the
render graph.

Four places resolve "the presented image", and all four need the new name at the
top of their fallback chain:

| consumer | site | what breaks if missed |
|---|---|---|
| the graph | `FinalRenderPass::Setup` candidate list | the stage's output is dropped; the frame presents unadapted |
| the editor viewport | `EditorLayer.cpp` `UI_Viewport` | the effect is invisible in the editor |
| MCP screenshots | `EditorLayer.cpp` `mcpContext.CaptureViewportPng` | `olo_screenshot` answers with the pre-stage frame — so your *verification* silently lies |
| frame capture | `RenderGraphFrameCapture.cpp`, the `FinalPass` branch that fills `Source::Backbuffer` | "the image the player saw" is the image from one stage earlier |

The third row is the one that costs a debugging session: the MCP capture and the
visual-evidence fixtures are the instruments you reach for to answer "did it
work?", and until they learn the new name they answer *no* for a reason that has
nothing to do with the pass.

The same applies to the `*VisualEvidenceTest` fixtures, which resolve
`UIComposite` → `ToneMapColor` → `SceneColor` in that order. A new last-stage
test must put its own resource first, or it measures the frame from before its
own stage and every differential assertion passes vacuously against itself.

> **Rule of thumb:** grep `ResolveFrameGraphFramebuffer(ResourceNames::` before
> adding anything to the tail of the post chain. Every hit is a consumer that
> has hard-coded "the last stage" by name.

## 18. The RHI's persistent mapping is WRITE-only — a GPU-visible CPU structure needs host authority, not mapped reads

`RenderCommand::AllocatePersistentUploadStorage` maps with
`GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` and **no read
bit** (`OpenGLRendererAPI.cpp`), so a CPU read through the returned pointer is
undefined behaviour — and would be a WC-memory perf trap even where it happens
to work. This matters the moment a CPU-maintained structure must also be
readable by shaders: a hash table or link table the CPU probes cannot live *in*
the mapping.

The `Renderer/GPUCache/` substrate (#704) is the worked example of the correct
shape: `GPUCacheStorage`'s `HostMirrored` backing keeps the heap copy as the
single authority for every CPU read/write and write-through-mirrors each
committed mutation into the mapping for shader-side lookup. Two dividends
beyond correctness: CPU probes hit cacheable heap memory instead of uncached
WC pages, and the identical logic runs with `HostOnly` backing — which is what
lets the allocate/evict/split-chain/collision tests run **headless** instead of
skipping without a GL context. Bulk payloads that the CPU only ever writes
(`DeviceMapped` backing) are the one case the raw mapping serves directly.

If you find yourself wanting a READ|WRITE persistent mapping instead, that is a
new RHI entry point across both backends — weigh it against the mirror pattern
first.

## 19. A/B a shader-side renderer change against the live editor without rebuilding

Shaders load from `OloEditor/assets/shaders` at runtime and `OpenGLShader::IsCacheStale` checks
every transitively included file's mtime, so a `.glsl` edit costs an editor restart, not a build.
That turns "did this actually change the frame, and by how much?" from an hour of build-mutex queue
into about two minutes:

1. Launch with the MCP diagnostics server and capture a **fixed pose set**
   (`olo_screenshot` takes the pose itself and restores it, so one call per pose).
2. Neutralise the change at its narrowest point — one constant, one function's return — rather than
   reverting the file. For #751 that was `ddgiBounceMargin()` returning `vec3(0.0)`, which is exactly
   the pre-fix behaviour.
3. Restart, capture the *same* poses, restore the shader **from a copy you made first**.
   Never `git checkout --` it (see [shader-ab-restore-pitfall] in the session memory: it wipes your
   uncommitted work).
4. Compare region means in approximate linear luminance (`display^2.2`), not display units — a 4×
   linear change reads as only 1.9× after the gamma encode.

**Include a pose the change cannot possibly affect, and you get the noise floor for free.** For a
DDGI volume that is any surface outside its bounds: the lit-pass sampler still hard-rejects those,
so that pose must reproduce exactly. It came back at ×1.000/×1.001 across two independent editor
launches — which is what licensed reading the ×1.04–1.09 elsewhere as real rather than as jitter.
Without such a control you are guessing, and
[live-verification-noise-floor.md](live-verification-noise-floor.md) is the longer version of why
that guess is usually wrong.

The same trick has a headless twin: a shader-only fix can be re-measured by re-running the test
binary, with no relink at all. Only C++ changes need the mutex.
