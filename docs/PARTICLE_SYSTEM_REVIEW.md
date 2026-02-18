# Particle System — Design Review & Issues

> **Date**: February 2026
> **Scope**: Full review of the OloEngine particle system after Phase 1–3 implementation.
> Covers design problems, bugs, missing features, and comparison to production engines.

---

## Summary

The particle system has a solid foundation: SOA pool with swap-to-back death, modular
modifier pipeline, sub-emitters with independent child systems, ring-buffer trails, and
comprehensive editor UI. However, **the 3D rendering integration has fundamental problems**
that make particles unusable in real 3D scenes, and several bugs affect correctness when
modules are combined. This document catalogues every issue found, ordered by severity.

---

## 1 — Critical: Rendering Architecture

The particle system's 3D rendering integration has **fundamental architectural problems**,
not just minor state issues. Particles are rendered entirely outside the Renderer3D render
graph, through the wrong renderer, with incorrect pipeline ordering.

### 1.1 Particles render outside the render graph

**Files**: `Scene.cpp` — `RenderScene3D(EditorCamera)` (L1658-L1729),
`RenderScene3D(Camera, mat4)` (L1731+), `OnUpdateRuntime` (L576-L700)

**The render graph pipeline**:
```
Renderer3D::BeginScene()
  ↓ Scene submits command packets (meshes, skybox, gizmos)
Renderer3D::EndScene()
  ↓ RenderGraph::Execute()
      ↓ SceneRenderPass::Execute()  — binds ScenePass FB, sorts+executes commands, unbinds
      ↓ FinalRenderPass::Execute()  — binds default FB (0), blits ScenePass color to screen
  ↓ (GL is now on default framebuffer 0)
```

**After the render graph completes**, Scene.cpp manually:
```cpp
// Re-bind ScenePass FB (because FinalPass left us on FB 0)
scenePass->GetTarget()->Bind();
RenderCommand::SetDepthTest(false);
RenderCommand::SetDepthMask(false);

// Draw particles via Renderer2D into the ScenePass framebuffer
Renderer2D::BeginScene(camera);
// ... ParticleRenderer::RenderParticlesBillboard(), TrailRenderer, etc ...
Renderer2D::EndScene();

RenderCommand::SetDepthTest(true);
RenderCommand::SetDepthMask(true);
RenderCommand::BindDefaultFramebuffer();
```

Particles are **bolted on after the render graph has finished**. They are not a render
pass. They don't participate in command sorting, profiling, or the render graph debugger.

### 1.2 FinalPass blits to screen BEFORE particles are drawn

`Renderer3D::EndScene()` calls `RGraph->Execute()`, which runs FinalRenderPass — this
blits the ScenePass color attachment to the default framebuffer (the screen). **This
happens before any particle is drawn.**

Then particles are drawn into the ScenePass framebuffer after the blit.

**Consequence in the editor**: Works by accident. The editor viewport (`EditorLayer.cpp`)
reads the ScenePass texture directly via `ImGui::Image(scenePass->GetColorAttachment(0))`
at the end of the frame — so it picks up the particles that were drawn after `EndScene`.

**Consequence in standalone runtime** (no editor/ImGui): FinalPass already blitted the
ScenePass to screen **without particles**. The particles drawn afterwards go into the
ScenePass texture but are **never shown on screen**. Particles would be invisible in any
non-editor runtime window.

### 1.3 Particles go through Renderer2D (wrong renderer for 3D)

All particle rendering — billboards, stretched billboards, trails — calls
`Renderer2D::DrawQuad()` and `Renderer2D::DrawPolygon()`. This is a layer violation:

- **Renderer2D has no concept of depth** — its shader doesn't read a depth buffer.
  Enabling `SetDepthTest(true)` before Renderer2D calls would require Renderer2D's
  vertex shader to output correct clip-space Z values (it does, since it applies the VP
  matrix, but this was never the intended path and has not been validated).
- **No custom shader support** — Renderer2D uses a fixed batch shader. You can't add
  depth-fade (soft particles), per-particle lighting, or normal mapping.
