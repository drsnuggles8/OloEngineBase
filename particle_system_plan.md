# Plan: Particle System for OloEngine

## TL;DR
Full-featured particle system for OloEngine. Phase 1 delivers CPU-simulated particles with modular emitters, lifetime modifiers, billboard/2D rendering, editor UI, serialization, and script bindings. Phase 2 adds trails/ribbons, sub-emitters, collision, LOD, warm-up, and particle system assets. Phase 3 (from design review) addresses correctness bugs, visual quality, and performance. Phase 4 (from second design review, Feb 2026) focused on **proper 3D rendering integration** — a `ParticleRenderPass` in the render graph with depth occlusion, plus all bug fixes and design improvements. Phase 4 also added **instanced billboard rendering** (dedicated `Particle_Billboard.glsl` with GPU billboarding), **soft particles** (depth-fade near surfaces), and **mesh particles** (SSBO-instanced via `Particle_Mesh.glsl`). Remaining work: GPU compute simulation, particle lights, custom vertex streams. SOA data layout with swap-to-back death enables efficient particle counts.

Full issue list: `docs/PARTICLE_SYSTEM_REVIEW.md`

---

## Current Status

### Phase 1 — COMPLETE
All core features implemented and working:
- SOA ParticlePool with swap-to-back death
- Emission shapes (Point, Sphere, Box, Cone, Ring, Edge)
- ParticleEmitter with rate-based + burst emission
- 7 lifetime modifier modules (Color, Size, Velocity, Rotation, Gravity, Drag, Noise)
- ParticleSystem orchestrator with duration, looping, playback speed
- 2D rendering (Renderer2D), 3D billboard + mesh rendering (dedicated shaders via ParticleBatchRenderer)
- ECS integration (ParticleSystemComponent)
- Full YAML serialization
- Editor UI with collapsible module panels
- C# and Lua script bindings
- Tests (304 passing)

### Phase 2 — COMPLETE
Advanced features implemented:
- Trail/ribbon system (ring buffer per particle, line-segment rendering via Renderer3D)
- Sub-emitters (OnBirth/OnDeath triggers, shared pool)
- Collision module (WorldPlane + Jolt scene raycasts)
- Force fields (Attraction, Repulsion, Vortex)
- LOD (distance-based spawn rate reduction)
- Warm-up (pre-simulation on first play)
- Particle system asset (.oloparticle files via AssetManager)

### Phase 3 — Design Review Fixes (COMPLETE)
Bugs and improvements identified in first review:

| # | Item | Status |
|---|------|--------|
| 1 | **Fix ColorOverLifetime/SizeOverLifetime** — multiply by initial values instead of overwriting | ✅ Done |
| 2 | **Fix VelocityOverLifetime** — frame-rate independent (initial velocity × curve, not per-frame compound) | ✅ Done |
| 3 | **Implement Local simulation space** — particles simulated at origin, offset at render time | ✅ Done |
| 4 | **Replace sin-based noise with 3D Simplex noise** — spatially coherent, position-based evaluation | ✅ Done |
| 5 | **Ring buffer for trail data** — O(1) insert/age instead of O(n) vector insert/erase | ✅ Done |
| 6 | **Pass entity ID in 3D render path** — signatures updated, billboard path already passes ID | ✅ Done |
| 7 | **Increase curve key count** — 4 → 8 keys per ParticleCurve | ✅ Done |
| 8 | **Add InitialColors/InitialSizes/InitialVelocities** to ParticlePool SOA arrays | ✅ Done |
| 9 | Instanced particle rendering (Renderer3D batch draws) | ✅ Done (Phase 4) |
| 10 | **Depth sorting** (back-to-front for alpha blending) | ✅ Done |
| 11 | **Blend mode support** (Alpha, Additive, Premultiplied) | ✅ Done |
| 12 | **Sprite sheet / texture atlas animation** | ✅ Done |
| 13 | **Trail rendering as triangle strips** (camera-facing quad strips) | ✅ Done |
| 14 | **Sub-emitters as separate ParticleSystem instances** | ✅ Done |
| 15 | Mesh particle rendering | ✅ Done (Phase 4) |
| 16 | **Velocity inheritance from parent** | ✅ Done |
| 17 | Task system parallelization for module application | ✅ Done (Phase 4) |

