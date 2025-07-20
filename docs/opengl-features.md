# OpenGL Advanced Features TODO

This document tracks the implementation of advanced OpenGL 4.3+ features and extensions for OloEngine.

---

## Core Features

- [ ] **Compute Shaders**
  - General-purpose GPU computation for culling, particles, post-processing, etc.

- [ ] **Shader Storage Buffer Objects (SSBOs)**
  - Large, random-access buffers for sharing data between CPU and all shader stages.

- [ ] **Atomic Counters**
  - Atomic operations in shaders for GPU-side counters, OIT, etc.

- [ ] **Image Load/Store**
  - Direct read/write access to texture memory in shaders for advanced effects.

- [ ] **Multi-Draw Indirect**
  - Submit many draw calls with a single API call to reduce CPU overhead.

- [ ] **Debug Output / KHR_debug**
  - Real-time debug messages and profiling from the OpenGL driver.

---

## Notable Extensions

- [ ] **GL_ARB_bindless_texture**
  - Bindless textures for thousands of textures per draw.

- [ ] **GL_ARB_shader_draw_parameters**
  - Access to `gl_DrawID` and `gl_BaseInstance` in shaders for efficient instancing.

- [ ] **GL_ARB_shader_viewport_layer_array**
  - Output to multiple viewports/layers for VR, shadow mapping, etc.

- [ ] **GL_ARB_sparse_texture**
  - Virtualized texture memory for very large textures.

- [ ] **GL_ARB_clip_control**
  - Vulkan/DX-style clip space control.

- [ ] **GL_ARB_gpu_shader_int64**
  - 64-bit integer support in shaders.

- [ ] **GL_ARB_shader_ballot / subgroup operations**
  - Wave-level programming and advanced compute/fragment operations.

---

## Notes

- Prioritize features based on project needs and hardware support.
- Reference the OpenGL and extension specifications for implementation details.
- Consider adding automated tests and sample scenes for each feature.
