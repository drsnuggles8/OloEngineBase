# Particle System — Design Review & Issues

> **Date**: February 2026
> **Last updated**: February 2026 (All incremental improvements complete: trail shader, mesh surface emission)
> **Scope**: Full review of the OloEngine particle system after Phase 1–3 implementation.
> Covers design problems, bugs, missing features, and comparison to production engines.

---

## Summary

The particle system has a solid foundation: SOA pool with swap-to-back death, modular
modifier pipeline, sub-emitters with independent child systems, ring-buffer trails, and
comprehensive editor UI. All simulation-side bugs have been fixed (velocity module, LOD,
warm-up, emission rotation, force fields, sub-emitter collision events, curve evaluation,
depth sorting). **Rendering architecture is fully complete**: `ParticleRenderPass`
(Phase A) integrates particles into the render graph with depth occlusion. **Phase B**
(instanced billboard shader with GPU billboarding) is complete. **Soft particles**
(depth-fade near opaque surfaces) are implemented for all render modes. **Mesh particles**
(instanced mesh rendering via UBO) are implemented. **Trail rendering** uses a dedicated
`Particle_Trail.glsl` shader via `ParticleBatchRenderer`. **Stretched billboards** use
the stretch branch in `Particle_Billboard.glsl`. **Mesh surface emission** supports
emitting particles from primitive mesh surfaces using weighted random triangle sampling.
All particle rendering uses dedicated shaders — no Renderer2D dependency remains.
The remaining gaps are GPU compute simulation and minor editor polish.

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

### ~~1.3 Particles still go through Renderer2D~~ ✅ FIXED

> **Fixed**: All particle rendering now uses dedicated shaders via
> `ParticleBatchRenderer`:
> - Billboard: `Particle_Billboard.glsl` (instanced GPU billboarding)
> - Stretched Billboard: `Particle_Billboard.glsl` (stretch branch via `SubmitStretched()`)
> - Trail: `Particle_Trail.glsl` (batched quad-strip via `SubmitTrailQuad()`)
> - Mesh: `Particle_Mesh.glsl` (UBO-based per-draw-call rendering)
> All paths support soft particles and depth buffer access. No particle rendering
> goes through Renderer2D.

### ~~1.4 Particles ignore the 3D depth buffer~~ ✅ FIXED

> **Fixed**: `ParticleRenderPass::Execute()` enables depth testing (`GL_LEQUAL`,
> read-only via `SetDepthMask(false)`) so particles are correctly occluded by opaque
> geometry. Depth writes remain disabled so particles don't occlude each other or later
> transparent objects.

### 1.5 ~~Blend state changes during an active Renderer2D batch~~ ✅ FIXED

> **Fixed**: `SetParticleBlendMode()` now calls `Renderer2D::Flush()` before changing
> GL blend state, ensuring queued quads render with the correct blend mode.

### ~~1.6 No soft particles (depth fade near surfaces)~~ ✅ FIXED

> **Fixed**: Both `Particle_Billboard.glsl` and `Particle_Mesh.glsl` fragment shaders
> sample the scene depth texture (`u_DepthTexture`), linearize both scene and fragment
> depths using near/far clip planes, and compute alpha fade:
> `fade = clamp((linearScene - linearFrag) / softDistance, 0, 1)`. Controlled per-system
> via `ParticleSystem::SoftParticlesEnabled` and `SoftParticleDistance`. The scene depth
> attachment is bound as a texture during the particle pass via
> `ParticleBatchRenderer::SetSoftParticleParams()`. Editor UI provides checkbox + distance
> slider; values are serialized to scene YAML.

### ~~1.7 No inter-system depth sorting~~ ✅ FIXED

> **Fixed**: Particle systems are now sorted back-to-front by distance to camera before
> rendering. Both editor and runtime render callbacks collect all `TransformComponent` +
> `ParticleSystemComponent` entities, compute squared distance to camera position, and
> render in farthest-first order. This ensures overlapping alpha-blended systems composite
> correctly.

### ~~1.8 The proper fix: ParticleRenderPass~~ ✅ FIXED (Phases A + B complete)

