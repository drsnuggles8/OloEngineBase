# Rendering Pipeline Integration Audit

## Executive Summary

The engine's rendering pipeline was designed around a **Molecular Matters-inspired command bucket architecture**: deferred command recording into POD packets → radix sort by 64-bit draw key → batching → dispatch. A `RenderGraph` provides pass ordering and framebuffer piping. These systems are well-implemented and form a solid foundation.

However, **every rendering feature added after the command bucket was established** bypasses it entirely. Shadows, SSAO, particles, terrain, foliage, voxels, SSS/snow, and post-processing all render via direct `RenderCommand::` calls, raw `gl*` invocations, or callback escape hatches. The render graph provides execution ordering but its framebuffer piping is partially redundant with manual wiring in `EndScene()`.

The result: the architectural intent (deferred record → sort → execute) is only realized for main scene geometry in `SceneRenderPass`. Everything else is an immediate-mode workaround.

---

## Implementation Status

All three phases have been implemented and verified (OloEditor builds clean, 365/365 tests pass).

### Phase 1 — Foundation ✅

| Issue | Status | Summary |
|-------|--------|---------|
| **#6** — Shared fullscreen triangle | ✅ Done | `FullscreenRenderPass` base class with static `s_FullscreenTriangleVA`. SSAO, SSS, PostProcess, Final inherit from it. |
| **#1** — Split RenderPass base class | ✅ Done | `RenderPass` is now lean (no command bucket). `CommandBufferRenderPass` adds bucket/allocator. Only `SceneRenderPass` inherits it. |
| **#7** — Raw GL call abstractions | ✅ Done | Added `RenderCommand::CopyImageSubData()`, `SetDrawBuffers()`, `SetBlendStatePerAttachment()`, `DeleteTexture()`. All raw `gl*` calls in render passes replaced. |
| **#4** — Consolidated framebuffer piping | ✅ Done | Removed redundant manual `SetInputFramebuffer()` calls from `EndScene()`. Graph's `ConnectPass()` piping is the single source of truth. |

### Phase 2 — Graph Correctness ✅

| Issue | Status | Summary |
|-------|--------|---------|
| **#5** — SSS gets own framebuffer | ✅ Done | `SSSRenderPass` now has its own RGBA16F output framebuffer. Reads scene color as input texture, writes to own target — no read-write hazard. `GetTarget()` returns `m_InputFramebuffer` when disabled (passthrough). |
| **#8** — Staging copy eliminated | ✅ Done | Resolved by #5 — separate input/output means no `glCopyImageSubData` staging copy needed. |

### Phase 3 — Feature Integration ✅

| Issue | Status | Summary |
|-------|--------|---------|
| **#2** — Terrain as command packets | ✅ Done | New `DrawTerrainPatchCommand` and `DrawVoxelMeshCommand` POD types with dispatch functions. Terrain chunks and voxel meshes now submit through the command bucket via `Renderer3D::DrawTerrainPatch()` / `DrawVoxelMesh()`, participating in sort/batch/profiling. `PostExecuteCallback` reduced to foliage-only (self-batching subsystem). |
| **#3** — Shadow pass data-driven | ✅ Done | `ShadowRenderPass` completely rewritten: callback replaced with 5 POD caster structs (`ShadowMeshCaster`, `ShadowSkinnedCaster`, `ShadowTerrainCaster`, `ShadowVoxelCaster`, `ShadowFoliageCaster`). Scene.cpp submits casters during entity traversal. `Execute()` iterates caster lists per cascade/face with appropriate depth shaders. No duplicate entity traversal, no per-frame lambda allocation. Skinned shadow casters reuse bone data offsets from scene packets. |
| **#9** — Per-frame callback elimination | ✅ Done | Shadow callback entirely removed. Terrain callback replaced with command packets. Particle callback remains (appropriate for self-batching subsystem). Foliage stays as slim `PostExecuteCallback` (also a self-batching subsystem). |

---

