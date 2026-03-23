# OloEngine Code Quality & Technical Debt Review

**Date:** March 21, 2026
**Scope:** Code bugs, unfinished features, shader issues, CMake problems, documentation gaps, and Lua scripting status.

---

## 1. Shader Issues

### 1.1 CRITICAL: Bare Uniforms in Compute Shaders (SPIR-V Violation)

The project's GLSL conventions require all non-opaque uniforms to be wrapped in `layout(std140, binding = N) uniform BlockName { ... };` blocks for SPIR-V compatibility. **7 compute/utility shaders violate this rule** with a total of **38 bare uniform declarations**, which cause silent `shaderc` compilation failures.

| Shader | Bare Uniforms | Count |
|--------|---------------|-------|
| `MorphTargetEval.glsl` | `u_VertexCount`, `u_TargetCount` | 2 |
| `LightCulling.comp` | `u_ViewMatrix`, `u_InverseProjectionMatrix`, `u_ScreenWidth`, `u_ScreenHeight`, `u_PointLightCount`, `u_SpotLightCount`, `u_TileSizePixels`, `u_MaxLightsPerTile` | 8 |
| `Particle_Emit.comp` | `u_EmitCount`, `u_MaxParticles` | 2 |
| `Particle_Compact.comp` | `u_MaxParticles` | 1 |
| `Wind_Generate.comp` | `u_GridMin`, `u_GridWorldSize`, `u_GridResolution`, `u_WindDirection`, `u_WindSpeed`, `u_GustStrength`, `u_GustFrequency`, `u_TurbulenceIntensity`, `u_TurbulenceScale`, `u_Time` | 10 |
| `Terrain_Erosion.comp` | `u_Resolution`, `u_MaxDropletSteps`, `u_Inertia`, `u_SedimentCapacity`, `u_MinSedimentCapacity`, `u_DepositSpeed`, `u_ErodeSpeed`, `u_EvaporateSpeed`, `u_Gravity`, `u_InitialWater` | 10 |
| `Precipitation_Feed.comp` | `u_AccumulationFeedRate`, `u_GroundY`, `u_GroundThreshold`, `u_ClipmapCenter`, `u_ClipmapExtent`, `u_ClipmapResolution` | 5 |

**Fix:** Wrap each shader's bare uniforms in a `std140` UBO block using a binding slot from the `ShaderBindingLayout.h` enum, or define new compute-specific binding slots.

### 1.2 Binding Inventory — No Conflicts Found

All 26 UBO binding slots (0–25), 32+ texture binding slots, and 14 SSBO binding slots are properly segregated with no overlaps. The `ShaderBindingLayout.h` enum serves as a single source of truth and is consistently used.

### 1.3 MRT Compliance — Correct

All forward-pass geometry shaders properly output 3 MRT attachments (color, entityID, view-normal) matching the framebuffer configuration.

### 1.4 Include System — Well Organized

11 shared include files under `shaders/include/` with no circular dependencies. Some includes lack `#ifndef` guards but this hasn't caused issues since the shader compiler handles single-TU compilation.

### 1.5 Minor Shader Notes

- Texture binding slots 6–7 (`TEX_ROUGHNESS`, `TEX_METALLIC`) are reserved in the enum but unused by any shader — PBR uses ARM-packed textures instead. Consider documenting these as "reserved/unused" or removing.
- Intentional unused vertex attributes in shadow depth shaders (for vertex layout compatibility) are correctly documented with comments.

---

## 2. Lua Scripting Issues

### 2.1 Lifecycle Callbacks NOT Implemented

The most significant Lua issue: `OnCreate`, `OnUpdate`, and `OnDestroyed` callbacks are **commented out / stubbed** in `LuaScriptEngine.cpp`. Lua scripts can bind to component data and call API functions, but they have no way to run game logic each frame.

This means Lua scripting is currently limited to:
- One-shot operations (create texture, call API)
- SoundGraph audio node processing
- Responding to events if manually wired

It **cannot** be used to write game behavior scripts in the same way as C#.

### 2.2 OloEngine-LuaScriptCore Project Structure is Misleading

- `OloEngine-LuaScriptCore/src/Main.cpp` is 13 lines — just a comment documenting bindings
- The actual Lua engine and bindings live in `OloEngine/src/OloEngine/Scripting/Lua/`
- The separate CMake project builds a shared library (`OloEngine-LuaScriptCore.dll`) but it's mostly empty
- This confuses developers about where Lua code lives

