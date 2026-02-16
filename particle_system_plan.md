# Plan: Particle System for OloEngine

## TL;DR
Add a full-featured particle system to OloEngine in two phases. Phase 1 delivers CPU-simulated particles with modular emitters, lifetime modifiers, billboard/2D rendering, editor UI, serialization, and script bindings. Phase 2 adds trails/ribbons, sub-emitters, collision, LOD, and a GPU compute simulation backend. SOA data layout with `ParallelFor` enables high particle counts from the start.

---

## Architecture Overview

```
ParticleSystemComponent (ECS)
  └── ParticleSystem (owns emitters, orchestrates update/render)
        ├── ParticleEmitter (emission shape, rate, bursts, initial properties)
        │     └── EmissionShape: Point, Sphere, Box, Cone, Ring, Edge
        ├── ParticlePool (SOA storage: position[], velocity[], color[], size[], life[], rotation[])
        ├── ParticleModules[] (pluggable modifiers applied each frame)
        │     ├── ColorOverLifetimeModule
        │     ├── SizeOverLifetimeModule
        │     ├── VelocityOverLifetimeModule
        │     ├── RotationOverLifetimeModule
        │     ├── GravityModule
        │     ├── NoiseModule
        │     ├── ForceFieldModule (Phase 2)
        │     ├── CollisionModule (Phase 2)
        │     └── SubEmitterModule (Phase 2)
        └── ParticleRenderer (rendering strategy)
              ├── BillboardRenderer (camera-facing quads, 3D)
              ├── Quad2DRenderer (flat quads via Renderer2D, 2D)
              ├── StretchedBillboardRenderer (velocity-aligned, 3D)
              └── TrailRenderer (Phase 2)
```

---

## PHASE 1 — Core Particle System

### Step 1: Particle Data Pool (SOA Storage)
*No dependencies — can start immediately*

Create `OloEngine/src/OloEngine/Particle/ParticlePool.h/.cpp`

- **SOA layout**: Separate contiguous arrays for each particle attribute:
  - `std::vector<glm::vec3> Positions`
  - `std::vector<glm::vec3> Velocities`
  - `std::vector<glm::vec4> Colors` (current interpolated)
  - `std::vector<f32> Sizes` (current interpolated)
  - `std::vector<f32> Rotations`
  - `std::vector<f32> Lifetimes` (remaining)
  - `std::vector<f32> MaxLifetimes` (initial, for normalized age)
  - `std::vector<u8> Active` (alive flag, avoids branches with swap-to-back removal)
- **Fixed-capacity pool**: Pre-allocate to `MaxParticles` (configurable). Active count tracked separately. Dead particles swapped with last alive — O(1) removal.
- **Memory**: Use LLM `Particles` tag (`OloEngine/src/OloEngine/HAL/LowLevelMemTracker.h` already has it) for tracking allocations.
- Interface: `Emit(count)`, `Kill(index)`, `Compact()`, `GetAliveCount()`, `GetAge(index)` (returns 0..1 normalized age).

### Step 2: Emission Shapes & Emitter
*No dependencies — parallel with Step 1*

Create `OloEngine/src/OloEngine/Particle/EmissionShape.h`
Create `OloEngine/src/OloEngine/Particle/ParticleEmitter.h/.cpp`

**EmissionShape** — polymorphic or variant-based (prefer `std::variant` to avoid vtable overhead):
- `PointShape` — all particles emit from origin
- `SphereShape { f32 Radius; bool Surface; }` — random inside volume or on surface
- `BoxShape { glm::vec3 HalfExtents; }` — random within AABB
- `ConeShape { f32 Angle; f32 Radius; f32 Length; }` — cone emission
- `RingShape { f32 Radius; f32 Thickness; }` — torus cross-section
- `EdgeShape { glm::vec3 Start; glm::vec3 End; }` — line segment

Each shape provides `Sample(FastRandomPCG&) -> {position, direction}` — uses `FastRandom` from `Core/FastRandom.h` for thread-safe randomness.

**ParticleEmitter** — controls when and how particles spawn:
- `EmissionRate` (particles/sec) with fractional accumulator
- `Bursts` — `std::vector<Burst>` where `Burst { f32 Time; u32 Count; u32 Cycles; f32 Interval; }`
- `InitialSpeed { f32 Min, Max }` — random range
- `InitialSize { f32 Min, Max }`
- `InitialColor { glm::vec4 Min, Max }` — lerp or random per-channel
- `InitialLifetime { f32 Min, Max }`
- `InitialRotation { f32 Min, Max }`
- `SimulationSpace` enum: `Local` (transforms with entity) vs `World` (detached)