## Issue 1: Only `SceneRenderPass` Uses the CommandBucket

### Problem

The base class `RenderPass` allocates a `CommandAllocator` and owns a `CommandBucket`. Only `SceneRenderPass::Execute()` calls `m_CommandBucket.SortCommands()` / `m_CommandBucket.Execute()`. Every other pass ignores its inherited bucket entirely — it's dead weight.

| Pass | Rendering approach | Uses `m_CommandBucket`? |
|------|-------------------|------------------------|
| `SceneRenderPass` | Sorts + executes bucket, then fires callback | **Yes** |
| `ShadowRenderPass` | Lambda callback with direct `RenderCommand::DrawIndexed()` | No |
| `SSAORenderPass` | Direct fullscreen draws | No |
| `ParticleRenderPass` | Lambda callback with direct draws | No |
| `SSSRenderPass` | Direct fullscreen draws + raw `gl*` | No |
| `PostProcessRenderPass` | Direct fullscreen draws | No |
| `FinalRenderPass` | Direct fullscreen draws | No |

**Files:**
- `OloEngine/src/OloEngine/Renderer/Passes/RenderPass.h` — base class allocates unused resources
- All pass `.cpp` files — none except `SceneRenderPass.cpp` touch `m_CommandBucket`

### Proposed Fix

**Phase 1 — Stop allocating unused resources:**

Split `RenderPass` into two base classes:
- `RenderPass` — pure interface (name, framebuffer lifecycle, execute). No command bucket.
- `CommandBufferRenderPass : RenderPass` — adds `m_CommandBucket`, `m_Allocator`, `SubmitPacket()`, `ResetCommandBucket()`.

Only `SceneRenderPass` (and eventually `ShadowRenderPass`) would inherit `CommandBufferRenderPass`. Fullscreen passes inherit plain `RenderPass`.

```cpp
// RenderPass.h — lean base
class RenderPass : public RefCounted
{
public:
    virtual ~RenderPass() = default;
    virtual void Init(const FramebufferSpecification& spec) = 0;
    virtual void Execute() = 0;
    virtual Ref<Framebuffer> GetTarget() const = 0;
    virtual void SetupFramebuffer(u32 width, u32 height) = 0;
    virtual void ResizeFramebuffer(u32 width, u32 height) = 0;
    virtual void OnReset() = 0;
    virtual void SetInputFramebuffer(const Ref<Framebuffer>&) {}

    void SetName(std::string_view name) { m_Name = name; }
    const std::string& GetName() const { return m_Name; }

protected:
    std::string m_Name = "RenderPass";
    Ref<Framebuffer> m_Target;
    FramebufferSpecification m_FramebufferSpec;
};

// CommandBufferRenderPass.h — command bucket owner
class CommandBufferRenderPass : public RenderPass
{
public:
    CommandBufferRenderPass();

    void ResetCommandBucket();
    void SetCommandAllocator(CommandAllocator* allocator);
    void SubmitPacket(CommandPacket* packet);
    CommandBucket& GetCommandBucket();

protected:
    CommandBucket m_CommandBucket;
    Scope<CommandAllocator> m_OwnedAllocator;
    CommandAllocator* m_Allocator = nullptr;
};
```

**Phase 2 — Fullscreen pass base class:**

Create `FullscreenRenderPass` for SSAO, SSS, PostProcess, and Final passes that shares a single fullscreen triangle (see Issue 6).

---

## Issue 2: Terrain Bypasses CommandBucket via `PostExecuteCallback`

### Problem

`SceneRenderPass` has a `PostExecuteCallback` escape hatch invoked after command bucket execution but while the framebuffer is still bound. `Scene.cpp` sets a ~250-line lambda that directly renders terrain chunks, voxel overlays, and foliage with immediate `RenderCommand::` calls.