- **No instanced rendering** — each particle is a separate `DrawQuad()` call into the
  batch. The CPU computes a full 4×4 matrix per particle.
- **State management conflicts** — Renderer2D manages its own GL state (blend, shader
  binds). Calling `glBlendFunc()` between `DrawQuad()` calls inside an active batch may
  not take effect until the batch flushes (see §1.5).

### 1.4 Particles ignore the 3D depth buffer

Depth testing is explicitly **disabled** before particles render:
```cpp
RenderCommand::SetDepthTest(false);  // ← particles can't be occluded
RenderCommand::SetDepthMask(false);
```

Every particle renders on top of every mesh, regardless of 3D position. A particle behind
a wall, inside a building, or underground is still visible.

**Note**: Even a quick fix of `SetDepthTest(true)` is not straightforward because:
1. Particles are drawn after `Renderer3D::EndScene()`, which resets GL state.
2. The ScenePass framebuffer's depth attachment still contains valid depth from the
   opaque pass, so depth reads *would* work if re-enabled.
3. But Renderer2D's shader hasn't been validated for correct depth output in this
   configuration, and the per-particle transform matrices constructed in
   `ParticleRenderer` must produce correct clip-space Z values.
4. Even if depth reads work, particles still render after FinalPass — fixing the
   display-order problem (§1.2) requires more than a state toggle.

### 1.5 Blend state changes during an active Renderer2D batch

**File**: `Scene.cpp` — multiple call sites.

`SetParticleBlendMode()` calls `glBlendFunc()` between individual `Renderer2D::DrawQuad()`
calls. If `Renderer2D` batches quads internally (which it does), `glBlendFunc()` only
takes effect when the batch is actually flushed. Particles within the same batch that
span a blend-mode change may render with the wrong blend mode.

**Fix**: Call `Renderer2D::Flush()` (or `EndScene()` / `BeginScene()`) before changing
blend state, or submit each blend mode group in its own batch.

### 1.6 No soft particles (depth fade near surfaces)

When particles intersect opaque geometry they produce hard, ugly edges. Production engines
fade the particle alpha based on the distance between the particle's depth and the scene
depth buffer value at that pixel.

**Requires**: A particle-specific fragment shader with a depth texture uniform. Cannot be
done through Renderer2D's fixed shader.

### 1.7 No inter-system depth sorting

Each `ParticleSystem` sorts its own particles back-to-front, but if two particle systems
overlap spatially, there's no sorting between them. Alpha-blended particles from system A
may incorrectly render over particles from system B.

**Fix (future)**: Collect all particles from all systems into a single sort pass before
rendering, or at least sort systems by their emitter distance to camera.

### 1.8 The proper fix: ParticleRenderPass

The correct solution is to create a **`ParticleRenderPass`** that integrates into the
existing render graph between SceneRenderPass and FinalRenderPass:

```
SceneRenderPass (opaque geometry, skybox, gizmos)
      ↓ outputs: color + depth in ScenePass framebuffer
ParticleRenderPass (translucent particles, trails)  ← NEW
      ↓ reads ScenePass depth (for occlusion + soft particles)
      ↓ writes color into ScenePass (or its own target)
FinalRenderPass (blit to screen)
```

**What the ParticleRenderPass needs**:

1. **Shared framebuffer access**: Render into the ScenePass framebuffer (or a separate
   translucent target composited later). Must read the ScenePass depth attachment as a
   texture for occlusion and soft-particle fade.

2. **A dedicated particle billboard shader**:
   - Vertex shader: constructs camera-facing billboard from per-particle attributes
     (position, size, rotation, UV rect). No CPU matrix construction.
   - Fragment shader: texture sampling, alpha blending, depth buffer read for
     soft-particle fade, optional sprite sheet UV selection.

3. **Instance buffer or SSBO for per-particle data**: Upload positions, sizes, colors,
   rotations, UV rects for all particles of one texture in a single buffer. One instanced
   draw call per texture batch instead of one `DrawQuad()` per particle.

