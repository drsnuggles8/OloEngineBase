# OloEngine — Feature Gap Analysis

This document identifies features that are commonly expected in a production game engine but are currently missing or incomplete in OloEngine. Each section is rated by **priority** (Critical / High / Medium / Low) based on how essential the feature is for shipping a game.

---

## Summary

| Category | Gap | Priority |
|----------|-----|----------|
| Networking | No multiplayer / replication system | High |
| AI | No behaviour trees, state machines, or blackboard | High |
| Navigation | No NavMesh generation or pathfinding | High |
| Input | No high-level action-mapping / rebinding system | High |
| Rendering — LOD | No mesh LOD selection or streaming | High |
| Rendering — Culling | No frustum or occlusion culling | Critical |
| Rendering — GI | No global illumination or light probes | Medium |
| Rendering — Decals | No decal projection system | Medium |
| Rendering — Volumetrics | No volumetric fog or volumetric lighting | Medium |
| Scene Management | No runtime level / scene streaming | High |
| Save / Load | No game-state serialization for save files | High |
| Localization | No string-table or locale system | Medium |
| Video | No video / cinematic playback | Low |
| Dialogue | No dialogue / conversation framework | Low |
| Editor — Material Editor | No node-based material / shader graph | Medium |
| Editor — Animation Editor | No animation timeline / state-machine editor | Medium |
| Editor — Prefab Workflow | Prefabs exist but no nested-prefab or override support visible | Medium |
| Cross-Platform | Windows-only; no Linux, macOS, or console abstraction | Medium |

---

## 1  Rendering Gaps

### 1.1  Frustum & Occlusion Culling — **Critical**

**What is missing:** There is no frustum culling pass that discards off-screen objects before draw-call submission, and no occlusion culling (hardware or software) to skip objects hidden behind other geometry.

**Why it matters:** Without culling, every mesh in the scene is submitted to the GPU every frame. Any non-trivial scene (hundreds of objects) will be severely draw-call bound.

**Recommended approach:**
- Add a frustum-culling pass using AABB tests against the camera frustum.
- Add optional GPU-driven occlusion culling (Hi-Z buffer or compute-based).

---

### 1.2  Level-of-Detail (LOD) — **High**

**What is missing:** There is no system to automatically switch mesh or material detail levels based on distance from the camera.

**Why it matters:** Rendering high-poly meshes at large distances wastes GPU fill rate and vertex throughput. LOD is one of the most impactful performance optimizations in any 3D engine.

**Recommended approach:**
- Add an `LODGroup` component that holds multiple mesh references with distance thresholds.
- Integrate LOD selection into the render submission pipeline so the correct mesh is bound per-entity.
- Consider automatic LOD generation using mesh simplification (e.g., meshoptimizer).

---

### 1.3  Global Illumination / Light Probes — **Medium**

**What is missing:** No baked or real-time GI. IBL (environment maps) is supported, but there are no per-location light probes, irradiance volumes, or screen-space GI.

**Why it matters:** Scenes lack realistic indirect lighting, making interiors and shadowed areas appear flat.

**Recommended approach:**
- Start with baked irradiance probes placed in the scene.
- Add a screen-space GI pass (SSGI / GTAO) as a cheaper alternative.

---

### 1.4  Decals — **Medium**

**What is missing:** No deferred or forward decal projection system.

**Why it matters:** Decals are widely used for bullet impacts, blood splatters, environmental storytelling, road markings, etc.

**Recommended approach:**
- Implement deferred decal rendering using an oriented bounding box and G-buffer compositing.

---

### 1.5  Volumetric Effects — **Medium**

**What is missing:** No volumetric fog, god-rays, or volumetric lighting. Standard fog (linear/exponential) exists but is not volumetric.

**Why it matters:** Volumetric lighting adds significant atmosphere to outdoor and interior scenes.

**Recommended approach:**
- Implement a froxel-based volumetric fog pass integrated with the existing shadow maps.

---

## 2  Gameplay Systems

### 2.1  Networking / Multiplayer — **High**

**What is missing:** No networking layer, no client-server architecture, no replication, no RPC mechanism.

**Why it matters:** Multiplayer is a core requirement for many game genres. Even single-player games often benefit from online features (leaderboards, cloud saves).

**Recommended approach:**
- Abstract a transport layer (UDP/TCP sockets, or integrate a library such as ENet, GameNetworkingSockets, or Yojimbo).
- Build a replication system for ECS component state.
- Add RPC and reliable-message support.

---

### 2.2  AI — Behaviour Trees / State Machines — **High**

**What is missing:** No AI framework. There are no behaviour trees, finite state machines, utility-AI scorers, or blackboard data stores.

**Why it matters:** Almost every game with NPCs needs some form of AI decision-making.

**Recommended approach:**
- Implement a data-driven behaviour-tree evaluator with standard node types (Selector, Sequence, Decorator, Leaf).
- Add a `Blackboard` component for per-entity AI knowledge.
- Optionally expose the tree to the editor as a visual node graph.

---

### 2.3  Navigation / Pathfinding — **High**

**What is missing:** No NavMesh generation, no A* or jump-point search, no navigation queries.

**Why it matters:** AI agents cannot move intelligently through a 3D world without a navigation system.

**Recommended approach:**
- Integrate Recast/Detour for NavMesh baking and runtime pathfinding.
- Add `NavMeshAgent` component with steering and avoidance.

---

### 2.4  Input Action Mapping — **High**

**What is missing:** The engine provides raw key/mouse polling (`Input::IsKeyPressed`, `Input::IsMouseButtonPressed`) but no high-level action-mapping layer that maps physical inputs to game actions, supports rebinding, or handles gamepad input generically.