### 2.3 Only One Example Script

`OloEngine-LuaScriptCore/src/assets/Script.lua` demonstrates only texture creation and renderer debugging. No examples exist for:
- Entity component manipulation
- Input handling
- Animation control
- Gameplay system usage (inventory, quests, abilities)
- Any actual game logic

### 2.4 Lua API is Actually More Complete Than C#

Despite the lifecycle issue, the Lua binding layer (`LuaScriptGlue.cpp`, ~800 lines) exposes 40+ components and all major systems (animation, AI, navigation, quest, inventory, abilities, dialogue, save, network). The Lua API surface is significantly wider than C# — the issue is that without lifecycle hooks, none of it can be used in game loops.

---

## 3. C# Scripting Issues

### 3.1 Component Binding Gap

Only ~10 component types have C# wrappers in `OloEngine-ScriptCore/src/OloEngine/Components.cs`, compared to 40+ in Lua. Missing C# bindings:

- AnimationGraphComponent (SetFloat/Bool/Int, SetTrigger, GetCurrentState)
- MorphTargetComponent (SetWeight, ApplyExpression)
- QuestJournalComponent (AcceptQuest, CompleteQuest, objective tracking)
- InventoryComponent (AddItem, RemoveItem, HasItem, CountItem)
- AbilityComponent (GetAttribute, SetAttribute, HasTag, abilities)
- DialogueComponent (start, advance, select_choice)
- BehaviorTreeComponent / StateMachineComponent (blackboard operations)
- All lighting components (DirectionalLight, PointLight, SpotLight)
- All 3D physics querying from script

**Now implemented:** NavAgentComponent and MaterialComponent have C# wrappers and are used in gameplay scripts (e.g., `PlayerController.cs`).

### 3.2 Stub Component Wrappers in C#

Several C# component classes exist but are empty bodies:
- `WaterComponent` — marked "Stub — no scripting bindings yet"
- `ItemPickupComponent` — empty class body
- `ItemContainerComponent` — empty class body
- `QuestGiverComponent` — empty class body

### ~~3.3 Missing Physics.Raycast and Camera.ScreenToWorldRay~~ — **DONE**

**Resolved.** `Physics.Raycast` and `Camera.ScreenToWorldRay` are now exposed to both C# (via Mono `InternalCall`) and Lua (via Sol2 bindings). Additionally, cross-entity damage routing (`ApplyDamageToTarget`, `TryActivateAbilityOnTarget`) has been added to both scripting languages.

### ~~3.4 Mono stdout Not Redirected~~ — **DONE**

**Resolved.** `mono_trace_set_print_handler()` and `mono_trace_set_printerr_handler()` are called in `ScriptEngine::InitMono()`, routing Mono stdout/stderr through `OLO_CLIENT_INFO` / `OLO_CLIENT_ERROR`. `Console.WriteLine()` now appears in the editor ConsolePanel.

---

## 4. CMake / Build System Issues

### 4.1 No Install or Packaging Targets

The CMake configuration has **no `install()` rules** and **no CPack configuration**. However, the `GameBuildPipeline` (invoked from the editor’s `BuildGamePanel`) now handles standalone game assembly.

Remaining gaps:
- No `cmake --install` workflow for creating a deployable *engine/editor* package
- No automated creation of ZIP/MSI/NSIS installers for the engine itself

**Improvement (March 2026):** `add_dependencies(OloEditor OloRuntime)` ensures OloRuntime is always rebuilt alongside OloEditor, and `CopyRuntimeExecutable` now warns at build time if the OloRuntime binary is stale.

### 4.2 Dist Configuration Ambiguity

The `Dist` build type inherits from `Release` flags but:
- `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` is set for `Debug,Release` but **not Dist** — unclear if Dist includes PDB or not
- No explicit symbol stripping configured for Dist
- No documented difference between Release and Dist beyond the preprocessor define `OLO_DIST`

### ~~4.3 VULKAN_SDK Environment Variable Required~~ — **DONE**

**Resolved.** `OloEngine/CMakeLists.txt` now checks for `$ENV{VULKAN_SDK}` early and issues a clear `message(FATAL_ERROR ...)` with instructions to install the Vulkan SDK and set the environment variable.

### 4.4 Mono Libraries Checked-In

