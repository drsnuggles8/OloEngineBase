# Particle System — Design Review & Issues

> **Date**: February 2026
> **Last updated**: February 2026 (ParticleRenderPass Phase A implemented)
> **Scope**: Full review of the OloEngine particle system after Phase 1–3 implementation.
> Covers design problems, bugs, missing features, and comparison to production engines.

---

## Summary

The particle system has a solid foundation: SOA pool with swap-to-back death, modular
modifier pipeline, sub-emitters with independent child systems, ring-buffer trails, and
comprehensive editor UI. All simulation-side bugs have been fixed (velocity module, LOD,
warm-up, emission rotation, force fields, sub-emitter collision events, curve evaluation,
depth sorting). **Rendering architecture** has been improved: `ParticleRenderPass` (Phase
A) integrates particles into the render graph with depth occlusion. The remaining gaps
are rendering performance (instanced rendering, dedicated particle shader) and editor
quality (curve editor UI, soft particles).

---

## 1 — Critical: Rendering Architecture

The particle system's 3D rendering integration has **fundamental architectural problems**,
not just minor state issues. Particles are rendered entirely outside the Renderer3D render
graph, through the wrong renderer, with incorrect pipeline ordering.

### ~~1.1 Particles render outside the render graph~~ ✅ FIXED

> **Fixed**: Particles now render inside the render graph via `ParticleRenderPass`,
> inserted between ScenePass and FinalPass. Particle rendering logic is set as a callback
> before `Renderer3D::EndScene()`, which the graph executes in the correct order.

### ~~1.2 FinalPass blits to screen BEFORE particles are drawn~~ ✅ FIXED

> **Fixed**: `ParticleRenderPass` executes between ScenePass and FinalPass. Particles
> are drawn into the ScenePass framebuffer *before* FinalPass blits to screen.
> Both editor (ImGui viewport) and standalone runtime paths display particles correctly.

### 1.3 Particles still go through Renderer2D

All particle rendering — billboards, stretched billboards, trails — still uses
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

### ~~1.4 Particles ignore the 3D depth buffer~~ ✅ FIXED

> **Fixed**: `ParticleRenderPass::Execute()` enables depth testing (`GL_LEQUAL`,
> read-only via `SetDepthMask(false)`) so particles are correctly occluded by opaque
> geometry. Depth writes remain disabled so particles don't occlude each other or later
> transparent objects.

### 1.5 ~~Blend state changes during an active Renderer2D batch~~ ✅ FIXED

> **Fixed**: `SetParticleBlendMode()` now calls `Renderer2D::Flush()` before changing
> GL blend state, ensuring queued quads render with the correct blend mode.

### 1.6 No soft particles (depth fade near surfaces)

When particles intersect opaque geometry they produce hard, ugly edges. Production engines
fade the particle alpha based on the distance between the particle's depth and the scene
depth buffer value at that pixel.

**Requires**: A particle-specific fragment shader with a depth texture uniform. Cannot be
done through Renderer2D's fixed shader.

### ~~1.7 No inter-system depth sorting~~ ✅ FIXED

> **Fixed**: Particle systems are now sorted back-to-front by distance to camera before
> rendering. Both editor and runtime render callbacks collect all `TransformComponent` +
> `ParticleSystemComponent` entities, compute squared distance to camera position, and
> render in farthest-first order. This ensures overlapping alpha-blended systems composite
> correctly.

### ~~1.8 The proper fix: ParticleRenderPass~~ ✅ FIXED (Phase A)

> **Fixed (Phase A)**: Created `ParticleRenderPass` class, inserted into the render graph
> between ScenePass and FinalPass. Graph order: ScenePass → ParticlePass → FinalPass.
> The pass renders into the ScenePass framebuffer with depth test enabled (read-only,
> `GL_LEQUAL`, no depth write) so particles correctly occlude against opaque geometry.
> Particle rendering moved from manual post-EndScene code into a render callback set
> before `Renderer3D::EndScene()`. Both editor and runtime paths updated. Phases B
> (dedicated particle shader) and C (instanced rendering) remain as future optimizations.

---

## 2 — Bugs (all fixed)

### 2.1 ~~VelocityOverLifetime overwrites physics forces~~ ✅ FIXED