Consequences:
- Terrain draws are **never sorted** by draw key — can't minimize state changes with other opaque geometry.
- Terrain **can't be batched** with PBR meshes sharing similar state.
- Terrain draws **aren't tracked** by `RendererProfiler` (it counts command bucket packets).
- The callback must manually reset blend state left over from the command bucket (`RenderCommand::SetBlendState(false)`), proving the two code paths don't share state tracking.

**Files:**
- `OloEngine/src/OloEngine/Renderer/Passes/SceneRenderPass.h` — `PostExecuteCallback` typedef
- `OloEngine/src/OloEngine/Renderer/Passes/SceneRenderPass.cpp` lines 98–102
- `OloEngine/src/OloEngine/Scene/Scene.cpp` lines 2022–2260

### Proposed Fix

**Create POD command types for terrain rendering:**

```cpp
// In Commands/RenderCommand.h

// Add to CommandType enum:
DrawTerrainPatch,
DrawTerrainPatchInstanced,
DrawVoxelMesh,
DrawFoliageInstanced,

// POD command structs:
struct DrawTerrainPatchCommand
{
    RendererID VAO = 0;
    u32 IndexCount = 0;
    u32 PatchVertexCount = 3;
    RendererID ShaderID = 0;
    RendererID HeightmapTextureID = 0;
    RendererID SplatmapTextureID = 0;
    RendererID AlbedoArrayID = 0;
    RendererID NormalArrayID = 0;
    RendererID ARMArrayID = 0;
    // Tessellation factors are already in TerrainUBO — just store UBO offset
    u32 TerrainUBOOffset = 0;
    u32 ModelUBOOffset = 0;
    i32 EntityID = -1;
    PODRenderState RenderState;
};
```

**Add dispatch functions in `CommandDispatch`:**

```cpp
static void DrawTerrainPatch(const void* data, RendererAPI& api);
```

**Refactor `Scene.cpp` terrain rendering:**

Instead of the callback lambda, submit `DrawTerrainPatchCommand` packets to the scene pass's command bucket during the normal entity traversal. The terrain UBO data (tess factors, world size, etc.) goes into `FrameDataBuffer` with an offset stored in the packet.

```cpp
// Scene.cpp — terrain entity loop (runs alongside mesh entity loop)
for (auto entity : terrainView)
{
    // ... build terrain UBO data ...
    u32 uboOffset = FrameDataBufferManager::Get().Write(&terrainUBOData, sizeof(terrainUBOData));

    for (const auto& chunk : selectedChunks)
    {
        DrawTerrainPatchCommand cmd{};
        cmd.VAO = chunk->GetVertexArray()->GetRendererID();
        cmd.IndexCount = chunk->GetIndexCount();
        cmd.ShaderID = terrainShader->GetRendererID();
        cmd.HeightmapTextureID = heightmapGPU->GetRendererID();
        cmd.TerrainUBOOffset = uboOffset;
        cmd.EntityID = static_cast<i32>(entity);
        // ... fill texture IDs ...

        PacketMetadata meta;
        meta.Key = DrawKey::Create(/* terrain sort layer */, shaderID, materialID, depth);
        Renderer3D::SubmitPacket(allocator->CreateCommandPacket(cmd, meta));
    }
}
```

**Remove `PostExecuteCallback`** from `SceneRenderPass` entirely once all geometry types submit through the bucket.

---

## Issue 3: Shadow Rendering Bypasses CommandBucket and Duplicates Entity Traversal

### Problem

`ShadowRenderPass` uses a callback pattern where `Scene.cpp` sets a lambda (~300 lines) that re-iterates all mesh entities, animated meshes, terrain chunks, voxels, and foliage with direct `RenderCommand::DrawIndexed()` calls. This means:

1. **Same entity traversal happens twice** per frame (once for scene commands, once for shadow callback).
2. Shadow draws are **not sorted** — pointless state thrashing between cascade/face renders.
3. Shadow draws **can't use instancing** from the batching system.