`OloEngine/mono/lib/` contains pre-built Mono static libraries committed to the repository. No versioning metadata, license attribution in CMake, or mechanism to update/rebuild them. This works but is fragile for long-term maintenance.

### 4.5 No Asset Pipeline Integration

CMake does not:
- Pre-compile shaders to SPIR-V as a build step
- Validate shader correctness at build time
- Process/optimize textures or models
- Build asset packs for distribution

Shaders are compiled at runtime on first load and cached to `OloEditor/assets/cache/shader/`.

### 4.6 Working Directory Not Enforced

The editor and tests require `OloEditor/` as the working directory. This is documented but not enforced in CMake — the VS Code tasks handle it correctly, but Visual Studio solution users may miss this.

---

## 5. Asset Serializer Issues

### 5.1 Font Serialization Stub

`AssetSerializer.cpp` has TODO comments at the font serialization functions — font file read/write is not fully implemented. This means font assets may not survive a full asset pack export.

### 5.2 Environment Map Loading Stub

Loading environment maps from file is marked as not implemented in the asset serializer.

### 5.3 MeshSource / Mesh Pack Serialization Missing

`AssetSerializer.cpp` has completely empty implementations for MeshSource and mesh pack serialization. Mesh data is loaded on-demand from source files (FBX/OBJ via Assimp) rather than from optimized packed format.

### 5.4 Audio Metadata Analysis Incomplete

Audio asset metadata analysis is partially implemented — the serializer can save/load audio source components but full audio metadata (duration, channel count, sample rate extraction) is noted as incomplete.

### 5.5 SoundGraph Cache Serialization Stubs

`SoundGraphCache.cpp` has empty serialization and deserialization methods — SoundGraph configurations are not cached to disk.

---

## 6. Scene Serialization Coverage

Contrary to initial concerns, scene serialization is **extensive** — covering virtually all component types:

**Serialized (verified in SceneSerializer.cpp):**
- Core: Transform, Camera, Script, Tag, ID, Relationship, Prefab
- Rendering: Sprite, Circle, Mesh, Submesh, Model, Material, Text, LODGroup
- Lighting: DirectionalLight, PointLight, SpotLight, EnvironmentMap, LightProbe, LightProbeVolume
- 2D Physics: Rigidbody2D, BoxCollider2D, CircleCollider2D
- 3D Physics: Rigidbody3D, Box/Sphere/Capsule/Mesh/ConvexMesh/TriangleMesh/CompoundColliders, CharacterController3D
- Audio: AudioSource, AudioListener
- Animation: AnimationState, Skeleton, AnimationGraph, MorphTarget, Submesh
- Terrain: Terrain (heightmap, splatmap, layers, streaming, voxel), Foliage, Water
- Effects: ParticleSystem, Decal, FogVolume, SnowDeformer, Precipitation
- UI: UICanvas, UIRectTransform, all 12 widget types
- AI/Navigation: NavMeshBounds, NavAgent, BehaviorTree, StateMachine
- Networking: NetworkIdentity, NetworkInterest, NetworkLOD, InstancePortal
- Gameplay: Inventory, ItemPickup, ItemContainer, QuestJournal, QuestGiver, AbilityComponent, Dialogue
- Streaming: StreamingVolume

This is **~55+ component types serialized** — comprehensive coverage with proper sanitization/validation on deserialization.

---

## 7. RuntimeAssetManager — Stub Implementation

`RuntimeAssetManager` is intended for shipping builds (loading from packed `.olopack` files) but is mostly non-functional:

- `LoadAssetPack()` — **not implemented** (the core function)
- `GetAssetTypeFromPacks()` — returns placeholder types
- `ReloadData`, `EnsureCurrent` — mostly no-op
- Constructor references `Assets/AssetPack.olopack` which doesn't exist

The `EditorAssetManager` (file-based loading) is what actually runs at all times, including in "Dist" builds. This means the asset packing infrastructure has no consumer.

---

## 8. Documentation Gaps

### What Exists (Good)
- `README.md` — Good project overview
- `docs/deployment.md` — Comprehensive OloServer deployment guide
- `docs/UI_SYSTEM_GUIDE.md` — Complete UI system architecture with all 14 widgets
- `docs/gameplay_system_missing_features.md` — Honest and detailed gap analysis (9 items)
- `CONTRIBUTING.md` — Pre-commit hooks, formatting conventions
- `.github/instructions/` — GLSL shader guidelines, C++ language service tool docs
- `.github/copilot-instructions.md` — Thorough AI agent guide