> **Fixed**: `VelocityOverLifetime::Apply()` now preserves accumulated force contributions
> (gravity, drag, noise) by computing `forceContribution = Velocities - InitialVelocities`
> and applying the speed curve only to the initial component. Module execution order changed:
> forces (Gravity → Drag → Noise) run first, then VelocityModule scales on top.

### 2.2 ~~`OnCollision` sub-emitter event is defined but never fires~~ ✅ FIXED

> **Fixed**: `ModuleCollision::Apply()` and `ApplyWithRaycasts()` now accept an optional
> `std::vector<CollisionEvent>*` output parameter. When collisions occur, events are
> recorded and converted to `SubEmitterTriggerInfo` entries with
> `Event = SubEmitterEvent::OnCollision` in `ParticleSystem::UpdateInternal()`.

### 2.3 ~~LOD uses stepped thresholds instead of smooth interpolation~~ ✅ FIXED

> **Fixed**: `UpdateLOD()` now uses smooth linear falloff:
> `multiplier = (LODMaxDistance - dist) / (LODMaxDistance - LODDistance1)`,
> clamped to [0, 1]. `LODDistance2` is no longer used (could be removed from the struct).

### 2.4 ~~Emission rate temporarily mutated during Update~~ ✅ FIXED

> **Fixed**: `ParticleEmitter::Update()` now accepts a `rateMultiplier` parameter.
> The LOD multiplier is passed through without mutating the public `RateOverTime` member.

### 2.5 ~~Warm-up recursion depth~~ ✅ FIXED

> **Fixed**: `Update()` now delegates to a private `UpdateInternal()` method. The warm-up
> loop calls `UpdateInternal()` iteratively — no recursion, no stack growth risk.

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

### ~~3.3 Trail rendering is O(particles × trail_points) separate draw calls~~ ✅ FIXED

> **Fixed**: Trail segments now use `Renderer2D::DrawQuadVertices()` instead of
> `DrawPolygon()`. This eliminates per-segment `std::vector` heap allocations, fixes a
> `GL_TRIANGLE_FAN` correctness bug (multiple polygons in one batch created wrong
> triangles), adds per-vertex color interpolation for smooth gradients (was averaged),
> and uses the properly indexed quad batch path.

### 3.4 ~~Only one force field per system~~ ✅ FIXED

> **Fixed**: `ForceFieldModule` replaced with `std::vector<ModuleForceField> ForceFields`.
> `ParticleSystem::UpdateInternal()` iterates over all force fields. Editor UI supports
> add/remove with per-field tree nodes. Serialization writes a YAML sequence with backward
> compatibility for old single-field format.

### 3.5 ~~Emission direction ignores entity rotation~~ ✅ FIXED

> **Fixed**: `ParticleSystem::Update()` accepts an `emitterRotation` quaternion parameter,
> threaded through to `ParticleEmitter::InitializeParticle()`. Both the emission shape
> position offset and direction are transformed by the entity's rotation. `Scene.cpp`
> passes `glm::quat(transform.Rotation)` at both runtime and editor call sites.

### 3.6 ~~ParticleCurve find-segment is O(n) linear scan~~ ✅ FIXED

> **Fixed**: `ParticleCurve::Evaluate()` now uses binary search (O(log n)) to find the
> segment containing `t`, instead of an O(n) linear scan.

### 3.7 ~~`std::sort` for depth sorting is not stable and allocates~~ ✅ FIXED

> **Fixed**: `SortByDepth()` now precomputes squared distances into a cached
> `m_SortDistances` vector and uses insertion sort (O(n) for nearly-sorted data) instead
> of `std::sort`. No allocations in the hot path for frame-to-frame coherent data.

---

## 4 — Missing Features (Priority Order)

### ~~4.1 HIGH — ParticleRenderPass (proper 3D integration)~~ ✅ FIXED (Phase A)

> **Fixed (Phase A)**: `ParticleRenderPass` created and integrated into the render graph.
> Particles now render inside the render graph with depth testing against scene geometry.
> Phases B (particle shader) and C (instanced rendering) remain as optimizations.

Remaining phases:
- **Phase B**: Dedicated particle billboard shader (vertex shader billboarding, fragment
  shader depth-fade for soft particles).