### Step 3: Particle Modules (Lifetime Modifiers)
*Depends on Step 1 (ParticlePool interface)*

Create `OloEngine/src/OloEngine/Particle/ParticleModules.h/.cpp`

Base module concept (not inheritance — use function pointers or `std::variant` for data orientation):

Each module implements `Update(ParticlePool& pool, f32 dt, u32 startIndex, u32 count)` — designed for `ParallelFor` chunked invocation.

**Phase 1 modules:**
- **ColorOverLifetime** — Start/End `glm::vec4`, lerp by normalized age. Optionally a gradient (up to 4 color keys with positions 0..1).
- **SizeOverLifetime** — Start/End `f32`, lerp by normalized age. Optional curve (linear, ease-in, ease-out, custom Bezier with 2 control points).
- **VelocityOverLifetime** — Additive velocity modifier `glm::vec3` (constant wind/drift).
- **RotationOverLifetime** — Angular velocity `f32` (degrees/sec).
- **GravityModule** — `glm::vec3 Gravity` (default `{0, -9.81, 0}`), multiplied by `GravityScale`.
- **DragModule** — Linear drag coefficient, `Velocity *= (1 - Drag * dt)`.
- **NoiseModule** — Simplex/Perlin noise-based velocity perturbation. `f32 Strength, f32 Frequency, f32 ScrollSpeed`. Use `choc` library (already in deps) or implement basic 3D simplex.

Curve representation: `ParticleCurve` struct with `std::array<CurveKey, 4>` where `CurveKey { f32 Time; f32 Value; }` — simple piecewise linear interpolation. Sufficient for Phase 1; can be upgraded to Bezier curves later.

### Step 4: ParticleSystem (Orchestrator)
*Depends on Steps 1-3*

Create `OloEngine/src/OloEngine/Particle/ParticleSystem.h/.cpp`

- Owns: `ParticlePool`, `ParticleEmitter`, `std::vector<ParticleModuleVariant>` (variant of all module types)
- Configuration: `u32 MaxParticles`, `f32 Duration`, `bool Looping`, `f32 StartDelay`, `bool PlayOnAwake`
- Playback state: `Playing`, `Paused`, `Stopped` enum. `f32 m_ElapsedTime`.
- **Update(Timestep dt, glm::mat4 transform)**:
  1. Accumulate emission → spawn new particles via emitter + shape
  2. Apply modules via `ParallelFor` (chunk particles across workers, each module processes its chunk) — reuse existing `OloEngine/src/OloEngine/Task/ParallelFor.h`
  3. Advance lifetimes, kill expired particles, compact pool
  4. Profile with `OLO_PROFILE_FUNCTION()`
- **Simulation space handling**: If `Local`, particle positions are in emitter-local space — transform applied at render time. If `World`, particle positions are transformed to world space at emission time.
- Thread safety: Each `ParticleSystem` is single-owner (one component), but internal simulation uses `ParallelFor` for data-parallel work.

### Step 5: Particle Rendering
*Depends on Steps 1, 4*

Create `OloEngine/src/OloEngine/Particle/ParticleRenderer.h/.cpp`
Create `OloEditor/assets/shaders/Renderer2D_Particle.glsl` (optional — may reuse Quad shader)

**Two render paths:**

**A) 2D Path (via Renderer2D):**
- Iterate alive particles → call `Renderer2D::DrawQuad(transform, texture, uvMin, uvMax, tintColor, entityID)`
- Build per-particle transform matrix from position, size, rotation
- Leverages existing batching (20K quads/batch, 32 textures)
- Sprite sheet animation: compute UV sub-rect from `FrameIndex` based on particle age and `SpriteSheetLayout { u32 Columns, Rows; f32 FPS; }`

**B) 3D Path (Billboard):**
- Custom shader `Particle_Billboard.glsl` using instanced rendering
- Per-instance data: `vec3 Position, vec4 Color, f32 Size, f32 Rotation` — packed into instance VBO
- Vertex shader constructs billboard quad from camera right/up vectors (passed via UBO)
- Fragment shader: textured quad with alpha * vertex color
- Submit via `RenderCommand::DrawIndexedInstanced()` — single draw call per texture batch
- Stretched billboard variant: align quad along velocity direction