### Phase 4 — Rendering Integration & Bug Fixes (COMPLETE)
Issues identified in second design review (`docs/PARTICLE_SYSTEM_REVIEW.md`):

| # | Item | Priority | Status |
|---|------|----------|--------|
| 18 | **Create ParticleRenderPass** — new render pass in the render graph between ScenePass and FinalPass. Renders particles with depth test (read-only) into ScenePass FB. Fixes: particles outside render graph (§1.1), FinalPass-before-particles ordering bug (§1.2), depth occlusion (§1.4). Phase A + Phase B complete. | 🔴 Critical | ✅ Done |
| 19 | **Fix blend state during Renderer2D batch** — flush batch before calling `SetParticleBlendMode()` to ensure GL state actually applies | 🔴 Critical | ✅ Done |
| 20 | **Fix VelocityOverLifetime overwriting forces** — preserves force contributions (gravity, drag, noise); module reordered after forces | 🔴 High | ✅ Done |
| 21 | **Convert warm-up from recursion to iteration** — `Update()` → `UpdateInternal()` split; iterative loop | 🟡 High | ✅ Done |
| 22 | **Smooth LOD interpolation** — linear falloff replacing stepped thresholds | 🟡 Medium | ✅ Done |
| 23 | **Don't mutate RateOverTime for LOD** — `rateMultiplier` parameter | 🟡 Medium | ✅ Done |
| 24 | **Wire OnCollision sub-emitter event** — `CollisionEvent` struct, events passed through collision Apply methods | 🟡 Medium | ✅ Done |
| 25 | **Apply entity rotation to emission direction** — `emitterRotation` quaternion threaded through Update → Emitter | 🟡 Medium | ✅ Done |
| 26 | **Curve editor UI** — Interactive ImGui curve editor with key add/remove/drag, gradient preview for color curves, wired into Color/Size/Velocity Over Lifetime sections | 🟡 Medium | ✅ Done |
| 27 | **Optimize trail rendering** — trail segments now use `DrawQuadVertices()` with per-vertex color; eliminates per-segment vector allocation and fixes `GL_TRIANGLE_FAN` batch bug | 🟡 Medium | ✅ Done |
| 28 | **Multiple force fields** — `std::vector<ModuleForceField> ForceFields` with editor add/remove UI | 🟢 Low | ✅ Done |
| 29 | **Soft particles (depth fade)** — alpha-fade near opaque surfaces using scene depth texture | 🟢 Low | ✅ Done |
| 30 | **Inter-system depth sorting** — particle systems sorted back-to-front by emitter distance to camera before rendering | 🟢 Low | ✅ Done |
| 31 | **Trail UV coordinates** — UV mapping along trail length (U=0→1 head to tail, V=0→1 across width); textured trails use `DrawQuadVertices` with texture | 🟢 Low | ✅ Done |
| 32 | **ParticleCurve Evaluate() optimization** — binary search replacing O(n) linear scan | 🟢 Low | ✅ Done |
| 33 | **Adaptive sort for depth sorting** — insertion sort with precomputed distances replacing `std::sort` | 🟢 Low | ✅ Done |
| 9 | Instanced particle rendering (from Phase 3) | 🟡 Medium | ✅ Done |
| 15 | Mesh particle rendering (from Phase 3) | 🟡 Medium | ✅ Done |
| 17 | Task system parallelization — Color, Size, Rotation modules launched as concurrent tasks alongside velocity chain; threshold >= 256 particles | 🟡 Medium | ✅ Done |

---

## Architecture Overview

