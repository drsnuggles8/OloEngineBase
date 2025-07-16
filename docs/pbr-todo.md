# OloEngine PBR (Physically Based Rendering) Implementation Plan

## Overview
This document outlines the implementation plan for adding PBR support to OloEngine's 3D rendering system. PBR is essential for modern graphics and required for proper glTF 2.0 material support. The implementation will follow a modular approach, integrating with the existing renderer architecture while maintaining performance and extensibility.

---

## 🔴 HIGH PRIORITY

### 1. PBR Shader Development
**Priority:** High
**Dependencies:** Shader system, SPIR-V compiler

**Description:**  
Create comprehensive PBR shaders supporting metallic-roughness workflow (glTF 2.0 standard).

**Key Steps:**
- [ ] Create base PBR vertex shader with proper vertex attributes
- [ ] Implement PBR fragment shader with BRDF calculations
- [ ] Support IBL (Image-Based Lighting) for environmental reflections
- [ ] Add support for all standard PBR texture maps
- [ ] Implement proper gamma correction and tone mapping

**Files to Create:**
- `OloEngine/assets/shaders/PBR.glsl` (main PBR shader)
- `OloEngine/assets/shaders/PBR_Skinned.glsl` (PBR with skeletal animation)
- `OloEngine/assets/shaders/includes/BRDF.glsl` (BRDF functions)
- `OloEngine/assets/shaders/includes/IBL.glsl` (IBL calculations)

**Implementation Notes:**
- Use Cook-Torrance BRDF model
- Support both punctual lights and IBL
- Ensure compatibility with existing lighting system
- Compile to SPIR-V for optimal performance

---

### 2. PBR Material System
**Priority:** High
**Dependencies:** Material class, texture system

**Description:**  
Extend the material system to support PBR properties and texture maps.

**Key Steps:**
- [ ] Extend Material class with PBR properties (metallic, roughness, AO)
- [ ] Add PBR texture map support (albedo, normal, metallic-roughness, AO, emissive)
- [ ] Implement material serialization for PBR properties
- [ ] Create material presets for common surfaces

**Files to Modify:**
- `OloEngine/src/OloEngine/Renderer/Material.h` (add PBR properties)
- `OloEngine/src/OloEngine/Renderer/Material.cpp` (implement PBR logic)

**Files to Create:**
- `OloEngine/src/OloEngine/Renderer/PBRMaterial.h` (specialized PBR material)
- `OloEngine/src/OloEngine/Renderer/PBRMaterial.cpp`

**Implementation Notes:**
- Support packed metallic-roughness textures (glTF standard)
- Implement proper default values for missing textures
- Ensure backward compatibility with existing materials

---

### 3. Environment Mapping & IBL System
**Priority:** High
**Dependencies:** Texture system, framebuffer

**Description:**  
Implement environment mapping and IBL for realistic reflections and ambient lighting.

**Key Steps:**
- [ ] Add cubemap texture support
- [ ] Implement HDR environment map loading
- [ ] Create IBL precomputation pipeline (irradiance, prefiltered env, BRDF LUT)
- [ ] Integrate IBL with PBR shaders
- [ ] Add skybox rendering support

**Files to Create:**
- `OloEngine/src/OloEngine/Renderer/EnvironmentMap.h`
- `OloEngine/src/OloEngine/Renderer/EnvironmentMap.cpp`
- `OloEngine/src/OloEngine/Renderer/IBLPrecompute.h`
- `OloEngine/src/OloEngine/Renderer/IBLPrecompute.cpp`
- `OloEngine/assets/shaders/Skybox.glsl`
- `OloEngine/assets/shaders/IBLPrefilter.glsl`
- `OloEngine/assets/shaders/IrradianceConvolution.glsl`

**Implementation Notes:**
- Use OpenGL 4.5+ texture views for efficiency
- Precompute IBL textures at startup or on-demand
- Support both HDR and LDR environment maps

---

## 🟡 MEDIUM PRIORITY

### 4. Renderer Pipeline Updates
**Priority:** Medium
**Dependencies:** Command buffer system, uniform buffers

**Description:**  
Update the rendering pipeline to efficiently handle PBR materials and lighting.

**Key Steps:**
- [ ] Extend RenderCommand3D to support PBR material parameters
- [ ] Update uniform buffer structures for PBR data
- [ ] Implement proper render state sorting for PBR materials
- [ ] Add multi-pass rendering support if needed