> **Fixed (Phase A)**: Created `ParticleRenderPass` class, inserted into the render graph
> between ScenePass and FinalPass. Graph order: ScenePass → ParticlePass → FinalPass.
> The pass renders into the ScenePass framebuffer with depth test enabled (read-only,
> `GL_LEQUAL`, no depth write) so particles correctly occlude against opaque geometry.
> Particle rendering moved from manual post-EndScene code into a render callback set
> before `Renderer3D::EndScene()`. Both editor and runtime paths updated.
>
> **Fixed (Phase B)**: Dedicated `Particle_Billboard.glsl` shader with GPU billboarding
> in the vertex shader. Instance VBO with per-vertex attribute divisor for efficient
> instanced billboard rendering via `ParticleBatchRenderer`. Also added
> `Particle_Mesh.glsl` with SSBO-based instanced mesh rendering for mesh particles.

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

### ~~3.1 All rendering goes through Renderer2D~~ ✅ FIXED

> **Fixed**: All particle rendering now uses dedicated shaders via `ParticleBatchRenderer`.
> Billboard and stretched billboard particles use `Particle_Billboard.glsl` (GPU
> billboarding with stretch branch). Trail particles use `Particle_Trail.glsl` (batched
> quad-strip rendering). Mesh particles use `Particle_Mesh.glsl` (UBO-based instanced
> rendering). All paths support soft particles and depth buffer access. No particle
> rendering goes through Renderer2D.

### ~~3.2 Per-particle CPU transform construction is expensive~~ ✅ FIXED

> **Fixed**: All billboard particle rendering (standard and stretched) uses
> `Particle_Billboard.glsl` with GPU billboarding in the vertex shader. Per-particle
> data (position, size, rotation, color, stretch factor) is uploaded via instance VBO —
> no CPU matrix construction or trig calls. Stretched billboards use
> `ParticleBatchRenderer::SubmitStretched()` with the stretch branch in the shader.

### ~~3.3 Trail rendering is O(particles × trail_points) separate draw calls~~ ✅ FIXED

> **Fixed**: Trail rendering now uses dedicated `Particle_Trail.glsl` shader via
> `ParticleBatchRenderer` with batched quad submission (`SubmitTrailQuad`). This
> eliminates per-segment heap allocations, adds soft particle support via scene depth
> texture, and provides per-vertex color interpolation for smooth gradients. Trail
> quads are batched and flushed in a single draw call per texture batch.

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
>
> **Fixed (Phase B)**: Dedicated `Particle_Billboard.glsl` shader with GPU billboarding
> in vertex shader and depth-fade soft particles in fragment shader. Instance VBO for
> instanced billboard rendering. `Particle_Mesh.glsl` for SSBO-based mesh instancing.
> All phases complete.

### ~~4.2 HIGH — Instanced particle rendering~~ ✅ FIXED

> **Fixed**: `ParticleBatchRenderer` uploads per-particle data to an instance VBO
> (billboards) or SSBO (meshes) and issues a single instanced draw call per texture batch.
> Billboard particles use per-vertex attribute divisors; mesh particles use
> `gl_InstanceID` into an SSBO. Dedicated shaders: `Particle_Billboard.glsl` (GPU
> billboarding) and `Particle_Mesh.glsl` (SSBO-instanced meshes).

### ~~4.3 HIGH — Curve editor UI~~ ✅ FIXED

> **Fixed**: Interactive ImGui curve editor widget implemented in `SceneHierarchyPanel.cpp`.
> `DrawParticleCurveEditor()` renders a canvas with polyline curve visualization, draggable
> key points, double-click to add keys (up to 8), right-click to remove (min 2).
> `DrawParticleCurve4Editor()` shows a gradient preview bar and per-channel (R/G/B/A)
> collapsible curve editors. Wired into Color Over Lifetime, Size Over Lifetime, and
> a new Velocity Over Lifetime section with LinearVelocity + SpeedMultiplier + SpeedCurve.

### ~~4.4 MEDIUM — Soft particles~~ ✅ FIXED

> **Fixed**: See §1.6. Both billboard and mesh particle shaders sample the scene depth
> texture and fade alpha based on the linear depth difference. Controlled per-system via
> `SoftParticlesEnabled` / `SoftParticleDistance`.

### ~~4.5 MEDIUM — Mesh particles~~ ✅ FIXED

