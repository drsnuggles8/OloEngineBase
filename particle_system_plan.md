# Plan: Particle System for OloEngine

## TL;DR
Full-featured particle system for OloEngine. Phase 1 delivers CPU-simulated particles with modular emitters, lifetime modifiers, billboard/2D rendering, editor UI, serialization, and script bindings. Phase 2 adds trails/ribbons, sub-emitters, collision, LOD, warm-up, and particle system assets. Phase 3 (from design review) addresses correctness bugs, visual quality, and performance. SOA data layout with swap-to-back death enables efficient particle counts.

---

## Current Status

### Phase 1 — COMPLETE
All core features implemented and working:
- SOA ParticlePool with swap-to-back death
- Emission shapes (Point, Sphere, Box, Cone, Ring, Edge)
- ParticleEmitter with rate-based + burst emission
- 7 lifetime modifier modules (Color, Size, Velocity, Rotation, Gravity, Drag, Noise)
- ParticleSystem orchestrator with duration, looping, playback speed
- 2D and 3D billboard rendering (Renderer2D-based for both)
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

### Phase 3 — Design Review Fixes (IN PROGRESS)
Bugs and improvements identified in `docs/PARTICLE_SYSTEM_REVIEW.md`:

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
| 9 | Instanced particle rendering (Renderer3D batch draws) | ❌ Pending (Large — requires Renderer3D instancing support) |
| 10 | **Depth sorting** (back-to-front for alpha blending) | ✅ Done |
| 11 | **Blend mode support** (Alpha, Additive, Premultiplied) | ✅ Done |
| 12 | **Sprite sheet / texture atlas animation** | ✅ Done |
| 13 | **Trail rendering as triangle strips** (camera-facing quad strips) | ✅ Done |
| 14 | **Sub-emitters as separate ParticleSystem instances** | ✅ Done |
| 15 | Mesh particle rendering | ❌ Pending |
| 16 | **Velocity inheritance from parent** | ✅ Done |
| 17 | Task system parallelization for module application | ❌ Pending |

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
        └── ParticleRenderer
              ├── RenderParticles2D (flat quads via Renderer2D)
              ├── RenderParticlesBillboard (camera-facing quads via Renderer2D, sorted + sprite sheet)
              └── RenderParticlesStretched (velocity-aligned quads via Renderer2D)
