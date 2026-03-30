# OloEngine Improvements Inspired by Hazel

Actionable items where Hazel's design or features are superior, ordered by impact. Vulkan migration is out of scope.

---

## 1. Rendering — Core Techniques

### 1.1 Replace SSAO with GTAO (Ground Truth Ambient Occlusion)

Hazel uses compute-based GTAO with multi-pass denoising (`GTAOCompute` → `GTAODenoiseCompute` × N passes → `AOComposite`). It produces significantly higher-quality AO than traditional SSAO, handles bent normals, and is configurable (radius, falloff, blur beta, half-res toggle). OloEngine's SSAO pass could be upgraded or replaced.

**Key Hazel references:** `SceneRenderer::GTAOCompute()`, `CBGTAOData` struct, `m_GTAOComputePass`, `m_GTAODenoisePass[2]`.

### 1.2 Hierarchical Z-Buffer (HZB)

Hazel generates an HZB (mip-chain of the depth buffer via compute) used for both **occlusion culling** and **SSR acceleration**. OloEngine has frustum/occlusion culling but no HZB. Adding one would improve SSR quality and enable GPU-driven occlusion queries.

**Key Hazel references:** `HZBCompute()`, `CreateHZBPassMaterials()`, `HZBUVFactor` in GTAO/SSR data.

### 1.3 Spot Light Shadow Maps

Hazel supports per-spot-light shadow maps (`SpotShadowMapPass`, `m_SpotShadowPassPipeline`, `UBSpotShadowData` with 1000 shadow matrices). OloEngine only has cascaded directional shadows. Spot shadow maps would significantly improve indoor/architectural lighting.


**Key Hazel references:** `JumpFloodPass()`, `EdgeDetectionPass()`, `SceneRendererSpecification::EnableEdgeOutlineEffect`.

---

## 2. Rendering — Design & Architecture

### 2.1 Dedicated Render Thread

Hazel separates game logic from GPU submission via a `RenderThread` class with `SingleThreaded`/`MultiThreaded` policies. Commands are queued as lambdas (`Renderer::Submit()`), double-buffered, and executed on a dedicated thread. OloEngine's OpenGL context is single-threaded. While OpenGL doesn't support true multi-threaded command recording, the **command queue double-buffering pattern** (main thread populates frame N+1 while render thread executes frame N) could still be applied to OpenGL to reduce frame latency and decouple simulation from rendering.

### 2.2 Resource Lifetime Management — **Done**

Hazel has `Renderer::SubmitResourceFree()` which defers GPU resource destruction to after the current frame finishes rendering (frame-indexed release queues). OloEngine now has `FrameResourceManager::SubmitForDeletion()` with per-frame deletion queues drained after GPU fence sync in `BeginFrame()`. All 12 OpenGL resource types defer destruction through this system. See `docs/deferred_gpu_resource_deletion.md`.

### 2.3 Quality Tiering System

Hazel has `TieringSettings` (environment map resolution, irradiance samples, etc.) that can be swapped per quality preset. OloEngine exposes many settings in the Renderer Settings panel but lacks a formal tiering/preset system to bundle them. A simple preset struct (Low/Medium/High/Ultra) mapping to existing settings would improve usability.

---

## 3. Audio

### 3.1 DSP Pipeline

Hazel has a full DSP chain under `Audio/DSP/`: reverb (Freeverb model with allpass/comb filters), spatializer, delay lines, and component-based filter graph. OloEngine has `AudioEngine` and `AudioSource`/`AudioListener` components but no visible DSP processing. Adding at least reverb and basic spatial attenuation would bring audio to minimum viable quality.

### 3.2 Sound Graphs (Node-Based Audio)

Hazel's `SoundGraph/` implements a node-based audio processing graph — essentially audio shader graphs. This allows designers to create complex audio behaviors visually. OloEngine has `SoundGraph` types registered with backend infrastructure ~70% complete (DSP nodes, graph evaluation, connection system). Remaining work: additional node types (oscillators, envelopes, mixers) and an editor UI (node canvas with drag-and-drop connections). Worth revisiting once core audio features are stable.

### 3.3 Audio Events System — **Done**