**Files:**
- `OloEngine/src/OloEngine/Renderer/Passes/ShadowRenderPass.h` — callback typedef
- `OloEngine/src/OloEngine/Renderer/Passes/ShadowRenderPass.cpp` — callback invocation per cascade/face
- `OloEngine/src/OloEngine/Scene/Scene.cpp` lines 1525–1850

### Proposed Fix

**Give `ShadowRenderPass` its own `CommandBucket`** (make it inherit `CommandBufferRenderPass`).

**Option A — Shadow-specific command submission (simpler, short-term):**

During entity traversal in `Scene.cpp`, submit shadow draw commands to the shadow pass's bucket alongside the scene pass submissions:

```cpp
// Scene.cpp entity loop
for (auto entity : meshView)
{
    // Submit to scene bucket (existing)
    Renderer3D::DrawMesh(mesh, transform, material, isStatic, entityID);

    // Submit to shadow bucket (new — if shadow casting)
    if (!material.GetFlag(MaterialFlag::DisableShadowCasting))
    {
        DrawShadowMeshCommand shadowCmd{};
        shadowCmd.VAO = mesh->GetVertexArray()->GetRendererID();
        shadowCmd.IndexCount = mesh->GetIndexCount();
        shadowCmd.ModelMatrix = transform;
        shadowPass->GetCommandBucket().Submit(shadowCmd, shadowMeta, shadowAllocator);
    }
}
```

The `ShadowRenderPass::Execute()` then iterates cascades/faces, sets the light VP, and dispatches the same bucket with different camera UBO data per cascade. The bucket is submitted once but dispatched N times (once per cascade/face).

**Option B — Command packet re-dispatch (optimal, long-term):**

Scene pass command packets already contain `VAO`, `IndexCount`, and `ModelMatrix`. The shadow pass could iterate the scene pass's sorted bucket and re-dispatch each opaque packet with a depth-only shader override:

```cpp
void ShadowRenderPass::Execute()
{
    for (u32 cascade = 0; cascade < cascadeCount; ++cascade)
    {
        SetupCascade(cascade);
        m_DepthShader->Bind();

        // Re-dispatch scene bucket with shadow shader override
        m_SceneBucket->ForEachCommand([&](const CommandPacket& packet)
        {
            if (packet.GetType() == CommandType::DrawMesh ||
                packet.GetType() == CommandType::DrawTerrainPatch)
            {
                DispatchAsShadow(packet);
            }
        });
    }
}
```

This eliminates the duplicate entity traversal entirely.

**Remove `ShadowRenderCallback`** once the shadow pass owns its command data.

---

## Issue 4: RenderGraph Framebuffer Piping Is Redundant with Manual Wiring

### Problem

In `SetupRenderGraph()`, both graph connections and manual `SetInputFramebuffer()` calls exist:

```cpp
// Graph connections (declarative)
s_Data.RGraph->ConnectPass("SSSPass", "PostProcessPass");
s_Data.RGraph->ConnectPass("PostProcessPass", "FinalPass");

// Manual wiring (imperative, partially duplicates above)
s_Data.PostProcessPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
s_Data.FinalPass->SetInputFramebuffer(s_Data.PostProcessPass->GetTarget());
```

In `EndScene()`, more manual wiring happens every frame:

```cpp
s_Data.FinalPass->SetInputFramebuffer(s_Data.PostProcessPass->GetTarget());
s_Data.ParticlePass->SetSceneFramebuffer(s_Data.ScenePass->GetTarget());
s_Data.SSAOPass->SetSceneFramebuffer(s_Data.ScenePass->GetTarget());
s_Data.SSSPass->SetSceneFramebuffer(s_Data.ScenePass->GetTarget());
s_Data.PostProcessPass->SetInputFramebuffer(s_Data.ScenePass->GetTarget());
s_Data.PostProcessPass->SetSceneDepthFramebuffer(s_Data.ScenePass->GetTarget());
```

The graph's `Execute()` also calls `SetInputFramebuffer()` from its connection map, so some passes get their input set **twice**. The manual calls override the graph's piping, making the graph connections partially ceremonial.