**Shared concerns:**
- `ParticleRenderMode` enum: `Billboard`, `StretchedBillboard`, `HorizontalBillboard`, `Quad2D`
- Sorting: Optional back-to-front sort for correct alpha blending (sort by camera distance). Use `std::sort` or radix sort on distance key. Sorting is expensive — make it optional (`bool SortByDistance`).
- Texture: `Ref<Texture2D>` for particle atlas. `BlendMode` enum: `Additive`, `AlphaBlend`, `Premultiplied`.
- Sprite sheet: `SpriteSheet { u32 Columns, Rows; f32 FramesPerSecond; bool RandomStartFrame; }` — compute UV rect per particle.

### Step 6: ECS Component + Scene Integration
*Depends on Steps 4-5*

**ParticleSystemComponent** in `OloEngine/src/OloEngine/Scene/Components.h`:
```cpp
struct ParticleSystemComponent {
    Ref<ParticleSystem> System;            // Owned particle system instance
    Ref<Texture2D> Texture;                // Particle texture/atlas (optional)
    ParticleRenderMode RenderMode = ParticleRenderMode::Billboard;
    BlendMode Blend = BlendMode::AlphaBlend;
    bool SortByDistance = false;
    // Runtime state:
    bool m_IsPlaying = false;
};
```
- Add to `AllComponents` type alias in `Components.h`
- Add `OnComponentAdded<ParticleSystemComponent>` specialization in `Scene.cpp`

**Scene update** in `Scene::OnUpdateRuntime()` and `Scene::RenderScene()`:
- New iteration: `view<TransformComponent, ParticleSystemComponent>` 
  → For each: `system->Update(ts, transform)`, then render via appropriate path
- Add between animation update and 2D rendering passes

### Step 7: Serialization
*Depends on Step 6*

In `OloEngine/src/OloEngine/Scene/SceneSerializer.cpp`:

Serialize/deserialize `ParticleSystemComponent` following the `SpriteRendererComponent` pattern:
- Emission shape type + parameters
- Emitter settings (rate, bursts, initial property ranges)
- Each module's settings (color gradient keys, size curve, gravity, drag, noise params)
- System settings (MaxParticles, Duration, Looping, PlayOnAwake)
- Render settings (RenderMode, BlendMode, SortByDistance)
- Texture path (if set)
- Sprite sheet layout (if applicable)

Both `SerializeEntity` and `DeserializeEntity` paths, plus YAML string variants.

### Step 8: Editor UI
*Depends on Steps 6-7*

In `OloEditor/src/Panels/SceneHierarchyPanel.cpp`:

**Add Component menu:**
- `DisplayAddComponentEntry<ParticleSystemComponent>("Particle System")` — in a new "Effects" section  

**DrawComponent lambda** — comprehensive property editor:
- **System section**: MaxParticles (DragInt), Duration (DragFloat), Looping (Checkbox), PlayOnAwake (Checkbox)
- **Playback controls**: Play/Pause/Stop/Restart buttons
- **Emission section**: Rate (DragFloat), Shape type (Combo dropdown), shape-specific params
- **Bursts sub-section**: Editable list (Add/Remove burst entries)
- **Initial Values section**: Speed, Size, Color, Lifetime, Rotation — all as min/max ranges (DragFloat2)
- **Modules section**: Collapsible tree nodes per module, each toggleable (Checkbox + settings)
  - Color Over Lifetime: Start/End color pickers, optional gradient keys
  - Size Over Lifetime: Start/End drag, curve type selector
  - Gravity: vec3 drag + scale
  - Drag: coefficient slider
  - Noise: strength, frequency, scroll speed
  - Rotation Over Lifetime: angular velocity
  - Velocity Over Lifetime: vec3 drift
- **Rendering section**: RenderMode combo, BlendMode combo, SortByDistance checkbox
- **Texture**: Drag-drop from Content Browser (existing pattern), sprite sheet columns/rows/fps
- **Preview**: The viewport already renders the scene — particles are visible live

### Step 9: Particle Shader (3D Billboard)
*Parallel with Steps 5-8*

Create `OloEditor/assets/shaders/Particle_Billboard.glsl`:
- `#type vertex`: Takes per-vertex quad corners (0,0 / 1,0 / 1,1 / 0,1) + per-instance position/color/size/rotation. Constructs billboard from camera right/up (from UBO). Applies rotation around view direction.
- `#type fragment`: Sample texture atlas at UV, multiply by vertex color, apply alpha. Support additive blending mode via uniform.
- Uniform buffer binding 0: camera ViewProjection + CameraRight + CameraUp vectors.