```
ParticleSystemComponent (ECS)
  └── ParticleSystem (owns emitters, orchestrates update/render)
        ├── ParticleEmitter (emission shape, rate, bursts, initial properties)
        │     └── EmissionShape: Point, Sphere, Box, Cone, Ring, Edge
        ├── ParticlePool (SOA storage: position[], velocity[], color[], size[], rotation[],
        │                  lifetime[], maxLifetime[], initialColor[], initialSize[], initialVelocity[])
        ├── ParticleModules (pluggable modifiers applied each frame)
        │     ├── VelocityOverLifetime (applied first — sets base velocity from initial × curve)
        │     ├── GravityModule (adds gravity force)
        │     ├── DragModule (reduces velocity)
        │     ├── NoiseModule (3D Simplex noise at particle position)
        │     ├── RotationOverLifetime (angular velocity)
        │     ├── ColorOverLifetime (initialColor × curve)
        │     ├── SizeOverLifetime (initialSize × curve)
        │     ├── ForceFieldModule (attraction/repulsion/vortex)
        │     ├── CollisionModule (plane + Jolt raycasts)
        │     ├── SubEmitterModule (OnBirth/OnDeath triggers → child ParticleSystem instances)
        │     └── TextureSheetAnimation (grid UV animation over lifetime or by speed)
        ├── ParticleTrailData (per-particle ring buffer trail history)
        ├── Rendering Settings
        │     ├── BlendMode (Alpha, Additive, PremultipliedAlpha)
        │     ├── RenderMode (Billboard, StretchedBillboard, Mesh)
        │     ├── DepthSortEnabled (back-to-front sorting for alpha)
        │     └── VelocityInheritance (fraction of parent velocity added to emissions)
        ├── ParticleBatchRenderer (instanced billboard + mesh rendering)
        │     ├── Instance VBO for billboard particles (GPU billboarding)
        │     ├── UBO-based per-particle mesh draws
        │     ├── ParticleParams UBO (binding 2, camera vectors, soft particle settings)
        │     └── Dedicated shaders: Particle_Billboard.glsl, Particle_Mesh.glsl
        └── ParticleRenderer
              ├── RenderParticles2D (flat quads via Renderer2D)
              ├── RenderParticlesBillboard (instanced GPU billboards via ParticleBatchRenderer)
              ├── RenderParticlesStretched (velocity-aligned quads via Renderer2D)
              └── RenderParticlesMesh (per-particle mesh draws via ParticleBatchRenderer)
```

---

## Key Design Decisions

### Module Application Order
Modules are applied in a specific order in `ParticleSystem::UpdateInternal()`:
1. **Gravity** — Adds `gravity × dt` to velocity.
2. **Drag** — Reduces velocity by `(1 - drag × dt)`.
3. **Noise** — Simplex noise displacement based on particle position.
4. **VelocityOverLifetime** — Preserves accumulated force contributions (`forceContribution = Velocities - InitialVelocities`), then applies speed curve to initial component: `InitialVelocity × SpeedCurve + forceContribution + LinearVelocity × dt`. Runs *after* forces so forces aren't overwritten.
5. **Rotation** — Additive angular velocity.
6. **Color** — `InitialColor × ColorCurve.Evaluate(age)`.
7. **Size** — `InitialSize × SizeCurve.Evaluate(age)`.
8. **ForceFields** — Attraction/repulsion/vortex forces (iterated over `std::vector<ModuleForceField>`).
9. **Collision** — Plane or raycast collision response (records `CollisionEvent`s for sub-emitter triggers).

### Simulation Space
- **World** (default): Particles are emitted at the entity's world position. Moving the entity only affects new emissions.
- **Local**: Particles are emitted at local origin (0,0,0). At render time, the emitter's world position is added as an offset. This makes all particles move with the entity.

### Initial Values Pattern
`ParticlePool` stores `InitialColors`, `InitialSizes`, and `InitialVelocities` alongside the current values. "OverLifetime" modules multiply the initial value by a curve evaluated at the particle's normalized age (0→1). This preserves per-particle variance from emission.

### Trail Ring Buffer
Each particle's trail history is stored in a fixed-size ring buffer (`TrailRingBuffer`) instead of a `std::vector`. Insert and age operations are O(1). The ring buffer wraps around when full, naturally discarding the oldest points.

### 3D Particle Rendering
Particles are rendered via `ParticleRenderPass` in the render graph (ScenePass →
ParticlePass → FinalPass). The pass renders into the ScenePass framebuffer with depth
testing enabled (read-only, `GL_LEQUAL`) so particles are correctly occluded by opaque
geometry. Per-draw-buffer blend control (`glEnablei`/`glDisablei`) avoids blending on the
RED_INTEGER entity ID attachment. Three render modes are supported:
- **Billboard** — GPU-billboarded quads via `ParticleBatchRenderer` using instanced
  rendering (instance VBO + `Particle_Billboard.glsl`). Vertex shader constructs
  camera-facing quads from position/size/rotation per-instance data.
- **StretchedBillboard** — Velocity-aligned quads via `Renderer2D::DrawQuadVertices()`
  (still uses Renderer2D path).