4. **Blend mode management**: Handle Alpha/Additive/Premultiplied blend modes as
   render-pass state, not raw `glBlendFunc()` calls mid-batch.

5. **Trail rendering via triangle strip**: One draw call per particle trail (or one
   for all trails of a system) using a dynamic vertex buffer with position + width + UV.

**Implementation phases**:

- **Phase A (minimum viable)**: Create `ParticleRenderPass`, insert into render graph.
  Render into ScenePass FB with depth test enabled (read-only). Still use per-particle
  quads (no instancing yet). This fixes §1.1–§1.4 and §1.2 display ordering.

- **Phase B (particle shader)**: Add a dedicated particle vertex/fragment shader.
  Billboard construction in vertex shader. Depth texture read in fragment shader for
  soft particles.

- **Phase C (instanced rendering)**: Per-particle data via instance buffer. One draw
  call per texture batch. This is the performance win.

---

## 2 — Bugs

### 2.1 VelocityOverLifetime overwrites physics forces

**File**: `ParticleModules.cpp`, `ModuleVelocityOverLifetime::Apply()`

```cpp
pool.Velocities[i] = pool.InitialVelocities[i] * speedMul + LinearVelocity * elapsedTime;
```

This **replaces** the velocity every frame. Even though VelocityOverLifetime runs first
(before Gravity, Drag, Noise), the forces applied in frame N are discarded in frame N+1
when the velocity is reconstructed from initial values. In practice:

- Gravity appears to work for one frame, then gets overwritten.
- Drag has no cumulative effect.
- Noise jitter is erased each frame.

Users cannot meaningfully combine VelocityOverLifetime with any force-based module.

**Fix options**:
1. Make VelocityOverLifetime additive: `Velocities[i] += delta` per frame instead of
   absolute reconstruction.
2. Separate "velocity damping curve" from "additive linear velocity" — apply the curve
   as a multiplier on top of whatever the current velocity is (after forces), not as a
   replacement.
3. Skip force modules when VelocityOverLifetime is enabled (Unity's approach — velocity
   curve overrides physics).

### 2.2 `OnCollision` sub-emitter event is defined but never fires

**File**: `SubEmitter.h`

`SubEmitterEvent::OnCollision` exists in the enum, but `CollisionModule::Apply()` and
`CollisionModule::ApplyWithRaycasts()` never create `SubEmitterTriggerInfo` entries.
Collision events are silently dropped.

**Fix**: In `CollisionModule`, when a particle bounces or is killed, check for
`OnCollision` sub-emitter entries and push trigger info into the pending triggers list.

### 2.3 LOD uses stepped thresholds instead of smooth interpolation

**File**: `ParticleSystem.cpp`, `UpdateLOD()`

```cpp
if (dist >= LODMaxDistance)       m_LODSpawnRateMultiplier = 0.0f;
else if (dist >= LODDistance2)    m_LODSpawnRateMultiplier = 0.25f;
else if (dist >= LODDistance1)    m_LODSpawnRateMultiplier = 0.5f;
else                              m_LODSpawnRateMultiplier = 1.0f;
```

Particles visibly pop in/out at the distance boundaries. Should use smooth linear or
inverse-square falloff between the thresholds.

### 2.4 Emission rate temporarily mutated during Update

**File**: `ParticleSystem.cpp`, `Update()` (L97-L104)

```cpp
f32 origRate = Emitter.RateOverTime;
Emitter.RateOverTime *= m_LODSpawnRateMultiplier;
// ... emit ...
Emitter.RateOverTime = origRate;
```

This mutates then restores a public member. If anything reads `RateOverTime` concurrently
(e.g., editor UI refresh on another thread, or a script query), it sees the wrong value.
The multiplier should be passed as a parameter or computed internally.

### 2.5 Warm-up recursion depth

**File**: `ParticleSystem.cpp`, `Update()`