```

---

## Key Design Decisions

### Module Application Order
Modules are applied in a specific order in `ParticleSystem::Update()`:
1. **VelocityOverLifetime** — Sets base velocity from `InitialVelocity × SpeedCurve × SpeedMultiplier + LinearVelocity × elapsed`. Applied first so forces add on top.
2. **Gravity** — Adds `gravity × dt` to velocity.
3. **Drag** — Reduces velocity by `(1 - drag × dt)`.
4. **Noise** — Simplex noise displacement based on particle position.
5. **Rotation** — Additive angular velocity.
6. **Color** — `InitialColor × ColorCurve.Evaluate(age)`.
7. **Size** — `InitialSize × SizeCurve.Evaluate(age)`.
8. **ForceField** — Attraction/repulsion/vortex forces.
9. **Collision** — Plane or raycast collision response.

### Simulation Space
- **World** (default): Particles are emitted at the entity's world position. Moving the entity only affects new emissions.
- **Local**: Particles are emitted at local origin (0,0,0). At render time, the emitter's world position is added as an offset. This makes all particles move with the entity.

### Initial Values Pattern
`ParticlePool` stores `InitialColors`, `InitialSizes`, and `InitialVelocities` alongside the current values. "OverLifetime" modules multiply the initial value by a curve evaluated at the particle's normalized age (0→1). This preserves per-particle variance from emission.

### Trail Ring Buffer
Each particle's trail history is stored in a fixed-size ring buffer (`TrailRingBuffer`) instead of a `std::vector`. Insert and age operations are O(1). The ring buffer wraps around when full, naturally discarding the oldest points.

### 3D Particle Rendering
Particles are rendered using `Renderer2D`'s quad batching in all paths. Three render modes are supported:
- **Billboard** — Camera-facing quads (`RenderParticlesBillboard`)
- **StretchedBillboard** — Velocity-aligned quads (`RenderParticlesStretched`)
- **Mesh** — Per-particle mesh instancing (placeholder, requires Renderer3D instancing)

Depth sorting is enabled by default for alpha-blended particles. `SortByDepth(cameraPosition)` sorts particle indices back-to-front by squared distance. Additive particles skip sorting since they're order-independent.

Blend modes (Alpha, Additive, PremultipliedAlpha) are set via `RenderCommand::SetBlendFunc()` before rendering each particle system, then restored to the default state after.

### Sub-Emitter Architecture
Sub-emitter entries reference child `ParticleSystem` instances stored on `ParticleSystemComponent::ChildSystems`. When triggers fire (OnBirth/OnDeath), `SubEmitterTriggerInfo` carries the `ChildSystemIndex`. Scene's `ProcessChildSubEmitters()` emits particles into the appropriate child system pool. Child systems have independent pools, settings, and textures. Legacy mode (`ChildSystemIndex == -1`) falls back to emitting into the parent pool.

### Texture Sheet Animation
`ModuleTextureSheetAnimation` divides a texture atlas into a grid (`GridX × GridY`). Each particle selects a frame based on either:
- **OverLifetime**: `frame = age * TotalFrames` (normalized 0→1)
- **BySpeed**: `frame = speed / SpeedRange * TotalFrames`

UV sub-rects are computed per particle and passed to `Renderer2D::DrawQuad()` via `uvMin`/`uvMax` overloads.

---

## Remaining Work (Priority Order)

### High Priority
1. **Instanced particle rendering** — Add `Renderer3D::DrawParticleBatch()` with instance buffer. Single draw call per texture batch. This is a Renderer3D feature that benefits the entire engine.

### Medium Priority
2. **Mesh particle rendering** — `RenderMode::Mesh` where each particle instances a user-specified mesh. Requires instanced rendering (item 1).
3. **Task system parallelization** — Split particle ranges across workers for independent modules (Gravity, Drag, Noise).

### Completed
- ✅ Depth sorting (back-to-front for alpha blending) — `ParticleSystem::SortByDepth()` sorts index array; renderer iterates in sorted order
- ✅ Blend mode support — `ParticleBlendMode` enum (Alpha, Additive, PremultipliedAlpha); GL blend state set per system
- ✅ Sprite sheet animation — `ModuleTextureSheetAnimation` with grid UVs, OverLifetime/BySpeed modes
- ✅ Trail rendering as triangle strips — `TrailRenderer::RenderTrails()` generates camera-facing quad strips via `Renderer2D::DrawPolygon()`
- ✅ Sub-emitters as separate systems — `ChildSystems` vector on `ParticleSystemComponent`; Scene manages child pools independently
- ✅ Velocity inheritance — `VelocityInheritance` setting; parent velocity computed from position delta in Scene.cpp
- ✅ Stretched billboard rendering — `RenderParticlesStretched()` with velocity-aligned quads

---

## Files

### Particle system files (`OloEngine/src/OloEngine/Particle/`):
- `ParticlePool.h/.cpp` — SOA storage (positions, velocities, colors, sizes, rotations, lifetimes, initial values)
- `EmissionShape.h` — Emission shape variants (Point, Sphere, Box, Cone, Ring, Edge)
- `ParticleEmitter.h/.cpp` — Rate-based + burst emission, initial value population
- `ParticleCurve.h` — Piecewise-linear curve with up to 8 keys
- `ParticleModules.h/.cpp` — All lifetime modifier modules
- `ParticleSystem.h/.cpp` — Orchestrator (update order, warm-up, LOD, sub-emitter triggers)
- `ParticleRenderer.h/.cpp` — 2D, billboard, and 3D render paths
- `ParticleTrail.h/.cpp` — Ring buffer trail data per particle
- `ParticleCollision.h/.cpp` — WorldPlane and Jolt raycast collision
- `SubEmitter.h` — Sub-emitter event types and trigger info
- `TrailRenderer.h/.cpp` — Trail quad-strip rendering via Renderer2D (camera-facing ribbons)
- `SimplexNoise.h/.cpp` — 3D Simplex noise for turbulence module

### Integration points:
- `OloEngine/src/OloEngine/Scene/Components.h` — `ParticleSystemComponent`
- `OloEngine/src/OloEngine/Scene/Scene.cpp` — Update + render particle systems (Local space offset, billboard rendering)
- `OloEngine/src/OloEngine/Scene/SceneSerializer.cpp` — YAML serialization
- `OloEditor/src/Panels/SceneHierarchyPanel.cpp` — Editor UI