- **Phase C**: Instance buffer for per-particle data. One draw call per texture batch.

### 4.2 HIGH — Instanced particle rendering
Replace per-particle `Renderer2D::DrawQuad()` with a single instanced draw call per
texture batch. Upload per-particle data (pos, size, color, rotation, UV) to an instance
buffer or SSBO. Part of Phase C of the ParticleRenderPass work (§4.1). Requires:
- A particle-specific vertex + fragment shader (billboard construction in vertex shader).
- Dynamic instance buffer management (map/orphan pattern or persistent mapped buffer).

### ~~4.3 HIGH — Curve editor UI~~ ✅ FIXED

> **Fixed**: Interactive ImGui curve editor widget implemented in `SceneHierarchyPanel.cpp`.
> `DrawParticleCurveEditor()` renders a canvas with polyline curve visualization, draggable
> key points, double-click to add keys (up to 8), right-click to remove (min 2).
> `DrawParticleCurve4Editor()` shows a gradient preview bar and per-channel (R/G/B/A)
> collapsible curve editors. Wired into Color Over Lifetime, Size Over Lifetime, and
> a new Velocity Over Lifetime section with LinearVelocity + SpeedMultiplier + SpeedCurve.

### 4.4 MEDIUM — Soft particles
Alpha-fade particles near opaque surfaces using depth buffer comparison. Requires depth
texture access in the particle fragment shader. Part of Phase B of the ParticleRenderPass
work (§4.1). See §1.6.

### 4.5 MEDIUM — Mesh particles
`ParticleRenderMode::Mesh` is declared but not implemented. Each particle should instance
a user-specified mesh (e.g., debris chunks, leaves). Requires instanced rendering (§4.2).

### ~~4.6 MEDIUM — Task system parallelization~~ ✅ FIXED

> **Fixed**: `ParticleSystem::UpdateInternal()` now launches Color, Size, and Rotation
> modules as concurrent tasks (via `Tasks::Launch()`) while the velocity chain
> (Gravity→Drag→Noise→VelocityOverLifetime) runs on the calling thread. These three
> modules are safe to run concurrently because they write to independent arrays
> (Colors, Sizes, Rotations respectively). The parallel path activates when
> `AliveCount >= 256` and at least one independent module is enabled; below that
> threshold, sequential fallback avoids task launch overhead.

### 4.7 ~~MEDIUM — Multiple force fields~~ ✅ FIXED

> See §3.4.

### 4.8 ~~MEDIUM — Emission direction from entity rotation~~ ✅ FIXED

> See §3.5.

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

### 4.12 ~~LOW — OnCollision sub-emitter trigger~~ ✅ FIXED

> See §2.2.

### ~~4.13 LOW — Trail texture coordinate mapping~~ ✅ FIXED

> **Fixed**: `TrailRenderer::RenderTrails()` now generates UV coordinates along the trail
> length (U = 0→1 from head to tail, V = 0→1 across width). When a texture is provided,
> `DrawQuadVertices()` with texture + per-vertex UVs is used; untextured trails fall back
> to the color-only path. The particle texture is shared with trails (via `psc.Texture`).

### ~~4.14 LOW — Inter-system sorting~~ ✅ FIXED

> **Fixed**: Particle systems sorted back-to-front by emitter distance to camera before
> rendering. See §1.7.

---

## 5 — Comparison to Unity / Unreal