**Files:**
- `OloEngine/src/OloEngine/Renderer/Renderer3D.cpp` lines 1793–1804 (SetupRenderGraph)
- `OloEngine/src/OloEngine/Renderer/Renderer3D.cpp` lines 486–526 (EndScene)
- `OloEngine/src/OloEngine/Renderer/RenderGraph.cpp` lines 117–135 (Execute piping)

### Proposed Fix

**Extend `RenderGraph` to handle all resource piping declaratively:**

Add named resource slots to `ConnectPass` so the graph knows which specific textures to pipe:

```cpp
// Declare what each pass produces / consumes
renderGraph->DeclareOutput("ScenePass", "Color", 0);      // color attachment 0
renderGraph->DeclareOutput("ScenePass", "EntityID", 1);    // color attachment 1
renderGraph->DeclareOutput("ScenePass", "Normals", 2);     // color attachment 2
renderGraph->DeclareOutput("ScenePass", "Depth", -1);      // depth attachment

renderGraph->ConnectResource("ScenePass", "Depth",   "SSAOPass",        "SceneDepth");
renderGraph->ConnectResource("ScenePass", "Normals", "SSAOPass",        "SceneNormals");
renderGraph->ConnectResource("SSAOPass",  "SSAO",    "PostProcessPass", "SSAOTexture");
renderGraph->ConnectResource("ScenePass", "Color",   "PostProcessPass", "InputColor");
renderGraph->ConnectResource("ScenePass", "Depth",   "PostProcessPass", "SceneDepth");
```

Each pass declares its input slots:

```cpp
class SSAORenderPass : public RenderPass
{
    void BindInputs(const ResourceBindings& inputs) override
    {
        m_SceneDepthID = inputs.GetTextureID("SceneDepth");
        m_SceneNormalsID = inputs.GetTextureID("SceneNormals");
    }
};
```

**Remove all manual `SetSceneFramebuffer()` / `SetInputFramebuffer()` / `SetSceneDepthFramebuffer()` calls from `EndScene()`**. The graph does all piping in `Execute()` before calling each pass.

**Remove all `Set*Framebuffer` methods** from individual passes once the graph handles resource binding.

---

## Issue 5: In-Place Framebuffer Modification Breaks the Graph Model

### Problem

`ParticleRenderPass` and `SSSRenderPass` render into the **scene framebuffer** rather than their own target. Their `GetTarget()` returns the scene FB. This means:

- The render graph's framebuffer piping from `ConnectPass("ParticlePass", "SSSPass")` is meaningless — it pipes the scene FB to a pass that already holds the scene FB.
- `SSSRenderPass` doesn't override `SetInputFramebuffer()` so the graph's piping call is a no-op.
- These passes are invisible to the graph's data flow — they appear as nodes but the graph can't reason about their actual I/O.

Additionally, the SSS pass must use `glCopyImageSubData` to create a staging copy to avoid a read-write hazard, because it reads and writes the same framebuffer attachment.

**Files:**
- `OloEngine/src/OloEngine/Renderer/Passes/ParticleRenderPass.cpp` — `GetTarget()` returns scene FB
- `OloEngine/src/OloEngine/Renderer/Passes/SSSRenderPass.cpp` — in-place rendering + staging copy

### Proposed Fix

**Give each pass its own output framebuffer:**

**ParticleRenderPass:**

Instead of rendering into the scene FB, the particle pass should:
1. Receive the scene color + depth as inputs (via graph resource piping).
2. Copy the scene color to its own framebuffer.
3. Render particles with depth testing against the scene depth (read-only).
4. Output its framebuffer as the new "combined" result.

Alternatively, keep the in-place pattern but **model it correctly in the graph** by marking the pass as "in-place modifier" of the scene FB resource, so the graph knows no separate output is produced.

**SSSRenderPass:**