Warm-up calls `Update()` recursively in a loop:
```cpp
while (remaining > 0.0f)
{
    Update(step, emitterPosition, parentVelocity);
    remaining -= step;
}
```

With `WarmUpTime = 10.0f` and `warmUpStep = 1/60`, that's 600 recursive calls. Each call
pushes a stack frame with several locals + profile markers. This is a stack overflow risk
for large warm-up times and should be converted to an iterative approach (loop with an
internal flag to distinguish warm-up ticks from real ticks).

---

## 3 — Design Issues

### 3.1 All rendering goes through Renderer2D

**Every** render path — `RenderParticles2D`, `RenderParticlesBillboard`,
`RenderParticlesStretched`, `TrailRenderer::RenderTrails` — uses `Renderer2D::DrawQuad()`
or `Renderer2D::DrawPolygon()`.

This couples particles to the 2D batch pipeline, which:
- Has no concept of depth or lighting.
- Doesn't support custom shaders (needed for soft particles, lit particles, distortion).
- Cannot do instanced rendering (each quad goes through the normal batch path).
- Means particles and 3D geometry live in completely separate pipelines.

Long-term, particles need their own render pass within `Renderer3D` with a dedicated
particle shader that supports:
- Reading the scene depth buffer (for depth test + soft particles).
- Per-particle instance data (position, size, color, rotation, UV rect) via SSBO or
  instance buffer — one draw call per texture.
- Optional lighting (normal-mapped particles, lit smoke).

### 3.2 Per-particle CPU transform construction is expensive

**File**: `ParticleRenderer.cpp`

`RenderParticlesBillboard()` computes a 4×4 matrix per particle, including `cos()` and
`sin()` calls for rotation:

```cpp
f32 cosR = std::cos(rotation);
f32 sinR = std::sin(rotation);
```

With 10K particles, that's 10K trig calls + 10K matrix constructions per frame on the
CPU. GPU instancing would eliminate this entirely — upload raw per-particle data and let
the vertex shader handle billboard construction.

### 3.3 Trail rendering is O(particles × trail_points) separate draw calls

**File**: `TrailRenderer.cpp`

Each trail *segment* is a separate `Renderer2D::DrawPolygon()` call. With 1000 particles,
each having 16 trail points = 15 segments, that's **15,000 individual polygon calls per
frame**. This will dominate frame time.

**Fix**: Submit each particle's entire trail as a single triangle strip or indexed triangle
list. Better yet, batch all trails into a single draw call with a dynamic vertex buffer.

### 3.4 Only one force field per system

`ModuleForceField` is a single struct on `ParticleSystem`. Real effects need multiple
force fields (wind zone + point attractor + vortex turbulence). Unity uses external
ForceField components that affect any particle system in range.

**Fix**: Change `ForceFieldModule` to a `std::vector<ModuleForceField>` or introduce
an entity-based force field component that the particle update queries.

### 3.5 Emission direction ignores entity rotation

`SampleEmissionShape()` generates positions/directions in a fixed coordinate frame. The
entity's `TransformComponent.Rotation` is never applied to the emission direction. A cone
emitter always points along +Z regardless of entity orientation.

**Fix**: In `ParticleEmitter::InitializeParticle()`, transform the sampled direction by
the entity's rotation quaternion. The entity rotation needs to be passed through from
`Scene.cpp` to the emitter.

### 3.6 ParticleCurve find-segment is O(n) linear scan

**File**: `ParticleCurve.h`, `Evaluate()`

With 8 keys the loop is fine, but the access pattern (called per particle per frame per
module) means it's executed hundreds of thousands of times. A binary search or precomputed
LUT (sample curve into 256 values on change) would be faster for hot paths.

### 3.7 `std::sort` for depth sorting is not stable and allocates

**File**: `ParticleSystem.cpp`, `SortByDepth()`

`std::sort` may allocate and is O(n log n). For particles that are mostly already sorted
frame-to-frame, an **insertion sort** or `pdqsort` would be significantly faster due to
adaptive behavior on nearly-sorted data. This is a well-known optimization in particle
engines.

