# Particle System Design Review

## Overview

The particle system lives under `OloEngine/src/OloEngine/Particle/` and follows a modular, data-oriented design with SOA (Structure of Arrays) storage. It integrates into the engine via an ECS component (`ParticleSystemComponent`) and renders through both `Renderer2D` and `Renderer3D`.

This document captures a design review of the current implementation, covering architectural strengths, bugs, design issues, overlooked concerns, missing features, and comparisons with Unity and Unreal Engine.

---

## What's Done Well

1. **SOA data layout** — `ParticlePool` stores parallel arrays (`Positions`, `Velocities`, `Colors`, `Sizes`, `Rotations`, `Lifetimes`, `MaxLifetimes`). Every module does a tight loop over contiguous memory. This is how production engines (Unity DOTS particles, Unreal Niagara data interfaces) structure particle data for cache-friendly iteration.

2. **Modular architecture** — Each module is a small struct with an `Enabled` flag and an `Apply()` method. Easy to add new behaviors without touching the orchestrator. This mirrors Unity's "module" approach and makes the system approachable.

3. **Swap-to-back kill with `OnSwapCallback`** — Clean solution to keep auxiliary SOA data (trail data) synchronized when particles die, without the pool needing to know about trails directly.

4. **Good editor integration** — Full ImGui inspector with collapsible headers per module, texture drag-drop, emission shape-specific parameter editing. YAML serialization is complete. The system previews live in editor mode via `OnUpdateEditor`.

5. **LOD and warm-up** — Distance-based spawn rate reduction (stepped at 50%, 25%, 0%) and pre-simulation on first play. These are features many indie engines skip.

6. **Physics integration** — Optional Jolt scene raycast collision gives accurate per-particle collision with the 3D scene. The fallback `WorldPlane` mode keeps the common case cheap.

7. **Full serialization** — `SceneSerializer` handles save/load of all particle system properties. Asset system supports `.oloparticle` files via `ParticleSystemAsset`.

---

## Fundamental Design Issues

### Issue 1: Per-particle draw calls in Renderer3D

**Severity: Critical — Performance**

In `ParticleRenderer::RenderParticles3D()`, each alive particle produces a separate `Renderer3D::DrawQuad()` or `Renderer3D::DrawCube()` call, each returning and submitting a `CommandPacket`. With 1,000 particles, that's 1,000 draw commands per frame.

This defeats the purpose of a particle system. Unity and Unreal use GPU instancing or compute-shader-driven indirect draws. Even the 2D path (`RenderParticles2D`) at least benefits from `Renderer2D`'s internal quad batching, but the 3D path has no such batching.

This is also a broader `Renderer3D` concern — the renderer should support instanced draws generally (not just for particles), which would benefit any system that draws many identical or similar objects.

**Recommendation:** Add a `Renderer3D::DrawParticleBatch()` (or more generally, instanced draw support) that takes the pool arrays, uploads positions/colors/sizes to an instance buffer, and draws all particles in one instanced draw call with a billboard shader. This benefits the renderer as a whole, not just particles.

```
Location: OloEngine/src/OloEngine/Particle/ParticleRenderer.cpp — RenderParticles3D()
```

---

### Issue 2: `ModuleVelocityOverLifetime` compounds speed multiplier every frame

**Severity: High — Simulation Correctness**

```cpp
pool.Velocities[i] = (pool.Velocities[i] + LinearVelocity * dt) * speedMul;
```

The `speedMul` (derived from `SpeedMultiplier * SpeedCurve.Evaluate(age)`) is applied **to the accumulated velocity every frame**. This compounds multiplicatively — if `SpeedMultiplier` is 0.5 and the curve evaluates to 1.0, velocity halves every frame rather than being 50% of original speed.

Unity applies the speed curve as a multiplier of the *initial* speed, not as a per-frame damping factor. The current behavior produces frame-rate-dependent results and exponential decay at any value < 1.0.

**Recommendation:** Store initial velocities per-particle in the pool (add `InitialVelocities` SOA array). Apply the curve as: `velocity = initialVelocity * speedCurve.Evaluate(age) * SpeedMultiplier + additionalForces`.

```
Location: OloEngine/src/OloEngine/Particle/ParticleModules.cpp — ModuleVelocityOverLifetime::Apply()
```

---

### Issue 3: `SimulationSpace::Local` is declared but never implemented

**Severity: High — Feature Correctness**

The `ParticleSpace` enum declares `Local` and `World`, and the editor exposes a combo box to select between them. However, `ParticleSystem::Update()` always passes `emitterPosition` to the emitter and all positions are stored in world space. Moving the entity has no effect on already-spawned particles regardless of the selected space mode.

A user selecting "Local" and seeing identical behavior to "World" is confusing.