Give it its own output framebuffer (same format as scene color: RGBA16F). It reads the scene color from an input texture and writes the blurred result to its output. This eliminates `glCopyImageSubData` entirely:

```cpp
void SSSRenderPass::Execute()
{
    if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
    {
        // Passthrough: GetTarget() returns input directly
        return;
    }

    m_Target->Bind();
    RenderCommand::Clear();
    m_SSSBlurShader->Bind();
    // Bind scene color (input) as texture — no read-write hazard
    RenderCommand::BindTexture(0, m_InputColorID);
    RenderCommand::BindTexture(TEX_POSTPROCESS_DEPTH, m_InputDepthID);
    DrawFullscreenTriangle();
    m_Target->Unbind();
}

Ref<Framebuffer> SSSRenderPass::GetTarget() const
{
    // Passthrough when disabled
    if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
        return m_InputFramebuffer;
    return m_Target;
}
```

**Remove `glCopyImageSubData`, `glDrawBuffers` direct calls, and the staging texture code.**

---

## Issue 6: Duplicate Fullscreen Triangle Creation (4 Copies)

### Problem

The exact same fullscreen triangle geometry (structure definition, vertex data, index data, VAO creation, `DrawFullscreenTriangle()` helper) is copy-pasted in 4 files:

1. `FinalRenderPass.cpp` lines 31–63
2. `PostProcessRenderPass.cpp` lines 30–58
3. `SSAORenderPass.cpp` lines 40–68
4. `SSSRenderPass.cpp` lines 35–62

Each creates its own VAO and stores its own `Ref<VertexArray> m_FullscreenTriangleVA`.

**Files:**
- All four `.cpp` files listed above
- All four `.h` files (each declares `m_FullscreenTriangleVA` and `DrawFullscreenTriangle()`)

### Proposed Fix

**Option A — Shared static utility:**

Add to `MeshPrimitives`:

```cpp
// MeshPrimitives.h
class MeshPrimitives
{
public:
    // ... existing methods ...
    static Ref<VertexArray> CreateFullscreenTriangle();
};
```

Passes call `MeshPrimitives::CreateFullscreenTriangle()` in `Init()` instead of inline creation.

**Option B — `FullscreenRenderPass` base class (preferred):**

```cpp
class FullscreenRenderPass : public RenderPass
{
public:
    FullscreenRenderPass();

protected:
    void DrawFullscreenTriangle();

private:
    // Shared across all instances via static lazy init
    static Ref<VertexArray> s_FullscreenTriangleVA;
    static void EnsureFullscreenTriangle();
};
```

`SSAORenderPass`, `SSSRenderPass`, `PostProcessRenderPass`, and `FinalRenderPass` all inherit from `FullscreenRenderPass`. Remove the per-pass fullscreen triangle code entirely.

---

## Issue 7: Raw OpenGL Calls in Render Passes

### Problem

Several passes use raw `gl*` calls that bypass the `RenderCommand` / `RendererAPI` abstraction:

| Pass | Raw GL call | Purpose |
|------|------------|---------|
| `SSSRenderPass` | `glCopyImageSubData()` | Copy scene FB to staging texture |
| `SSSRenderPass` | `glDrawBuffers()` | Restrict draw to attachment 0 only |
| `SSSRenderPass` | `glCreateTextures()`, `glTextureStorage2D()`, `glTextureParameteri()` | Staging texture creation |
| `SSAORenderPass` | `glCreateTextures()`, `glTextureStorage2D()`, `glTextureSubImage2D()`, `glTextureParameteri()` | Noise texture creation |
| `ParticleRenderPass` | `glEnablei(GL_BLEND, 0)`, `glDisablei(GL_BLEND, 1/2)` | Per-attachment blend control |

These calls:
- Create a **platform portability barrier** (no Vulkan/Metal path).
- Bypass the **render state tracking** in `CommandDispatch`.
- Are inconsistent with the rest of the codebase that uses `RenderCommand::`.