---

## 4 — Missing Features (Priority Order)

### 4.1 HIGH — ParticleRenderPass (proper 3D integration)
The entire rendering architecture needs to change. Particles must be a proper render pass
in the Renderer3D render graph, not a post-hoc Renderer2D hack. See §1.8 for the full
design. This is the **single biggest blocker** for the particle system.

Phased approach:
- **Phase A**: Create `ParticleRenderPass`, insert between ScenePass and FinalPass.
  Render into ScenePass FB with depth reads. Still per-particle quads. Fixes depth
  occlusion and display ordering.
- **Phase B**: Dedicated particle billboard shader (vertex shader billboarding, fragment
  shader depth-fade for soft particles).
- **Phase C**: Instance buffer for per-particle data. One draw call per texture batch.

### 4.2 HIGH — Instanced particle rendering
Replace per-particle `Renderer2D::DrawQuad()` with a single instanced draw call per
texture batch. Upload per-particle data (pos, size, color, rotation, UV) to an instance
buffer or SSBO. Part of Phase C of the ParticleRenderPass work (§4.1). Requires:
- A particle-specific vertex + fragment shader (billboard construction in vertex shader).
- Dynamic instance buffer management (map/orphan pattern or persistent mapped buffer).

### 4.3 HIGH — Curve editor UI
`ParticleCurve` supports 8 keyframes, but the editor UI only has enable/disable
checkboxes for ColorOverLifetime and SizeOverLifetime. No way to add/remove/move keys
or see the curve shape. Without this, the curve system is effectively unusable from
the editor.

### 4.4 MEDIUM — Soft particles
Alpha-fade particles near opaque surfaces using depth buffer comparison. Requires depth
texture access in the particle fragment shader. Part of Phase B of the ParticleRenderPass
work (§4.1). See §1.6.

### 4.5 MEDIUM — Mesh particles
`ParticleRenderMode::Mesh` is declared but not implemented. Each particle should instance
a user-specified mesh (e.g., debris chunks, leaves). Requires instanced rendering (§4.2).

### 4.6 MEDIUM — Task system parallelization
Independent modules (Gravity, Drag, Noise, Rotation) can run in parallel across particle
ranges. The engine already has a task system — split the alive range into chunks and
dispatch per-module tasks.

### 4.7 MEDIUM — Multiple force fields
Change `ModuleForceField` from a single instance to a collection, or introduce an
entity-based `ForceFieldComponent` that the particle system queries at update time.

### 4.8 MEDIUM — Emission direction from entity rotation
Apply the entity's rotation to emission direction so cone/edge shapes orient with the
entity. See §3.5.

### 4.9 LOW — GPU compute simulation
Move position integration and module application to compute shaders. Requires SSBO and
compute shader support in the renderer (not yet available). This would unlock 100K–1M+
particle counts.

### 4.10 LOW — Particle lights
Per-particle point lights that affect the 3D scene. Expensive, so typically limited to
the N brightest/nearest particles. Unity's Shuriken has this; useful for fire, explosions.

### 4.11 LOW — Custom vertex streams
Let users define which per-particle attributes are passed to the shader (tangent, custom
data, etc.). Enables advanced shader effects.

### 4.12 LOW — OnCollision sub-emitter trigger
Wire up the existing `SubEmitterEvent::OnCollision` enum to actually fire. See §2.2.

### 4.13 LOW — Trail texture coordinate mapping
Trails currently have no UVs — they render as solid colored quads via `DrawPolygon()`.
Add UV coordinates along the trail length (0→1) so trail textures (flame streaks, etc.)
work correctly.

### 4.14 LOW — Inter-system sorting
Sort all particle systems (and ideally all translucent objects) by distance to camera
before rendering to improve visual correctness of overlapping alpha-blended systems.

---

## 5 — Comparison to Unity / Unreal

