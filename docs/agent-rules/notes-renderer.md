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