**Files:**
- `SSSRenderPass.cpp` — 12 raw GL calls
- `SSAORenderPass.cpp` — 8 raw GL calls
- `ParticleRenderPass.cpp` — 5 raw GL calls

### Proposed Fix

**Add missing abstractions to `RenderCommand` / `RendererAPI`:**

```cpp
// RendererAPI.h
virtual void CopyImageSubData(u32 srcID, u32 srcTarget, u32 dstID, u32 dstTarget,
                               u32 width, u32 height) = 0;
virtual void SetDrawBuffers(std::span<const u32> attachments) = 0;
virtual void SetBlendStatePerAttachment(u32 attachment, bool enabled) = 0;

// RenderCommand.h (static forwarding)
static void CopyImageSubData(...) { s_RendererAPI->CopyImageSubData(...); }
static void SetDrawBuffers(...) { s_RendererAPI->SetDrawBuffers(...); }
static void SetBlendStatePerAttachment(...) { s_RendererAPI->SetBlendStatePerAttachment(...); }
```

**For texture creation** (noise texture, staging texture):
- Use the existing `Texture2D::Create()` API with appropriate format specifications.
- If the current `Texture2D` API doesn't support `RG16F` or raw float data uploads, extend the `TextureSpecification` to handle it.

**Replace all raw `gl*` calls** with the new `RenderCommand::` wrappers.

---

## Issue 8: SSS Pass Read-Write Hazard Workaround

### Problem

`SSSRenderPass::Execute()` reads from and writes to the same scene framebuffer color attachment. To avoid undefined behavior, it copies the color attachment to a staging texture via `glCopyImageSubData`:

```cpp
// Copy scene color to staging texture to avoid read-write hazard.
glCopyImageSubData(
    colorID, GL_TEXTURE_2D, 0, 0, 0, 0,
    m_StagingTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
    width, height, 1);
```

This is a GPU-side copy every frame, wasteful when the root cause is the in-place rendering pattern.

**Files:**
- `SSSRenderPass.cpp` lines 91–100 — staging copy
- `SSSRenderPass.cpp` lines 156–190 — staging texture management

### Proposed Fix

This is resolved by **Issue 5's fix** — giving SSS its own output framebuffer. When the pass reads from an input texture and writes to a separate output, there is no read-write hazard and no staging copy needed.

---

## Issue 9: Per-Frame Callback Allocation and Stale Reference Risks

### Problem

Both `ShadowRenderPass` and `ParticleRenderPass` clear their `std::function` callbacks at the end of `Execute()`:

```cpp
m_RenderCallback = nullptr; // "Clear callback to prevent stale captures"
```

This means `Scene.cpp` must re-create and re-assign the closure **every frame**, which:
1. **Allocates heap memory** for the `std::function` closure each frame (captures `this`, `camera` reference).
2. Creates **dangling reference risk** — the callbacks capture `camera` by reference. If execution were ever deferred or asynchronous, these references would be invalid.
3. **Couples `Scene` tightly to render pass internals** — `Scene` directly reaches into `Renderer3D::GetShadowPass()` / `GetParticlePass()` to set callbacks, bypassing the command recording abstraction.

**Files:**
- `Scene.cpp` lines 1525, 2554, 2720 — callback assignment
- `ShadowRenderPass.cpp` line 153 — callback cleared
- `ParticleRenderPass.cpp` line 56 — callback cleared

### Proposed Fix

**Short-term — Use persistent callbacks with per-frame data:**

Instead of recreating lambdas, set the callback once during scene initialization and pass per-frame data via a shared context struct:

```cpp
// Set once
shadowPass->SetRenderCallback([](const ShadowFrameContext& ctx)
{
    // Uses ctx instead of captured references
    for (const auto& meshDesc : ctx.ShadowCasters)
    {
        // ... render ...
    }
});

// Each frame — pass data, not a new lambda
ShadowFrameContext ctx;
ctx.LightVP = lightVP;
ctx.ShadowCasters = CollectShadowCasters();
shadowPass->SetFrameContext(ctx);
```