**Recommendation:** Either implement Local space (transform particle positions relative to the entity transform each frame, or store positions in local space and transform to world only at render time) or remove the enum/UI until it's implemented.

```
Location: OloEngine/src/OloEngine/Particle/ParticleSystem.cpp — Update()
```

---

### Issue 4: Sub-emitters emit into the parent's pool

**Severity: Medium — Architectural Limitation**

`ProcessSubEmitterTriggers()` calls `m_Pool.Emit()` — sub-emitter particles share the parent's max capacity, modules, and rendering settings. A "death burst" will be visually identical to the parent particles (same color curve, same size curve, same texture).

In Unity and Unreal, sub-emitters are *separate* particle systems with their own independent settings (different colors, sizes, lifetimes, textures). The current design makes sub-emitters functionally useless for most real effects (e.g., sparks on firework death, smoke spawning from fire).

**Recommendation:** Sub-emitter entries should reference separate `ParticleSystem` instances (or `ParticleSystemAsset` handles). The parent collects trigger info; the owning `Scene` or a manager spawns child systems at those positions.

```
Location: OloEngine/src/OloEngine/Particle/ParticleSystem.cpp — ProcessSubEmitterTriggers()
Location: OloEngine/src/OloEngine/Particle/SubEmitter.h
```

---

### Issue 5: `ColorOverLifetime` and `SizeOverLifetime` overwrite initial values

**Severity: Medium — Simulation Correctness**

```cpp
// ColorOverLifetime
pool.Colors[i] = ColorCurve.Evaluate(age);

// SizeOverLifetime
pool.Sizes[i] = SizeCurve.Evaluate(age);
```

These completely replace the emitter's `InitialColor` and `InitialSize` + `SizeVariance`. If a user sets particles to spawn red and enables ColorOverLifetime with a white→transparent curve, the initial red is immediately overwritten with white. The size variance set at emission time is also discarded.

Unity multiplies the curve value by the start value (e.g., `finalColor = startColor * colorOverLifetimeCurve`).

**Recommendation:** Store per-particle initial color and initial size in the pool (add `InitialColors` and `InitialSizes` arrays, populated at emission time). Apply the module as a multiplier: `pool.Colors[i] = pool.InitialColors[i] * ColorCurve.Evaluate(age)`.

```
Location: OloEngine/src/OloEngine/Particle/ParticleModules.cpp — ModuleColorOverLifetime::Apply(), ModuleSizeOverLifetime::Apply()
```

---

### Issue 6: Trail data uses O(n) vector inserts and erases

**Severity: Medium — Performance**

`ParticleTrailData::RecordPoint()` does `trail.insert(trail.begin(), point)` — shifting all existing elements forward every frame. `AgePoints()` uses `erase()` in the middle of the vector. With 1,000 particles × 16 trail points, this is O(n·m) memmoves per frame.

**Recommendation:** Replace `std::vector<TrailPoint>` per particle with a fixed-size ring buffer (circular index into a `std::array<TrailPoint, MaxPoints>`). Inserts become O(1), aging can mark elements dead by advancing a tail index without erasing.

```
Location: OloEngine/src/OloEngine/Particle/ParticleTrail.cpp — RecordPoint(), AgePoints()
```

---

### Issue 7: Trail rendering uses line segments instead of triangle strips

**Severity: Medium — Visual Quality**

`TrailRenderer::RenderTrails3D()` renders trails as `Renderer3D::DrawLine()` segments. Lines have constant width (no tapering), don't respect `WidthStart`/`WidthEnd` properly (the width parameter is passed to `DrawLine` but GPU line width is limited and driver-dependent), and can't be textured.

Unity and Unreal render trails as camera-facing triangle strips with proper width interpolation and UV mapping.

**Recommendation:** Generate quad strips from trail points (expanding each segment perpendicular to the camera and the trail direction) and submit as a single mesh or batch. This would also allow texture mapping along the trail.

```
Location: OloEngine/src/OloEngine/Particle/TrailRenderer.cpp — RenderTrails3D()
```

---

### Issue 8: Noise module uses sin-based pseudo-noise

**Severity: Low–Medium — Visual Quality**

The comment in the code acknowledges this:

```cpp
// Simple pseudo-noise: use sin-based displacement for Phase 1
// Phase 2 will replace with proper Simplex/Perlin noise
```

`sin(i * 0.7 + time * Frequency)` produces very regular, correlated motion — particles at adjacent indices move almost identically. Real turbulence requires spatially-coherent but temporally-varying noise. Simplex noise or curl noise (divergence-free for fluid-like motion) would be a significant visual quality improvement.

**Recommendation:** Implement 3D Simplex noise (or use a library). Evaluate noise at each particle's position for spatial coherence: `offset = simplexNoise3D(position * Frequency + time)`. For even better results, use curl noise (cross product of noise gradients) for divergence-free turbulence.

