# PSO precaching: requests, readiness and first-use hitches

The rules for the request/readiness contract behind Vulkan pipeline creation (#1345):
[VulkanPipelineBuilder](../../OloEngine/src/Platform/Vulkan/VulkanPipelineBuilder.h) owns the
pipelines and the request ledger,
[VulkanPipelinePrecache](../../OloEngine/src/Platform/Vulkan/VulkanPipelinePrecache.h) decides what to
request and when, and `olo_perf_pipeline_readiness` reports both.

## Rules

1. **A pipeline a draw has to create is a counted, classified miss, never a silent one.** The ledger
   ([PipelineRequestLedger.h](../../OloEngine/src/OloEngine/Renderer/PipelineRequestLedger.h)) sorts
   every draw-time miss into never requested, requested too late (still queued, or in flight on a
   worker) or retried after a failure, and times the draw thread's wait. The first 64 misses of a
   session are logged with the shader and key; the rest are only counted. A draw that finds a
   pipeline a request created is a *prepared first use*: the only outcome with no synchronous
   creation on a recording thread.
2. **Derive every key through `VulkanPipelineBuilder::DeriveGraphicsKey`.** The draw, the request and
   the tests must not disagree about what a key is. Its inputs are the shader identity (a skinned,
   mesh-stage or material variant is its own shader), the root-data layout, the attachment formats
   and count, the sample count, the tessellation patch size, and the blend state only where EDS3
   blend is missing. The render **resolution is not an input**: viewport and scissor are
   `*_WITH_COUNT` dynamic state. Adding a baked axis means adding it to the key, or two
   incompatible configurations share a pipeline (`VulkanPipelineKey.*` pins the separation).
3. **Requests come from the PSO manifest crossed with the shaders that exist.** The manifest
   ([PipelineManifest.h](../../OloEngine/src/OloEngine/Renderer/PipelineManifest.h)) records each key
   a draw had to create, against the shader's name, stage mask and the configuration tag published
   at the time. A prepared pipeline's draw refreshes its entry too, once per recording epoch (a new
   session or a new selected configuration), so a key that keeps hitting stays recently seen and
   learns every configuration it is drawn under. A shader is requested against all its entries the moment its modules exist
   (construction, hot reload): the selected configuration's entries first, the others after them.
   Construction is the content hook; there is no per-pass derivation, because the renderer picks
   shaders in three layers (submit, dispatch, each pass's `Execute`) and a mirror of that would drift.
4. **A setting that changes a pass's attachment formats or sample count belongs in the
   configuration tag.** `MakePipelineConfigurationTag` (path, G-Buffer samples, upscaled or native,
   temporal upscale, OIT) is published from `RenderPipeline::CompileFrameGraph`. A setting left out
   of the tag still works, but its keys are prepared speculatively instead of first, and a switch to
   it can land before they are ready. The first tag of a session is selected at once; a change is
   selected after it has been the only tag published for two frames, so two renderers with
   different settings in one frame cannot flip the selection every frame.
5. **Never hold the builder lock across `vkCreate*Pipelines`.** The hit path of every recording
   thread shares that lock. A creation claims its key under the lock, snapshots the shader's modules
   and layout on the asking thread, releases the lock for the driver call, then publishes under the
   lock. A second thread that needs the same key waits on the first instead of compiling it again.
6. **Destroy a shader's modules only after `InvalidateShader` or `QuiesceShader` returned.** A queued
   request carries the old module handles, and a worker may be compiling against them right now.
   `InvalidateShader` waits for this shader's creations still in flight (graphics reload, destructor),
   and `QuiesceShader` drops its queued requests and waits too (the compute rebuild, which replaces
   its module before the reload invalidates). A worker's result for a shader invalidated meanwhile is
   discarded (`discardedStale`), never published.
7. **Render-ready means every selected-configuration request is created.** The precache declares a
   shader ready to the ledger only then. A later draw that still needs a pipeline nobody requested
   for that shader is counted as a `readyClaimViolation` and logged, so a readiness claim cannot hide
   synchronous work. A shader the manifest has no entry for under the selected configuration is
   *unknown*, never ready. Its first draw is a counted never-requested miss, recorded, and requested
   ahead from the next session on. The fallback for a draw that arrives first is synchronous
   creation, timed and named `Vulkan::PipelineBlockingCreate` in the CPU scope ring; a draw is never
   skipped.
8. **Preparation is budgeted per frame and runs outside every recording region.**
   `VulkanPipelinePrecache::Tick` runs on the render thread after the frame fence and hands the
   workers `OLO_VK_PSO_PRECACHE_JOBS_PER_FRAME` starts (default 4; `OLO_VK_PSO_PRECACHE_WORKERS`
   threads, default 2). The budget does not accumulate, and work over it carries over to the next
   frame. While no frame comes (a packaged runtime compiles and loads for tens of seconds before its
   first frame) the workers refill the same budget every 33 ms themselves, so loading prepares the
   requests on background threads. Nothing ever waits for the queue: a long manifest spreads over
   frames, or over the load, instead of becoming a stall.
9. **On OpenGL "ready" means the program link completed, and nothing more is claimed.** GL has no
   pipeline object. The counters there are compiles, SPIR-V cache hits, links issued, program-binary
   hits and rejects, and binds that forced an unfinished link (`EnsureLinked`, the GL form of a late
   request). The driver's recompile on a program's first draw under new state has no GL signal; the
   tool lists it as `unmeasurable`, never as zero.
10. **Measure cold against warm in fresh processes with fixed quality.** Point `OLO_SHADER_CACHE_DIR`
    at a copy of a primed cache and delete `vulkan/pipeline_cache.vkpc` (pipeline-cold) and
    `vulkan/pso_manifest.yaml` (request-cold) as the cell needs. Read the counters with
    `olo_perf_pipeline_readiness` (`reset: true` opens a window), and read the frame-time tail from
    the CPU scope ring. `OLO_VK_PSO_PRECACHE=0` is the A/B: no requests, so every first use is a
    synchronous creation.
11. **A packaged game ships the manifest as a seed.** Build Game saves the live manifest, then
    stages the machine's `pso_manifest.yaml` as `assets/PipelineManifest.yaml`. Only an application
    session reads and writes the machine manifest; a test that builds a `VulkanContext` does not. The runtime merges it under its own
    manifest and requests keys for the shaders its cooked content creates. A seed entry whose shader
    is not in the build is never requested.

## Where it lives

- Ledger and counters: `OloEngine/Renderer/PipelineRequestLedger.h`. Manifest and tag:
  `PipelineManifest.*`. Backend-neutral snapshot and shader-preparation counters:
  `PipelineReadiness.h`.
- Vulkan: `VulkanPipelineBuilder.*` (keys, miss path, creation feedback),
  `VulkanPipelinePrecache.*` (collection, workers, readiness). Lifecycle in `VulkanContext`: `Start`
  after device bring-up, `Tick` after the frame fence, `Stop` before `ReleaseAll`.
- Tests: `PipelinePrecacheContractTest.cpp` (device-free), `VulkanShaderPipeline.*` precache cases
  (device), `McpPipelineReadinessTest.cpp`.