Hazel uses an event-driven audio model (`AudioEventsManager`, `AudioCommands`, `CommandID`, `SoundBank`) where gameplay triggers named events rather than directly playing sounds. This decouples game logic from audio assets and enables sound designers to remap audio without code changes. OloEngine now implements this pattern: `AudioCommandRegistry` stores trigger definitions in YAML, `AudioEventsManager` dispatches events at runtime, `CommandID` provides deterministic CRC32 hashing, and `AudioPlayback` offers a static convenience API. Both C# and Lua scripting expose the full event API (PostTrigger, StopEvent, PauseEvent, ResumeEvent, StopAll, IsEventActive). The `AudioSourceComponent` supports dual-mode playback (direct or event-driven via `UseEventSystem` flag).

---

## 4. Editor UX (Hazelnut → OloEditor)

### 4.1 Dual Viewport

Hazelnut supports a toggleable secondary viewport with independent camera. Useful for previewing a scene from multiple angles, debugging cameras, or split editing. OloEditor currently has a single viewport.

### 4.2 Auto-Save with Recovery

Hazelnut auto-saves every 300 seconds to `.hscene.auto` and presents a recovery dialog on launch if an auto-save exists. OloEditor has Quick Save (F5) but no automatic timed save or crash recovery.

### 4.3 Content Browser Polish

Hazelnut's content browser has:
- **Dual-pane layout** (folder tree + content grid)
- **Thumbnail generation** (with cache clear button)
- **Real-time search filtering**
- **Multi-select** with Ctrl/Shift+click
- **Breadcrumb navigation** with back/forward history
- **Recent projects** (last 10, sorted by date)

OloEditor's content browser now matches Hazelnut's feature set: dual-pane layout (tree + grid), real-time search, multi-select with Ctrl/Shift, breadcrumb navigation with back/forward, settings persistence via imgui.ini, and typed drag-drop payloads for scenes/models/scripts.

### 4.4 Statistics Panel Depth ✅

OloEditor now has a 4-tab Statistics panel (Renderer, Audio, Performance, Memory) replacing the old flat Scene Statistics panel. Includes `PerformanceProfiler` for per-function per-frame timing with sample counts (instrumented on EditorLayer and Scene update methods), `AudioStats` query, `RendererMemoryTracker` summary, GPU info via `glGetString`, and VSync toggle.

### 4.5 Entity Selection Outline

OloEditor now has 2D selection outlines (via `Renderer2D::DrawRect`) and a 3D Jump Flood Algorithm (JFA) outline pass (`SelectionOutlineRenderPass`). The JFA pipeline seeds a distance field from the ScenePass entity-ID attachment, propagates it via configurable ping-pong flood passes, and composites a smooth anti-aliased outline over the scene. Matches Hazelnut's approach without the extra SelectedGeometry re-render pass (reuses existing entity IDs instead).

---

## 5. Scene / Components

### 5.1 Rich Text Component — **Done**

Hazel's `TextComponent` supports: font asset handles, kerning, line spacing, max width, screen-space mode, **drop shadows** (distance + color). OloEngine's `TextComponent` now has `MaxWidth` (two-pass word wrapping in `Renderer2D::DrawString`), `DropShadow`, `ShadowDistance`, `ShadowColor` with full serialization and editor UI. See `docs/rich_text_component.md`.

### 5.2 Compound Colliders

Hazel has `CompoundColliderComponent` which aggregates multiple child colliders into a single physics shape (with `IncludeStaticChildColliders` and `IsImmutable` flags). OloEngine has individual collider types but no compound aggregation. This is useful for complex static geometry.

### 5.3 Tile Renderer — **Done**

Hazel has `TileRendererComponent` for grid-based tile rendering (width × height grid, per-cell material IDs). OloEngine now has `TileRendererComponent` with `TileMesh`, `Width`/`Height` grid, `TileSize`, material palette (`vector<Material>`), per-cell `MaterialIDs`, full 3D rendering via `Renderer3D::DrawMesh`, serialization, and editor panel with drag-drop mesh assignment and material palette editing. See `docs/tile_renderer_component.md`.

### 5.4 Transform Component — Quaternion/Euler Sync

Hazel's `TransformComponent` stores **both** quaternion and Euler, kept in sync via `SetRotation()`/`SetRotationEuler()` with a clever algorithm that picks the closest Euler representation to avoid 180° flips when converting back from quaternions. Worth comparing to OloEngine's implementation — if it only stores Euler or only stores quaternion, it may suffer from gimbal lock or representation instability.