```
Location: OloEngine/src/OloEngine/Particle/ParticleModules.cpp — ModuleNoise::Apply()
```

---

## Overlooked Issues

### Overlooked 1: No depth sorting or blending mode control

Particles are rendered in pool-index order, not sorted by distance to camera. Transparent particles rendered out of order produce incorrect alpha blending artifacts (back particles drawn on top of front particles). This is especially visible with smoke, fog, or any semi-transparent particle.

The 3D path is particularly affected since particles overlap with scene geometry and each other in complex ways.

Unity supports multiple sort modes (by distance, by age, by oldest/youngest). Unreal Niagara has configurable sort keys.

**Recommendation:** Before rendering, sort alive particle indices by distance to camera (back-to-front for alpha blending). This doesn't require sorting the SOA arrays themselves — sort an index array and render in that order. Also expose a blending mode setting (Alpha, Additive, Premultiplied Alpha) — additive particles don't need sorting at all.

---

### Overlooked 2: No texture atlas / sprite sheet animation

There's a single `Ref<Texture2D>` per system. Most particle effects need animated flipbook/sprite sheet textures (fire frames, smoke puffs, spark sequences, explosions).

Unity has the "Texture Sheet Animation" module (grid-based UV animation over lifetime). Unreal has `SubUV` support with blending between frames.

**Recommendation:** Add a `ModuleTextureSheetAnimation` with grid dimensions (rows × columns), animation mode (over lifetime or by speed), and frame blending. Modify the renderer to compute UV coordinates per-particle based on the current frame.

---

### Overlooked 3: No per-particle entity ID in 3D rendering path

`RenderParticles3D()` doesn't pass entity IDs for mouse picking:

```cpp
// 2D path passes entity ID:
ParticleRenderer::RenderParticles2D(pool, texture, static_cast<int>(entity));

// 3D path does not:
ParticleRenderer::RenderParticles3D(pool, camRight, camUp, texture);
```

Clicking on particles in the 3D viewport won't select the owning entity in the editor. The 2D path handles this correctly.

**Recommendation:** Pass the entity ID through to `RenderParticles3D` and include it in the submitted packets.

---

### Overlooked 4: Particle curves limited to 4 keys with linear interpolation only

`ParticleCurve` uses `std::array<Key, 4>` with piecewise-linear interpolation. Four linear keys cannot represent many useful curves:
- Bell curves (sharp attack, slow decay)
- Ease-in/ease-out
- Oscillating patterns
- Sharp falloff near death

Unity allows arbitrary key counts with Bezier tangent handles. Unreal uses rich curve editors with multiple interpolation modes.

**Recommendation:** At minimum, increase the key limit (8–16) and consider adding Hermite or Catmull-Rom interpolation for smoother curves. Alternatively, use a small LUT (lookup table) baked from an editor curve for O(1) evaluation.

---

### Overlooked 5: No GPU acceleration path

All simulation is CPU-only. Every module iterates all particles sequentially on the main thread. For particle counts above ~10,000, this becomes a bottleneck.

Unity VFX Graph and Unreal Niagara can simulate millions of particles on the GPU via compute shaders.

Even without full GPU simulation, the engine has a task system that could parallelize CPU work — independent modules (Gravity, Drag, Noise) could run in parallel across particle ranges using the existing task system.

**Recommendation:** Short-term: parallelize module application using the task system (split particle range across workers). Long-term: implement a compute shader simulation path for high particle counts.

---

### Overlooked 6: No blend mode / rendering mode options

The system has no concept of blend mode. All particles render with whatever the default blend state is. Common particle blend modes include:

- **Alpha blend** — standard transparency (requires depth sorting)
- **Additive** — fire, sparks, glows (no sorting needed, always brightens)
- **Premultiplied alpha** — better edge quality for atlas textures
- **Soft particles** — fade near scene geometry intersections using the depth buffer

Both Unity and Unreal expose blend mode per particle system. Additive blending alone would make fire and glow effects possible without any sorting.

**Recommendation:** Add a `BlendMode` enum to `ParticleSystemComponent` and set the appropriate GPU blend state before rendering each system.

---

### Overlooked 7: No mesh particle rendering

Particles are always rendered as quads (2D) or billboard quads / cubes (3D). Many effects need mesh particles — debris chunks, leaves, confetti, shells, sparks with 3D geometry.

Unity supports "Mesh" render mode where each particle is an instanced mesh. Unreal Niagara has mesh renderers as a core feature.

**Recommendation:** Add a `RenderMode` enum (Billboard, StretchedBillboard, Mesh) to the component. For mesh mode, each particle would instance a user-specified mesh with the particle's transform. This pairs well with the instanced rendering recommendation from Issue 1.