### Step 10: Script Bindings
*Depends on Step 6*

**C# (OloEngine-ScriptCore):**
- In `Components.cs`: Add `ParticleSystemComponent` class with properties: `IsPlaying`, `MaxParticles`, `EmissionRate`, `Play()`, `Pause()`, `Stop()`, `Restart()`, `Emit(int count)` (manual burst).
- In `InternalCalls.cs` + `ScriptGlue.cpp`: Register internal calls for the above properties/methods.

**Lua (Sol2):**
- In `LuaScriptGlue.cpp`: Register `ParticleSystemComponent` usertype with same properties/methods.

### Step 11: Tests
*Depends on Steps 1-4*

In `OloEngine/tests/`:
- `ParticlePoolTests.cpp`: Emit, kill, compact, capacity limits, SOA integrity.
- `ParticleEmitterTests.cpp`: Rate emission accumulator accuracy, burst timing, shape sampling distribution.
- `ParticleModuleTests.cpp`: Color/size interpolation at age 0, 0.5, 1.0. Gravity acceleration correctness. Drag convergence.
- `ParticleSystemTests.cpp`: Full lifecycle — create, play, update N frames, verify particle count and positions.

---

## PHASE 2 — Advanced Features (Future)

### Step 12: Trail / Ribbon Renderer
- Track particle position history (ring buffer per particle, N trail points)
- Generate triangle strip mesh from trail points with width modulation
- Dedicated `Particle_Trail.glsl` shader
- New render mode: `ParticleRenderMode::Trail`

### Step 13: Sub-Emitters
- `SubEmitterModule` — trigger child `ParticleSystem` on events: `OnBirth`, `OnDeath`, `OnCollision`
- Child systems inherit parent particle's position/velocity at trigger time
- Managed by parent `ParticleSystem`, pooled to avoid allocations

### Step 14: Collision Module
- Simple plane collision (world ground plane)
- Scene depth buffer collision (read depth buffer, reconstruct world position, bounce)
- `CollisionModule { f32 Bounce; f32 LifetimeLoss; bool KillOnCollide; CollisionMode Mode; }`
- Jolt raycast-based collision (opt-in, expensive — limited particle count)

### Step 15: GPU Compute Simulation
- `ParticleComputeBackend` — alternative to CPU `ParticlePool` + module updates
- Particle data lives in SSBOs (Shader Storage Buffer Objects)
- Compute shader: emission, module updates, death/compaction via atomic counters + prefix sum
- Indirect draw: `glDrawArraysIndirect` reads alive count from compute output
- Shared particle shader for rendering (same billboard vertex shader)
- Abstraction: `IParticleSimulator` interface — `CPUParticleSimulator` (Phase 1) vs `GPUParticleSimulator` (Phase 2)
- Design the Phase 1 `ParticleSystem` with this boundary in mind: simulation is separate from rendering.

### Step 16: LOD & Performance
- Distance-based particle count reduction (spawn rate multiplier by LOD level)
- Screen-space size culling (skip particles smaller than N pixels)
- Fixed time-step sub-stepping for deterministic simulation
- Warm-up: Pre-simulate N seconds on first play to avoid "popping" effect

### Step 17: Particle System Asset
- Register `ParticleSystem` as an asset type in `AssetTypes.h` (new enum value)
- Save/load `.oloparticle` files via `AssetManager`
- Drag-drop particle assets onto entities from Content Browser
- Hot-reload support via `AssetReloadedEvent`

---

## Relevant Files

### New files to create:
- `OloEngine/src/OloEngine/Particle/ParticlePool.h/.cpp` — SOA particle storage
- `OloEngine/src/OloEngine/Particle/EmissionShape.h` — Emission shape variants
- `OloEngine/src/OloEngine/Particle/ParticleEmitter.h/.cpp` — Emission logic
- `OloEngine/src/OloEngine/Particle/ParticleModules.h/.cpp` — Lifetime modifier modules
- `OloEngine/src/OloEngine/Particle/ParticleSystem.h/.cpp` — Orchestrator
- `OloEngine/src/OloEngine/Particle/ParticleRenderer.h/.cpp` — Render strategies
- `OloEngine/src/OloEngine/Particle/ParticleCurve.h` — Simple piecewise-linear curve for module interpolation
- `OloEditor/assets/shaders/Particle_Billboard.glsl` — 3D billboard instanced shader
- `OloEngine/tests/Particle/` — Test files