**Long-term — Eliminate callbacks entirely:**

Per Issues 2 and 3, shadow and particle rendering should submit command packets. Once they do, there's no need for callbacks at all. The passes own their command buckets and dispatch them internally.

---

## Implementation Priority & Sequencing

| Priority | Issue | Effort | Impact |
|----------|-------|--------|--------|
| **P0** | #6 — Extract shared fullscreen triangle | Small | Eliminates code duplication, enables other refactors |
| **P0** | #1 — Split `RenderPass` base class | Small | Eliminates wasted allocations, clarifies pass taxonomy |
| **P1** | #7 — Wrap raw GL calls in `RenderCommand` | Medium | Platform portability, consistent state tracking |
| **P1** | #5 — Give SSS/Particles own framebuffers | Medium | Fixes graph model, eliminates staging copy hack |
| **P1** | #4 — Consolidate framebuffer piping in graph | Medium | Single source of truth for pass I/O |
| **P2** | #2 — Terrain as command packets | Large | Terrain participates in sorting/batching/profiling |
| **P2** | #9 — Eliminate per-frame callback allocation | Medium | Cleaner data flow, no dangling reference risk |
| **P3** | #3 — Shadow pass command reuse | Large | Eliminates duplicate entity traversal, enables shadow instancing |
| **P3** | #8 — SSS read-write hazard | None | Resolved automatically by Issue #5 |

### Suggested Implementation Order

1. **Phase 1 (Foundation):** Issues #6, #1
   - Create `FullscreenRenderPass` base and `CommandBufferRenderPass`.
   - All passes updated to correct base class. No behavioral changes yet.

2. **Phase 2 (Graph correctness):** Issues #5, #4, #7
   - SSS and Particle passes get own framebuffers.
   - Graph resource piping replaces manual wiring.
   - Raw GL calls wrapped.

3. **Phase 3 (Feature integration):** Issues #2, #9, #3
   - Terrain/foliage/voxel submit through command bucket.
   - Callbacks replaced with command packets.
   - Shadow pass reuses scene command data.

---

## Architectural Diagram

### Current State (after all three phases)

```
Scene.cpp
  └── Entity traversal (single pass)
        ├── DrawMesh()           → ScenePass.CommandBucket
        ├── DrawAnimatedMesh()   → ScenePass.CommandBucket
        ├── DrawTerrainPatch()   → ScenePass.CommandBucket
        ├── DrawVoxelMesh()      → ScenePass.CommandBucket
        ├── AddMeshCaster()      → ShadowPass caster list
        ├── AddSkinnedCaster()   → ShadowPass caster list (reuses bone offsets)
        ├── AddTerrainCaster()   → ShadowPass caster list
        ├── AddVoxelCaster()     → ShadowPass caster list
        ├── AddFoliageCaster()   → ShadowPass caster list
        └── PostExecuteCallback  → FoliageRenderer (self-batching subsystem)

RenderGraph::Execute()  ← single source of truth for ordering + I/O piping
  ├── ShadowPass (RenderPass)
  │     └── Iterates caster lists per cascade/face with depth shaders
  ├── ScenePass (CommandBufferRenderPass)
  │     └── bucket.Sort() + bucket.Execute() + PostExecuteCallback (foliage only)
  ├── SSAOPass (FullscreenRenderPass)
  │     └── DrawFullscreenTriangle() — reads scene depth/normals, writes own FB
  ├── ParticlePass
  │     └── Callback (self-batching subsystem, appropriate)
  ├── SSSPass (FullscreenRenderPass)
  │     └── DrawFullscreenTriangle() — reads scene color input, writes own FB
  ├── PostProcessPass (FullscreenRenderPass)
  │     └── Ping-pong chain with DrawFullscreenTriangle()
  └── FinalPass (FullscreenRenderPass)
        └── DrawFullscreenTriangle() — blits to default FB
```