---

## 6. Asset System

### 6.1 Dependency-Aware Hot Reload

Hazel's `Asset` base class has `virtual void OnDependencyUpdated(AssetHandle handle)`. When a texture changes, any material referencing it gets notified and can refresh. OloEngine has hot-reload via `AssetReloadedEvent`, but it's worth auditing whether dependent assets cascade properly (e.g., does changing a texture auto-update all materials using it?).

### 6.2 Shader Packs — ✅ Done

Hazel can pre-compile all shaders into a pack file (`ShaderPack`, `RendererConfig::ShaderPackPath`). This eliminates runtime shader compilation stalls in shipped builds. OloEngine now has a `ShaderPack` class with `.osp` binary format ("OLSP" magic) that bundles both Vulkan and OpenGL SPIR-V. The editor Build menu has "Build Shader Pack", and `ShaderLibrary` transparently loads from pack when available. 11 unit tests cover the binary format round-trip.

---

## 7. Type Reflection

Hazel has a lightweight reflection system (`Reflection/TypeDescriptor.h`, `TypeName.h`, `TypeUtils.h`, `MetaHelpers.h`) that appears to support type introspection without heavy macros. OloEngine doesn't have a reflection system. Benefits:
- Generic component serialization (less boilerplate per component)
- Script binding generation
- Editor property inspector automation

This is a large architectural investment but would reduce the "add component → update 8 places" problem mentioned in Hazel's own Components.h comments.

---

## 8. Animation Graph Nodes

Hazel's animation system includes specialized graph nodes: `IKNodes` (inverse kinematics), `BlendNodes`, `TriggerNodes`, `LogicNodes`, `MathNodes`, `ArrayNodes`, `StateMachineNodes`. OloEngine has an animation graph editor with blend trees, but should audit whether IK solving and the full node variety are implemented or stubbed.

---

## Priority Ranking

| Priority | Item | Effort | Impact | Status |
|----------|------|--------|--------|--------|
| **High** | 4.5 Selection outline (Jump Flood) | Medium | Major editor UX | ✅ Done |
| **High** | 1.1 GTAO replacing SSAO | Medium | Visual quality leap | ✅ Done |
| **High** | 3.1 Audio DSP (at least reverb + spatial) | Medium | Audio is unusable without it | ✅ Done |
| **High** | 4.2 Auto-save with recovery | Low | Prevents data loss | ✅ Done |
| **Medium** | 1.3 Spot light shadows | Medium | Lighting quality | ✅ Done |
| **Medium** | 1.2 HZB for SSR + occlusion | Medium | Performance + SSR quality | ✅ Done |
| **Medium** | 4.3 Content browser search + thumbnails | Medium | Editor workflow | ✅ Done |
| **Medium** | 2.3 Quality tiering presets | Low | Usability | ✅ Done |
| **Medium** | 6.1 Dependency-aware hot reload | Low-Med | Asset iteration speed | ✅ Done |
| **Medium** | 5.4 Quaternion/Euler sync in Transform | Low | Prevents rotation bugs | ✅ Done |
| **Medium** | 2.2 Resource lifetime management | Low-Med | Correctness hardening | ✅ Done |
| **Medium** | 5.1 Rich text component | Low-Med | Text rendering completeness | ✅ Done |
| **Medium** | 5.3 Tile renderer | Low-Med | Grid-based game support | ✅ Done |
| **Lower** | 4.1 Dual viewport | Medium | Nice to have | Skip — low priority, single viewport sufficient for current workflow |
| **Lower** | 7 Type reflection | High | Long-term architecture | Skip — high architectural effort for lower priority feature |
| **Lower** | 6.2 Shader packs | Medium | Distribution builds | ✅ Done |
| **Lower** | 5.2 Compound colliders | Low | Physics completeness | ✅ Done |
| **Lower** | 2.1 Dedicated render thread | High | Perf (limited by single-threaded GL) | Skip — unfinished in Hazel, wrong arch for single-threaded GL |
| **Lower** | 3.2 Sound graphs (node-based audio) | Medium | Audio designer workflow | Skip — backend 70% done, missing only node types + editor UI |
| **Lower** | 3.3 Audio events system | Medium | Audio architecture | ✅ Done |
| **Lower** | 8 Animation graph nodes (IK, etc.) | Medium | Animation completeness | |