### What's Missing
- **Getting Started Guide** — No "create your first scene, add a script, hit play" walkthrough
- **API Reference** — No auto-generated docs (Doxygen etc.) for C++ APIs
- **Architecture Overview** — Design patterns, data flow, and system interactions aren't documented outside AI context files
- **Scripting Tutorial** — No guide for writing C# or Lua game scripts
- **Troubleshooting / FAQ** — Common build errors and runtime issues undocumented
- **Shader Authoring Guide** — GLSL instruction file exists but is in `.github/instructions/` (AI-facing), not in developer docs

---

## 9. Code Quality Spot-Check

### 9.1 TODO/FIXME/HACK Comments (~30+)

The codebase has ~30+ TODO comments. Most significant:

**High Priority:**
- `AssetSerializer.cpp` — Font serialization not implemented
- `AssetSerializer.cpp` — EnvironmentMap file loading not implemented
- `AssetSerializer.cpp` — MeshSource/mesh pack serialization empty
- `JoltShapes.cpp` — Mesh volume/center-of-mass calculation uses sphere approximation placeholder
- `Framebuffer.h` — `BindColorAttachment`, `BindDepthAttachment` methods not added
- `IBLPrecompute.cpp` — Parallel face rendering and quality-based optimizations pending
- `ShaderDebugger.cpp` — Shader reload trigger incomplete

**Low Priority:**
- `Snow_Accumulate.comp` — Slope-aware deformation and wind field sampling TODOs
- Various "Phase N" comments in newer systems indicating planned future work

### 9.2 Memory Management — Robust

- Consistent `Ref<T>` (RefCounted) smart pointer usage throughout
- OpenGL resources properly cleaned up in destructors
- CommandMemoryManager uses thread-local allocators
- No obvious memory leaks in critical paths

### 9.3 Deserialization Sanitization — Excellent

The SceneSerializer extensively validates loaded data:
- `SanitizeFloat()` — Replaces NaN/Inf with defaults, clamps to valid ranges
- `SanitizeVec2/3()` — Per-component NaN checks with fallbacks
- Enum bounds checking on all deserialized enums
- Grid/resolution clamping to prevent absurd allocations
- Array size validation before iteration

This is significantly better than most game engines and prevents corrupted scene files from crashing the editor.

---

## 10. Test Coverage

The test suite covers 90+ test files across:
- Core utilities, containers, templates, async
- Rendering system tiers (1-5), culling, LOD, command buckets
- Networking (P2P, MMO zones, persistence, chat, rollback)
- Dialogue, SaveGame, ShaderGraph, Navigation, AI
- Animation, MorphTargets, Inventory, Quests, Abilities

**Strengths:** Wide coverage of subsystems, especially networking and gameplay.
**Gap:** No integration tests for the full editor workflow (create entity → add component → serialize → deserialize → verify).

---

## Summary Table

| Category | Status | Severity | Notes |
|----------|--------|----------|-------|
| Shader bare uniforms (compute) | ❌ Bug | Critical | 38 violations in 7 shaders, silent SPIR-V failures |
| RuntimeAssetManager | ❌ Stub | Critical | Core function `LoadAssetPack()` not implemented |
| C# component bindings | ⚠️ Incomplete | High | ~10 of 40+ components exposed |
| Lua lifecycle (OnUpdate) | ❌ Stub | High | Lua scripts can't run game logic loops |
| Font/EnvironmentMap serialization | ❌ Stub | High | Asset types not fully serializable |
| MeshSource pack serialization | ❌ Stub | Medium | Meshes loaded from source, not packed |
| CMake install/packaging | ❌ Missing | Medium | No deployment pipeline |
| CMake Dist vs Release | ⚠️ Unclear | Medium | No clear differentiation |
| SoundGraph cache | ❌ Stub | Low | No disk caching for SoundGraph configs |
| Documentation gaps | ⚠️ Gaps | Medium | No getting started guide or API docs |
| Scene serialization | ✅ Complete | — | 55+ components with full sanitization |
| Memory management | ✅ Solid | — | Consistent RAII, no obvious leaks |
| UBO/SSBO/texture bindings | ✅ Correct | — | Zero conflicts across all shaders |
| Deserialization validation | ✅ Excellent | — | NaN/Inf/bounds checking everywhere |
| Test coverage | ✅ Good | — | 90+ test files across all subsystems |