**Files to Modify:**
- `OloEngine/src/OloEngine/Renderer/Renderer3D.h`
- `OloEngine/src/OloEngine/Renderer/Renderer3D.cpp`
- `OloEngine/src/OloEngine/Renderer/RenderCommand.h`

**Implementation Notes:**
- Minimize state changes between PBR and non-PBR materials
- Use instancing for objects with same PBR material
- Profile and optimize uniform buffer updates

---

### 5. Texture Asset Pipeline Enhancement
**Priority:** Medium
**Dependencies:** Asset system, texture importer

**Description:**  
Enhance texture loading to support PBR workflow requirements.

**Key Steps:**
- [ ] Add HDR texture format support (e.g., .hdr, .exr)
- [ ] Implement texture channel packing/unpacking
- [ ] Add normal map format detection and conversion
- [ ] Support BC6H/BC7 compression for PBR textures

**Files to Modify:**
- `OloEngine/src/OloEngine/Renderer/Texture.h`
- `OloEngine/src/OloEngine/Renderer/Texture.cpp`

**Implementation Notes:**
- Ensure proper sRGB/linear color space handling
- Implement texture streaming for large PBR texture sets
- Add texture validation for PBR requirements

---

### 6. Debug Visualization Tools
**Priority:** Medium
**Dependencies:** ImGui, debug renderer

**Description:**  
Add debugging tools for PBR material authoring and visualization.

**Key Steps:**
- [ ] Create ImGui PBR material editor
- [ ] Add debug visualization modes (albedo, normal, metallic, roughness, AO)
- [ ] Implement real-time IBL probe visualization
- [ ] Add BRDF preview sphere/ball

**Files to Create:**
- `OloEngine/src/OloEngine/ImGui/PBRDebugPanel.h`
- `OloEngine/src/OloEngine/ImGui/PBRDebugPanel.cpp`

**Implementation Notes:**
- Allow real-time material property tweaking
- Provide texture channel isolation views
- Show performance metrics for PBR rendering

---

## 🟢 LOW PRIORITY

### 7. Advanced PBR Features
**Priority:** Low
**Dependencies:** Basic PBR implementation

**Description:**  
Implement advanced PBR features for enhanced quality.

**Key Steps:**
- [ ] Add clearcoat support (for car paint, etc.)
- [ ] Implement subsurface scattering approximation
- [ ] Add anisotropic material support
- [ ] Implement sheen for fabric materials

**Implementation Notes:**
- Make features optional via shader variants
- Ensure minimal performance impact when unused
- Follow glTF extensions where applicable

---

### 8. Performance Optimizations
**Priority:** Low
**Dependencies:** Complete PBR implementation

**Description:**  
Optimize PBR rendering for maximum performance.

**Key Steps:**
- [ ] Implement clustered forward rendering
- [ ] Add LOD system for PBR materials
- [ ] Optimize shader variants and uber-shader approach
- [ ] Implement temporal caching for IBL

**Implementation Notes:**
- Profile on various hardware configurations
- Consider mobile/low-end GPU optimizations
- Maintain quality vs performance options

---

## Technical Considerations

### Shader Architecture
- Use shader includes for shared PBR functions
- Implement proper shader variant system
- Support both forward and deferred rendering paths
- Use DSA for all texture operations

### Material System
- Design for extensibility (custom material properties)
- Support material inheritance/templates
- Implement efficient material batching
- Cache material state to minimize API calls

### Integration Points
- Ensure compatibility with existing Model/Mesh classes
- Integrate with animation system (PBR + skinning)
- Support both static and dynamic lighting
- Maintain backward compatibility with existing shaders

### Performance Targets
- < 2ms additional overhead for PBR vs basic shading
- Support 100+ unique PBR materials per frame
- Efficient IBL with minimal texture bandwidth
- Scalable quality settings (low/medium/high)

---

## Success Metrics

- **Visual Quality:** Achieve industry-standard PBR rendering quality
- **glTF Support:** Full compliance with glTF 2.0 PBR materials
- **Performance:** Maintain 60+ FPS with complex PBR scenes
- **Usability:** Intuitive material authoring workflow
- **Compatibility:** Seamless integration with existing renderer
- **Extensibility:** Easy to add new PBR features

---

## Implementation Order

1. **Phase 1:** Basic PBR shader and material system
2. **Phase 2:** IBL and environment mapping
3. **Phase 3:** Renderer pipeline integration
4. **Phase 4:** Debug tools and visualization
5. **Phase 5:** Advanced features and optimizations

---

*Created: July 16, 2025*