**Why it matters:** Games need an abstraction layer so players can rebind controls and so gameplay code is decoupled from specific keys.

**Recommended approach:**
- Introduce `InputAction` and `InputContext` concepts.
- Support keyboard, mouse, and gamepad with dead-zone / curve configuration.
- Allow runtime rebinding with serialization of user preferences.

---

### 2.5  Save / Load System — **High**

**What is missing:** Scene serialization exists for the editor (YAML), but there is no game-state save/load system that captures runtime state (player progress, world modifications, inventories).

**Why it matters:** Players expect to save and resume their game. A robust save system is essential for any non-arcade game.

**Recommended approach:**
- Define a `SaveData` structure that captures relevant ECS component state.
- Support named save slots with metadata (timestamp, screenshot thumbnail).
- Handle versioning so older saves can be migrated.

---

### 2.6  Scene / Level Streaming — **High**

**What is missing:** The terrain system streams terrain chunks, but there is no general-purpose system for loading / unloading scenes or sub-levels at runtime based on proximity or triggers.

**Why it matters:** Open-world or large-level games cannot load everything at once. Streaming is required to keep memory usage manageable.

**Recommended approach:**
- Support additive scene loading: load a sub-scene into an existing world at a given transform offset.
- Add streaming volumes or distance-based triggers.
- Integrate with the async asset system that already exists.

---

## 3  Editor & Tooling Gaps

### 3.1  Material / Shader Graph Editor — **Medium**

**What is missing:** Materials are code-defined. There is no visual node-based material editor.

**Why it matters:** Artists need to iterate on materials without writing shader code.

**Recommended approach:**
- Build a node-graph UI in ImGui (using a library like imnodes).
- Generate GLSL from the graph at save time.

---

### 3.2  Animation State-Machine Editor — **Medium**

**What is missing:** The animation system supports clips and skeletal playback, but there is no visual state-machine or blend-tree editor for defining transitions.

**Why it matters:** Complex characters require dozens of animation states with conditional transitions. Editing these in code is error-prone.

**Recommended approach:**
- Introduce an `AnimationStateMachine` asset type.
- Build a node-graph editor for states and transitions, with condition scripting.

---

### 3.3  Prefab Overrides & Nesting — **Medium**

**What is missing:** `Prefab` exists, but there is no visible support for nested prefabs or per-instance property overrides.

**Why it matters:** Level designers need to place prefab instances and override individual properties (e.g., colour, scale) without breaking the prefab link.

**Recommended approach:**
- Track per-instance overrides in the scene file.
- Support nested prefab references that inherit changes from parent prefabs.

---

## 4  Platform & Miscellaneous

### 4.1  Cross-Platform Support — **Medium**

**What is missing:** The engine targets Windows only. There is no platform abstraction for Linux, macOS, or consoles.

**Why it matters:** Expanding platform reach multiplies the potential audience.

**Recommended approach:**
- Abstract platform-specific code (window creation, file I/O, input) behind interfaces.
- Add CMake toolchain files for Linux and macOS.

---

### 4.2  Localization / i18n — **Medium**

**What is missing:** No string table, no locale selection, no text substitution for translated strings.

**Why it matters:** Any game shipping internationally needs translated UI and dialogue text.

**Recommended approach:**
- Introduce a `StringTable` asset mapping string keys to locale-specific values.
- Use the key-based lookup in all UI text components and dialogue.

---

### 4.3  Video / Cinematic Playback — **Low**

**What is missing:** No video decoding or playback. Cut-scenes or intro videos cannot be played.

**Why it matters:** Many games use pre-rendered or real-time cinematics.

**Recommended approach:**
- Integrate a lightweight video decoder (e.g., pl_mpeg for MPEG-1, or ffmpeg for broader format support).
- Render decoded frames to a texture for display on UI or in-world screens.

---

### 4.4  Dialogue System — **Low**

**What is missing:** No conversation / dialogue-tree framework.

**Why it matters:** RPGs, adventure games, and narrative-driven games need branching dialogue with conditions and consequences.

**Recommended approach:**
- Implement a dialogue-tree data structure with nodes, choices, and condition callbacks.
- Add a runtime UI widget for displaying dialogue.

---

## Existing Strengths

For context, the following systems are already well-implemented:

| System | Status |
|--------|--------|
| ECS (EnTT) | ✅ Complete |
| PBR Rendering + IBL | ✅ Complete |
| Shadow Mapping (CSM, spot, point) | ✅ Complete |
| Post-Processing (Bloom, DOF, FXAA, SSAO, Motion Blur, Color Grading) | ✅ Complete |
| Fog (linear, exponential, height) | ✅ Complete |
| Skeletal Animation | ✅ Complete |
| 3D Physics (Jolt) | ✅ Complete |
| 2D Physics (Box2D) | ✅ Complete |
| GPU & CPU Particle Systems | ✅ Complete |
| Audio (miniaudio, 3D spatial) | ✅ Complete |
| Asset System (async, hot-reload, packing) | ✅ Complete |
| Dual Scripting (C# + Lua) | ✅ Complete |
| UI System (14 widget types) | ✅ Complete |
| Terrain (chunked, quadtree, foliage) | ✅ Complete |
| Memory Management (custom allocators) | ✅ Complete |
| Task System (parallel-for, scheduling) | ✅ Complete |
| Profiling & Debug Tools (Tracy, frame capture) | ✅ Complete |
| Snow & Wind Environmental Effects | ✅ Complete |

---

*Generated from a codebase review of OloEngine as of 2026-03-02.*