| Feature | Unity Shuriken | Unreal Niagara | OloEngine | Gap |
|---|---|---|---|---|
| CPU simulation | ✅ | ✅ | ✅ | — |
| GPU simulation (compute) | ✅ (VFX Graph) | ✅ | ❌ | Blocked on SSBO/compute |
| Depth buffer occlusion | ✅ | ✅ | ✅ | Fixed — ParticleRenderPass with depth test |
| Soft particles (depth fade) | ✅ | ✅ | ❌ | Need depth texture in particle shader |
| Instanced rendering | ✅ | ✅ | ❌ | Planned (Phase 3 #9) |
| Curve editor | ✅ (rich spline) | ✅ (graph editor) | ✅ | Interactive curve editor with key add/remove/drag |
| Multiple force fields | ✅ (external) | ✅ (modules) | ✅ | Fixed — `std::vector<ModuleForceField>` |
| Mesh particles | ✅ | ✅ | ❌ (placeholder) | Needs instancing first |
| Sub-emitters | ✅ | ✅ | ✅ | Working — separate child systems |
| Trails | ✅ | ✅ | ✅ | Working — quad-strip via batched DrawQuadVertices with per-vertex color |
| Emission shapes | ✅ (Mesh surface) | ✅ | Partial (6 shapes) | Missing: mesh surface emit |
| Events/callbacks | OnCollision, OnTrigger | Full scripting | ✅ (OnBirth/OnDeath/OnCollision) | Fixed — OnCollision wired |
| Particle lights | ✅ | ✅ | ❌ | Low priority |
| Texture atlas / sprite sheet | ✅ | ✅ | ✅ | Working |
| LOD | ✅ | ✅ | ✅ | Fixed — smooth linear falloff |
| Warm-up | ✅ | ✅ | ✅ | Fixed — iterative, no stack risk |
| Velocity inheritance | ✅ | ✅ | ✅ | Working |
| Serialization | ✅ | ✅ | ✅ | YAML |
| .particle asset files | ✅ (.prefab) | ✅ (.uasset) | ✅ | .oloparticle |

**Overall assessment**: The simulation layer is solid — all known bugs are fixed, modules
compose correctly with forces, LOD is smooth, warm-up is safe, emission respects entity
rotation, and multiple force fields are supported. **Depth occlusion is now fixed** —
particles render inside the render graph via `ParticleRenderPass` with depth testing
against scene geometry. The remaining gaps are **performance** (instanced rendering,
particle shader) and **editor quality** (soft particles, trail optimization).

---

## 6 — Recommended Fix Order (updated)

### ~~Bug fixes & design improvements~~ ✅ ALL DONE

- ~~**Fix blend state batching** (§1.5)~~ ✅
- ~~**Fix VelocityOverLifetime vs forces** (§2.1)~~ ✅
- ~~**Convert warm-up to iterative** (§2.5)~~ ✅
- ~~**Smooth LOD interpolation** (§2.3)~~ ✅
- ~~**Pass emission rate multiplier as parameter** (§2.4)~~ ✅
- ~~**Wire OnCollision sub-emitter event** (§2.2)~~ ✅
- ~~**Apply entity rotation to emission direction** (§3.5)~~ ✅
- ~~**Multiple force fields** (§3.4)~~ ✅
- ~~**Insertion sort for depth sorting** (§3.7)~~ ✅
- ~~**Binary search for ParticleCurve** (§3.6)~~ ✅

### ~~Remaining: Rendering architecture overhaul~~ ✅ DONE (Phase A)

~~1. **Create `ParticleRenderPass`** (§1.8, Phase A) — new render pass inserted into the
   render graph between ScenePass and FinalPass. Renders into the ScenePass framebuffer
   with depth test enabled (read-only). This fixes:
   - §1.1 (particles outside render graph)
   - §1.2 (FinalPass blits before particles)
   - §1.4 (no depth occlusion)
   Must port existing `ParticleRenderer` logic into the new pass, still using per-particle
   quads via Renderer2D initially. Can optimize to instanced rendering later.~~ ✅

### Remaining: Editor + quality

2. ~~**Add curve editor UI** (§4.3) — unblocks artist workflow.~~ ✅
3. ~~**Optimize trail rendering** (§3.3) — batch into single draw call per particle.~~ ✅
4. **Particle billboard shader** (§1.8, Phase B) — vertex-shader billboarding, soft
   particle depth fade in fragment shader.
5. **Instanced rendering** (§1.8, Phase C) — instance buffer, one draw call per texture.

### Remaining: Future features

6. **Mesh particles** (§4.5) — requires instanced rendering.
7. ~~**Task system parallelization** (§4.6) — parallel module application.~~ ✅
8. **GPU compute simulation** (§4.9) — requires SSBO/compute shader support.
9. **Particle lights** (§4.10) — per-particle point lights.
10. **Custom vertex streams** (§4.11) — advanced shader effects.
11. ~~**Trail texture UVs** (§4.13) — UV mapping along trail length.~~ ✅
12. ~~**Inter-system sorting** (§4.14) — sort systems by camera distance.~~ ✅