> **Fixed**: `ParticleRenderMode::Mesh` fully implemented. `ParticleBatchRenderer::
> RenderMeshParticles()` uploads per-instance data (model matrix, color, entity ID) to
> an SSBO and draws the user-specified mesh with instanced rendering. Dedicated
> `Particle_Mesh.glsl` shader reads SSBO via `gl_InstanceID`. `ParticleSystemComponent`
> holds `Ref<Mesh> ParticleMesh`; editor UI shows mesh assignment status.

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
| Soft particles (depth fade) | ✅ | ✅ | ✅ | Fixed — depth texture comparison in particle fragment shaders |
| Instanced rendering | ✅ | ✅ | ✅ | Fixed — instance VBO (billboards) + SSBO (meshes) |
| Curve editor | ✅ (rich spline) | ✅ (graph editor) | ✅ | Interactive curve editor with key add/remove/drag |
| Multiple force fields | ✅ (external) | ✅ (modules) | ✅ | Fixed — `std::vector<ModuleForceField>` |
| Mesh particles | ✅ | ✅ | ✅ | Fixed — SSBO-based instanced mesh rendering |
| Sub-emitters | ✅ | ✅ | ✅ | Working — separate child systems |
| Trails | ✅ | ✅ | ✅ | Fixed — dedicated Particle_Trail.glsl shader with soft particles |
| Emission shapes | ✅ (Mesh surface) | ✅ | ✅ (7 shapes incl. mesh) | Fixed — Point, Sphere, Box, Cone, Ring, Edge, Mesh |
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
rotation, and multiple force fields are supported. **Rendering is fully complete** —
all particle modes (billboard, stretched billboard, trail, mesh) render inside the render
graph via `ParticleRenderPass` with dedicated shaders, depth testing, and soft particles.
No particle rendering goes through Renderer2D. Emission supports 7 shapes including mesh
surface emission. The remaining gaps are **GPU compute simulation** and **minor polish**
(particle lights, custom vertex streams).

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

### ~~Remaining: Rendering architecture overhaul~~ ✅ DONE (Phases A + B)

~~1. **Create `ParticleRenderPass`** (§1.8, Phase A) — new render pass inserted into the
   render graph between ScenePass and FinalPass. Renders into the ScenePass framebuffer
   with depth test enabled (read-only). This fixes:
   - §1.1 (particles outside render graph)
   - §1.2 (FinalPass blits before particles)
   - §1.4 (no depth occlusion)
   Must port existing `ParticleRenderer` logic into the new pass, still using per-particle
   quads via Renderer2D initially. Can optimize to instanced rendering later.~~ ✅

### ~~Remaining: Editor + quality~~ ✅ ALL DONE

- ~~**Add curve editor UI** (§4.3)~~ ✅
- ~~**Optimize trail rendering** (§3.3)~~ ✅
- ~~**Particle billboard shader** (§1.8, Phase B)~~ ✅
- ~~**Instanced rendering** (§1.8, Phase B)~~ ✅
- ~~**Mesh particles** (§4.5)~~ ✅
- ~~**Task system parallelization** (§4.6)~~ ✅
- ~~**Trail texture UVs** (§4.13)~~ ✅
- ~~**Inter-system sorting** (§4.14)~~ ✅

### ~~Remaining: Incremental improvements~~ ✅ ALL DONE

- ~~**Stretched billboard shader** (§3.1/§3.2) — already uses `Particle_Billboard.glsl` with stretch branch via `ParticleBatchRenderer::SubmitStretched()`~~ ✅
- ~~**Trail shader** (§3.1) — dedicated `Particle_Trail.glsl` shader via `ParticleBatchRenderer::SubmitTrailQuad()` with soft particle support~~ ✅
- ~~**Mesh surface emission** (§5 comparison gap) — `EmitMesh` shape with weighted random triangle sampling from mesh surface; supports primitive meshes (Cube, Sphere, Cylinder, Torus, Icosphere, Cone)~~ ✅

### Remaining: Future features

1. **GPU compute simulation** (§4.9) — requires SSBO/compute shader support.
2. **Particle lights** (§4.10) — per-particle point lights.
3. **Custom vertex streams** (§4.11) — advanced shader effects.