- **Mesh** — Per-particle mesh rendering via `ParticleBatchRenderer::RenderMeshParticles()`
  using a single-instance UBO (binding 3) with one draw call per particle
  (`Particle_Mesh.glsl`). `ParticleSystemComponent` holds `Ref<Mesh> ParticleMesh`.

**Soft particles**: Both billboard and mesh shaders sample the scene depth texture and
fade alpha near opaque surfaces (`SoftParticlesEnabled` / `SoftParticleDistance` per system).

**UBO binding management**: `UniformBuffer::Bind()` re-binds particle UBOs after ScenePass
`CommandDispatch` overwrites shared binding points (0/2/3).

Depth sorting is enabled by default for alpha-blended particles. `SortByDepth(cameraPosition)` sorts particle indices back-to-front by squared distance. Additive particles skip sorting since they're order-independent.

Blend modes (Alpha, Additive, PremultipliedAlpha) are set via `RenderCommand::SetBlendFunc()` before rendering each particle system, then restored to the default state after.

### Sub-Emitter Architecture
Sub-emitter entries reference child `ParticleSystem` instances stored on `ParticleSystemComponent::ChildSystems`. When triggers fire (OnBirth/OnDeath), `SubEmitterTriggerInfo` carries the `ChildSystemIndex`. Scene's `ProcessChildSubEmitters()` emits particles into the appropriate child system pool. Child systems have independent pools, settings, and textures. Legacy mode (`ChildSystemIndex == -1`) falls back to emitting into the parent pool.

### Texture Sheet Animation
`ModuleTextureSheetAnimation` divides a texture atlas into a grid (`GridX × GridY`). Each particle selects a frame based on either:
- **OverLifetime**: `frame = age * TotalFrames` (normalized 0→1)
- **BySpeed**: `frame = speed / SpeedRange * TotalFrames`

UV sub-rects are computed per particle and passed to the renderer via `uvMin`/`uvMax` overloads.

---

## Remaining Work (Priority Order)

See `docs/PARTICLE_SYSTEM_REVIEW.md` for full details on each item.

All bug fixes, design issues, rendering architecture, and editor quality items are
**resolved**. The remaining items are long-term future features.

### Incremental improvements
1. **Stretched billboard shader** — migrate `RenderParticlesStretched` from Renderer2D
   to a dedicated shader (analogous to `Particle_Billboard.glsl`). Would add soft
   particle support to stretched mode.
2. **Trail shader** — migrate `TrailRenderer::RenderTrails` from Renderer2D to a
   dedicated shader. Would add soft particle support to trails.
3. **Mesh surface emission** — emit particles from mesh surface (currently 6 shapes:
   Point, Sphere, Box, Cone, Ring, Edge).

### Future features
4. **GPU compute simulation** — requires SSBO + compute shader support (not yet in engine).
   Would unlock 100K–1M+ particle counts.
5. **Particle lights** — per-particle point lights for fire/explosions.
6. **Custom vertex streams** — advanced shader effects.