---

### Overlooked 8: No velocity inheritance from parent rigidbody

When a particle emitter is attached to a moving entity (e.g., a rocket, a car exhaust), newly spawned particles should optionally inherit the entity's velocity so they don't spawn and immediately lag behind. Currently there's no mechanism to feed the entity's rigidbody velocity into the emitter.

Unity has "Inherit Velocity" on the emitter module. Unreal Niagara can bind to the parent component's velocity.

**Recommendation:** In the scene update loop, if the entity has a `Rigidbody3DComponent`, pass its linear velocity to `ParticleEmitter` so it can add it to each spawned particle's initial velocity (scaled by a configurable factor).

---

## Feature Comparison Table

| Feature | Unity | Unreal Niagara | OloEngine Status |
|---------|:-----:|:--------------:|:----------------:|
| SOA particle pool | ✅ (DOTS) | ✅ | ✅ |
| Modular design | ✅ | ✅ | ✅ |
| Editor UI | ✅ | ✅ | ✅ |
| YAML/scene serialization | ✅ | ✅ | ✅ |
| LOD / distance culling | ✅ | ✅ | ✅ |
| Warm-up / pre-simulation | ✅ | ✅ | ✅ |
| Physics collision | ✅ | ✅ | ✅ (Jolt raycasts) |
| Emission shapes | ✅ (7+) | ✅ (many) | ✅ (6 shapes) |
| Force fields | ✅ | ✅ | ✅ |
| Trails | ✅ (ribbon) | ✅ (ribbon) | ⚠️ Lines only |
| Sub-emitters | ✅ (separate systems) | ✅ (events → systems) | ⚠️ Shared pool |
| GPU instanced rendering | ✅ | ✅ | ❌ Per-particle draws |
| Depth sorting | ✅ | ✅ | ❌ |
| Sprite sheet animation | ✅ | ✅ (SubUV) | ❌ |
| Proper noise (Curl/Simplex) | ✅ | ✅ | ❌ Sin-based placeholder |
| Local simulation space | ✅ | ✅ | ❌ Declared, not implemented |
| Blend modes (additive, etc.) | ✅ | ✅ | ❌ |
| Mesh particles | ✅ | ✅ | ❌ Quads only |
| GPU compute simulation | ✅ (VFX Graph) | ✅ | ❌ |
| Particle lights | ✅ | ✅ | ❌ |
| Velocity inheritance | ✅ | ✅ | ❌ |
| Custom per-particle data | ✅ | ✅ (arbitrary attributes) | ❌ Fixed SOA fields |
| Stretched billboards | ✅ | ✅ | ❌ |
| Arbitrary curve keys | ✅ (Bezier) | ✅ (rich curves) | ❌ 4 linear keys max |

---

## Suggested Priority Order

Ranked by impact on usability and visual quality:

| Priority | Item | Type | Effort |
|:--------:|------|------|:------:|
| 1 | Instanced particle rendering (Renderer3D) | Performance | Large |
| 2 | Fix ColorOverLifetime/SizeOverLifetime to multiply by initial values | Bug | Small |
| 3 | Fix VelocityOverLifetime compounding speed multiplier | Bug | Small |
| 4 | Implement Local simulation space (or remove UI) | Bug | Medium |
| 5 | Depth sorting (back-to-front) | Visual Quality | Medium |
| 6 | Blend mode support (at minimum: Alpha, Additive) | Visual Quality | Medium |
| 7 | Sprite sheet / texture atlas animation | Feature | Medium |
| 8 | Replace sin-based noise with Simplex/curl noise | Visual Quality | Medium |
| 9 | Ring buffer for trail data | Performance | Small |
| 10 | Trail rendering as triangle strips | Visual Quality | Medium |
| 11 | Sub-emitters as separate systems | Architecture | Large |
| 12 | Pass entity ID in 3D render path | Bug | Trivial |
| 13 | Mesh particle rendering | Feature | Medium |
| 14 | Velocity inheritance from rigidbody | Feature | Small |
| 15 | Increase curve key count / add smooth interpolation | Feature | Small |
| 16 | Task system parallelization | Performance | Medium |

---

## Conclusion

The particle system has a solid foundation: SOA layout, modular design, ECS integration, serialization, editor UI, and physics collision. The main gaps fall into three categories:

1. **Rendering bottleneck** — Per-particle draw calls in the 3D path (this is also a broader Renderer3D instancing gap that would benefit the entire engine)
2. **Simulation correctness** — Speed multiplier compounding, curve modules overwriting initial values, Local space not implemented
3. **Missing visual features** — No sorting, no blend modes, no sprite sheets, placeholder noise

Addressing items 1–6 in the priority list would make the system genuinely usable for real game effects. Items 7+ are polish and parity with commercial engines.