### Existing files to modify:
- `OloEngine/src/OloEngine/Scene/Components.h` — Add `ParticleSystemComponent`, update `AllComponents`
- `OloEngine/src/OloEngine/Scene/Scene.h/.cpp` — Add `OnComponentAdded<>` specialization, particle update+render in `OnUpdateRuntime()` and `RenderScene()`/`RenderScene3D()`
- `OloEngine/src/OloEngine/Scene/SceneSerializer.cpp` — Serialize/deserialize particle component
- `OloEditor/src/Panels/SceneHierarchyPanel.cpp` — Add component menu entry + property editor UI
- `OloEngine-ScriptCore/src/OloEngine/Scene/Components.cs` — C# component wrapper
- `OloEngine-ScriptCore/src/OloEngine/InternalCalls.cs` — C# internal calls
- `OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp` — Register native functions
- `OloEngine/src/OloEngine/Scripting/Lua/LuaScriptGlue.cpp` — Lua bindings
- `OloEngine/CMakeLists.txt` — Add new source files to build

### Reference files (patterns to follow):
- `OloEngine/src/OloEngine/Scene/Components.h` — `SpriteRendererComponent` as component template
- `OloEngine/src/OloEngine/Scene/SceneSerializer.cpp` — Serialization pattern
- `OloEditor/src/Panels/SceneHierarchyPanel.cpp` — Editor UI pattern (`DrawComponent`, `DisplayAddComponentEntry`)
- `OloEngine/src/OloEngine/Task/ParallelFor.h` — For parallel particle simulation
- `OloEngine/src/OloEngine/Core/FastRandom.h` — Thread-safe randomness (`FastRandomPCG`)
- `OloEngine/src/OloEngine/Renderer/Renderer2D.cpp` — Batched quad rendering, sprite sheet UVs
- `OloEngine/src/OloEngine/Renderer/Renderer3D.h` — Command bucket, instanced rendering pattern

---

## Verification

### Automated:
1. Run `run-tests-debug` after implementing Steps 1-4 to validate pool/emitter/module/system logic.
2. Build `build-oloeditor-debug` after each step to catch compile errors incrementally.
3. Add Google Test cases covering: pool capacity, emission rate accuracy, module interpolation math, serialization round-trip (serialize→deserialize→compare).

### Manual:
1. After Step 8: Add a `ParticleSystemComponent` to an entity in OloEditor, verify particles render in viewport.
2. Test emission shapes visually: switch between Point/Sphere/Box/Cone and observe distribution.
3. Test modules: toggle Color Over Lifetime — particles should fade; toggle Gravity — particles should fall.
4. Save scene, reload — verify particle settings persist.
5. Profile with Tracy (`OLO_PROFILE_FUNCTION`) — check that 10K particles run at 60fps.
6. Test play/pause/stop controls in editor.
7. Test C# script: `GetComponent<ParticleSystemComponent>().Play()` in a MonoBehaviour-style script.

---

## Decisions
- **SOA data layout** for cache-friendly parallel simulation via `ParallelFor`
- **Phased approach**: Phase 1 ships a complete, usable particle system. Phase 2 adds advanced features.
- **Both 2D and 3D rendering** — 2D uses existing Renderer2D batching, 3D uses custom instanced billboard shader
- **GPU compute deferred to Phase 2** — CPU simulation with clear `ISimulator` abstraction boundary
- **Modules as `std::variant`** not virtual classes — avoids vtable overhead in hot loops
- **`Ref<ParticleSystem>` owned by component** — not an asset in Phase 1 (asset-ification in Phase 2, Step 17)
- **Emission shapes via `std::variant`** — same rationale as modules
- Curve system is minimal (piecewise linear, 4 keys) — sufficient for Phase 1, upgradeable later

## Further Considerations
1. **Warm-up simulation**: Should particles pre-simulate when entering play mode so looping systems aren't empty at frame 0? Recommend yes — add a `WarmUpTime` field (Phase 1 scope creep is minimal).
2. **Particle sorting cost**: Back-to-front sorting is O(n log n) per frame. For additive-blend particles sorting is unnecessary. Consider making additive the default blend mode and only sort when alpha-blend is selected.
3. **Editor-only preview**: Should particles play in edit mode (like Unity's particle preview)? Recommend gating behind a "Simulate" toggle in the editor panel — playing automatically in edit mode could be distracting.