### Completed (Phase 1–4)
- ✅ Instanced billboard rendering — `Particle_Billboard.glsl` with GPU billboarding, instance VBO
- ✅ Mesh particle rendering — `Particle_Mesh.glsl` with per-draw-call UBO
- ✅ Soft particles — depth-fade near opaque surfaces via scene depth texture
- ✅ ParticleRenderPass (Phase A + B) — render graph integration with depth occlusion, dedicated shaders
- ✅ Per-draw-buffer blend control — `glEnablei`/`glDisablei` for integer entity ID attachment
- ✅ UBO binding management — `UniformBuffer::Bind()` re-binds after ScenePass overwrites
- ✅ OpenGLFramebuffer::ReadPixel fix — bind FBO before `glReadPixels`
- ✅ Depth sorting — insertion sort with precomputed distances
- ✅ Blend mode support — `ParticleBlendMode` enum; GL blend state set per system with batch flush
- ✅ Sprite sheet animation — `ModuleTextureSheetAnimation` with grid UVs
- ✅ Trail rendering as triangle strips — `TrailRenderer::RenderTrails()` via `DrawQuadVertices()` with per-vertex color
- ✅ Sub-emitters as separate systems — `ChildSystems` vector; OnBirth/OnDeath/OnCollision triggers
- ✅ Velocity inheritance — parent velocity computed from position delta
- ✅ Stretched billboard rendering — velocity-aligned quads
- ✅ VelocityOverLifetime preserves forces — module reordered after forces
- ✅ Iterative warm-up — `Update()` → `UpdateInternal()` split
- ✅ Smooth LOD — linear falloff replacing stepped thresholds
- ✅ LOD rate multiplier as parameter — no mutation of public state
- ✅ OnCollision sub-emitter event — `CollisionEvent` struct wired through
- ✅ Entity rotation on emission — `emitterRotation` quaternion
- ✅ Multiple force fields — `std::vector<ModuleForceField>` with editor UI
- ✅ Binary search for ParticleCurve — O(log n) segment lookup
- ✅ Insertion sort for depth sorting — O(n) for nearly-sorted data
- ✅ Curve editor UI — interactive ImGui curve editor with key add/remove/drag
- ✅ Trail rendering optimization — `DrawQuadVertices()` replaces `DrawPolygon()`
- ✅ Trail UV coordinates — UV mapping along trail length, textured trail support
- ✅ Inter-system depth sorting — systems sorted back-to-front by camera distance
- ✅ Task system parallelization — Color/Size/Rotation modules run as concurrent tasks

---

## Files

### Particle system files (`OloEngine/src/OloEngine/Particle/`):
- `ParticlePool.h/.cpp` — SOA storage (positions, velocities, colors, sizes, rotations, lifetimes, initial values)
- `EmissionShape.h` — Emission shape variants (Point, Sphere, Box, Cone, Ring, Edge)
- `ParticleEmitter.h/.cpp` — Rate-based + burst emission, initial value population
- `ParticleCurve.h` — Piecewise-linear curve with up to 8 keys
- `ParticleModules.h/.cpp` — All lifetime modifier modules
- `ParticleSystem.h/.cpp` — Orchestrator (update order, warm-up, LOD, sub-emitter triggers)
- `ParticleBatchRenderer.h/.cpp` — Instanced billboard + mesh particle renderer (instance VBO, UBOs, dedicated shaders)
- `ParticleRenderer.h/.cpp` — 2D, billboard, stretched, and mesh render dispatch
- `ParticleTrail.h/.cpp` — Ring buffer trail data per particle
- `ParticleCollision.h/.cpp` — WorldPlane and Jolt raycast collision
- `SubEmitter.h` — Sub-emitter event types and trigger info
- `TrailRenderer.h/.cpp` — Trail quad-strip rendering via Renderer2D (camera-facing ribbons)
- `SimplexNoise.h/.cpp` — 3D Simplex noise for turbulence module

### Shaders (`OloEditor/assets/shaders/`):
- `Particle_Billboard.glsl` — GPU billboarding vertex shader + soft particle fragment shader
- `Particle_Mesh.glsl` — Per-draw-call mesh particle shader with single-instance UBO

### Integration points:
- `OloEngine/src/OloEngine/Scene/Components.h` — `ParticleSystemComponent` (Texture, ParticleMesh)
- `OloEngine/src/OloEngine/Scene/Scene.cpp` — Update + render particle systems (soft params, mesh dispatch, ParticleRenderPass callback)
- `OloEngine/src/OloEngine/Scene/SceneSerializer.cpp` — YAML serialization (incl. SoftParticlesEnabled/Distance)
- `OloEngine/src/OloEngine/Renderer/Passes/ParticleRenderPass.h/.cpp` — Render pass in the render graph
- `OloEngine/src/OloEngine/Renderer/Renderer3D.h/.cpp` — Render graph setup + ParticlePass registration
- `OloEngine/src/OloEngine/Renderer/UniformBuffer.h` — `Bind()` method for UBO re-binding
- `OloEngine/src/Platform/OpenGL/OpenGLUniformBuffer.h/.cpp` — `Bind()` implementation + `m_Binding` member
- `OloEngine/src/Platform/OpenGL/OpenGLFramebuffer.cpp` — `ReadPixel()` with FBO bind
- `OloEngine/src/OloEngine/Renderer/Camera/EditorCamera.h` — `GetNearClip()`/`GetFarClip()`
- `OloEditor/src/Panels/SceneHierarchyPanel.cpp` — Editor UI (soft particles, mesh status)
