# OloEngine Animated Mesh & ECS Renderer Integration - Feature Enhancement TODO

## Overview
This document outlines the planned enhancements to OloEngine’s renderer to support animated (skinned) meshes, with a modular, entity-driven architecture leveraging our existing entt-based ECS. The goal is to enable efficient, extensible rendering of animated characters (e.g., an animated dog) in Sandbox3D, while maintaining clean separation of concerns and high performance.

---

## 🔴 HIGH PRIORITY

### 1. ECS-Driven Animated Mesh Component System
**Priority:** High
**Dependencies:** entt ECS, asset pipeline

**Description:**  
Introduce new ECS components for animated mesh rendering and animation state.

**Key Steps:**
- [x] Define `AnimatedMeshComponent` (holds mesh, skeleton, skinning data)
- [x] Define `AnimationStateComponent` (current clip, time, blend info)
- [x] Define `SkeletonComponent` (bone hierarchy, transforms)
- [ ] Update asset pipeline to import and assign these components to entities *(TODO: See note in Renderer/Model.cpp)*

**Implementation Notes:**
- Use AssetRef<T> for asset objects and CreateScope<T> for non-asset objects
- Ensure components are cache-friendly (favor contiguous storage)
- Document component structure and usage

---


### 2. Animation System & State Machine
**Priority:** High 
**Dependencies:** Animation data, ECS components

**Description:**  
Implement a modular animation system that updates entity animation states and computes bone transforms each frame.

**Key Steps:**
- [x] Implement animation update system (per-entity, per-frame)
- [x] Support multiple animation clips and blending
- [x] Add animation state machine logic (idle, walk, run, etc.)
- [x] Store final bone matrices in a cache for rendering

**Implementation Notes:**
- Optimize for minimal dynamic allocation (pre-allocate bone matrix buffers)
- Use SIMD where possible for matrix operations
- Integrate with ImGui for debugging animation state

**Note:**
Basic automatic state machine logic is implemented: the test entity switches between Idle and Bounce every 2 seconds. This can be extended for more complex transitions.

---

### 3. Renderer Integration for Skinned Meshes
**Priority:** High
**Dependencies:** Command packet system, ECS, shaders

**Description:**  
Extend the renderer to support efficient submission and rendering of animated (skinned) meshes.

**Key Steps:**
- [x] Extend command packet system to support bone matrices and skinning data
- [x] Update GLSL shaders (SPIR-V) for GPU skinning (bone weights/indices)
- [x] Complete shader implementation with Point and Spot light support
- [x] Add SkinnedMesh::CreateCube() and CreateMultiBoneCube() primitives
- [x] Implement proper bind pose matrix support
- [x] Batch static and animated meshes efficiently
- [x] Iterate over ECS entities with `AnimatedMeshComponent` and submit draw commands

**Implementation Notes:**
- ✅ Minimize state changes and draw calls
- ✅ Use UBOs or SSBOs for bone matrices
- ✅ Profile for CPU-GPU sync and optimize command submission
- ✅ Added complete lighting model (Directional, Point, Spot lights)
- ✅ Proper bind pose calculations for realistic skeletal animation

---

## 🟡 MEDIUM PRIORITY

### 4. Asset Pipeline: Animated Model Import
**Priority:** Medium
**Dependencies:** glTF loader, material system

**Description:**  
Add support for importing skinned meshes and animation clips (preferably glTF).

**Key Steps:**
- [ ] Extend asset importer to handle skeletons, animations, and skinning data
- [ ] Ensure materials/textures are compatible with animated meshes
- [ ] Provide example animated dog asset for Sandbox3D

**Implementation Notes:**
- Validate bone hierarchy and animation data on import
- Support multiple animation clips per asset

---

### 5. Debugging & Visualization Tools
**Priority:** Medium
**Dependencies:** ImGui, renderer debug UI

**Description:**  
Enhance debugging tools for animated mesh rendering and animation state.

**Key Steps:**
- [ ] Add ImGui panels for animation state inspection and control
- [ ] Visualize bone transforms and skeleton hierarchy in real-time
- [ ] Provide playback controls for animation clips

**Implementation Notes:**
- Integrate with existing debug UI patterns
- Allow toggling of debug visualizations per-entity

---

## 🟢 LOW PRIORITY

### 6. Advanced Animation Features
**Priority:** Low
**Dependencies:** Animation system

**Description:**  
Implement advanced features for future extensibility.

**Key Steps:**
- [ ] Add support for animation events (e.g., footstep triggers)
- [ ] Implement root motion extraction
- [ ] Support for animation retargeting (optional)

**Implementation Notes:**
- Design for modularity and future extension
- Document advanced feature usage

---

## Technical Considerations

- Favor STL containers (e.g., std::vector) and pre-allocate where possible
- Use custom allocators for frequent small allocations (bone matrices, animation states)
- Ensure proper alignment for SIMD optimizations
- Minimize CPU-GPU sync and avoid unnecessary stalls
- All engine code should be wrapped in the `OloEngine` namespace
- Use the existing OLO_... macros for error handling
- Follow AssetRef<T> for assets and CreateScope<T> patterns for memory management

---

## Success Metrics

- **Functionality:** Animated dog entity rendered and animating in Sandbox3D
- **Performance:** Minimal overhead compared to static mesh rendering
- **Modularity:** Clean separation of animation, ECS, and rendering logic
- **Usability:** Intuitive debug UI for animation state and skeleton inspection
- **Integration:** Seamless with existing renderer and ECS
- **Documentation:** Clear usage and extension documentation

---

*Last Updated: June 19, 2025*