| Feature | Unity Shuriken | Unreal Niagara | OloEngine | Gap |
|---|---|---|---|---|
| CPU simulation | ✅ | ✅ | ✅ | — |
| GPU simulation (compute) | ✅ (VFX Graph) | ✅ | ❌ | Blocked on SSBO/compute |
| Depth buffer occlusion | ✅ | ✅ | ❌ | **Critical** — requires ParticleRenderPass in render graph |
| Soft particles (depth fade) | ✅ | ✅ | ❌ | Need depth texture in particle shader |
| Instanced rendering | ✅ | ✅ | ❌ | Planned (Phase 3 #9) |
| Curve editor | ✅ (rich spline) | ✅ (graph editor) | ❌ | No UI at all for curves |
| Multiple force fields | ✅ (external) | ✅ (modules) | ❌ (one per system) | Design limitation |
| Mesh particles | ✅ | ✅ | ❌ (placeholder) | Needs instancing first |
| Sub-emitters | ✅ | ✅ | ✅ | Working — separate child systems |
| Trails | ✅ | ✅ | ✅ | Working — quad-strip, but slow (per-segment draw call) |
| Emission shapes | ✅ (Mesh surface) | ✅ | Partial (6 shapes) | Missing: mesh surface emit |
| Events/callbacks | OnCollision, OnTrigger | Full scripting | Partial (OnBirth/OnDeath) | OnCollision not wired |
| Particle lights | ✅ | ✅ | ❌ | Low priority |
| Texture atlas / sprite sheet | ✅ | ✅ | ✅ | Working |
| LOD | ✅ | ✅ | ✅ | Working (but stepped, not smooth) |
| Warm-up | ✅ | ✅ | ✅ | Working (but recursive, stack risk) |
| Velocity inheritance | ✅ | ✅ | ✅ | Working |
| Serialization | ✅ | ✅ | ✅ | YAML |
| .particle asset files | ✅ (.prefab) | ✅ (.uasset) | ✅ | .oloparticle |

**Overall assessment**: OloEngine's particle system is architecturally comparable to
Unity's Shuriken circa 2015–2016 in terms of simulation. The SOA data layout, module
pipeline, sub-emitters, and trail ring buffers are well-designed. The critical gap is
rendering integration — particles are rendered outside the render graph through the
wrong renderer (Renderer2D), after the final blit has already occurred. A new
`ParticleRenderPass` integrated into the render graph is required. After that, instanced
rendering and a particle-specific shader are needed for performance and visual quality.

---

## 6 — Recommended Fix Order

### Rendering architecture overhaul (the real fix)

1. **Create `ParticleRenderPass`** (§1.8, Phase A) — new render pass inserted into the
   render graph between ScenePass and FinalPass. Renders into the ScenePass framebuffer
   with depth test enabled (read-only). This fixes:
   - §1.1 (particles outside render graph)
   - §1.2 (FinalPass blits before particles)
   - §1.4 (no depth occlusion)
   Must port existing `ParticleRenderer` logic into the new pass, still using per-particle
   quads via Renderer2D initially. Can optimize to instanced rendering later.

2. **Fix blend state batching** (§1.5) — flush Renderer2D batch before blend mode changes.

### Bug fixes (can be done in parallel)

3. **Fix VelocityOverLifetime vs forces** (§2.1) — either make additive or skip forces
   when VelocityOverLifetime is enabled.
4. **Convert warm-up to iterative** (§2.5) — prevent stack overflow.
5. **Smooth LOD interpolation** (§2.3) — remove popping.
6. **Pass emission rate multiplier as parameter** (§2.4) — don't mutate public state.
7. **Wire OnCollision sub-emitter event** (§2.2).
8. **Apply entity rotation to emission direction** (§3.5).

### Editor + quality (after architecture is stable)

9. **Add curve editor UI** (§4.3) — unblocks artist workflow.
10. **Optimize trail rendering** (§3.3) — batch into single draw call per particle.
11. **Particle billboard shader** (§1.8, Phase B) — vertex-shader billboarding, soft
    particle depth fade in fragment shader.
12. **Instanced rendering** (§1.8, Phase C) — instance buffer, one draw call per texture.
