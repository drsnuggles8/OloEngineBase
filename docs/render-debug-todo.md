# OloEngine Renderer Debugger - Feature Enhancement TODO

## Overview
This document outlines the planned enhancements to the OloEngine renderer debugging system. The goal is to bring our debugging capabilities closer to industry-standard tools like RenderDoc, Tracy, and NSight Graphics.

## Current Status
✅ **Completed Features:**
- RendererProfiler (frame timing, draw call tracking, bottleneck analysis)
- RendererMemoryTracker (allocation tracking, leak detection)
- CommandPacketDebugger (command visualization, draw key analysis)
- RenderGraphDebugger (graph visualization, DOT export)
- **GPU Resource Inspector** (texture preview, buffer inspection, memory tracking, resource states)
- **Shader Debugger** (source viewing, compilation tracking, uniform inspection, SPIR-V analysis)

---

## 🔴 HIGH PRIORITY

### ✅ 1. GPU Resource Inspector (COMPLETED)
**Status:** ✅ **COMPLETED**  
**Implementation:** Fully functional GPU resource debugging tool

**Completed Features:**
- ✅ Texture inspection (format, dimensions, mip levels, usage)
- ✅ Buffer content visualization (vertex/index/uniform buffers)
- ✅ Real-time texture preview with format detection and filtering
- ✅ Resource binding state monitoring
- ✅ Memory usage tracking with detailed statistics
- ✅ Framebuffer inspection with attachments
- ✅ Multiple texture formats support (RGB, RGBA, depth, etc.)
- ✅ Resizable UI with splitter for better UX
- ✅ Resource registration macros for easy integration

---

### ✅ 2. Shader Debugger (COMPLETED)
**Status:** ✅ **COMPLETED**  
**Implementation:** Advanced shader debugging and analysis tool

**Completed Features:**
- ✅ Shader source code viewing (original GLSL, generated GLSL, SPIR-V binary)
- ✅ Uniform and sampler inspection with usage tracking
- ✅ Compilation error/warning display with detailed messages
- ✅ Shader hot-reload tracking with history
- ✅ Performance metrics (compilation time, instruction count, memory usage)
- ✅ SPIR-V analysis and reflection data
- ✅ Real-time shader binding state monitoring
- ✅ Integration with existing shader system and renderer
- ✅ Menu-accessible debug UI in OloEditor
- ✅ Comprehensive shader statistics and profiling

---

### 3. GPU Timeline Profiler
**Priority:** High  
**Estimated Effort:** 5-6 days  
**Dependencies:** OpenGL timer queries, existing profiler integration

**Description:**
Industry-standard GPU profiling with detailed timeline visualization, similar to Tracy's GPU profiler.

**Key Features:**
- Hierarchical GPU event timing using OpenGL timer queries
- Visual timeline with zoom/pan capabilities
- GPU/CPU synchronization point visualization
- Per-pass timing breakdown
- Frame-to-frame comparison
- Export timeline data for external analysis

**Implementation Notes:**
- Use GL_TIME_ELAPSED queries for accurate GPU timing?
- Implement query pool management to avoid GPU stalls
- Add hierarchical event tracking with push/pop API
- Integrate with existing RendererProfiler data
- Keep in mind that we already have a general Instrumentation tool implemented, more for CPU though

---

## 🟡 MEDIUM PRIORITY

### 4. OpenGL State Change Tracker
**Priority:** Medium  
**Estimated Effort:** 2-3 days  
**Dependencies:** OpenGL debug output, state caching system

**Description:**
Tracks and analyzes OpenGL state changes to identify redundant calls and optimization opportunities.

**Key Features:**
- Real-time state change monitoring
- Redundant state change detection
- State change cost analysis
- Optimization suggestions
- State change heatmap per frame
- Integration with existing command packet system

**Implementation Notes:**
- Hook into OpenGL debug callback for state tracking
- Maintain shadow state for comparison
- Add metrics for state change frequency and cost
- Provide actionable optimization recommendations

---

### 5. Overdraw Visualization
**Priority:** Medium  
**Estimated Effort:** 3-4 days  
**Dependencies:** Framebuffer analysis, compute shaders

**Description:**
Visualizes pixel overdraw to help optimize rendering performance and identify inefficient draw order.

**Key Features:**
- Real-time overdraw heatmap overlay
- Per-object overdraw contribution analysis
- Depth complexity visualization
- Overdraw statistics (average, max, hotspots)
- Integration with existing render passes

**Implementation Notes:**
- Use compute shaders for overdraw analysis
- Implement depth complexity counting
- Add color-coded heatmap visualization
- Provide per-draw-call overdraw metrics

---

### 6. Enhanced Texture/Buffer Preview
**Priority:** Medium  
**Estimated Effort:** 2-3 days  
**Dependencies:** ResourceInspector base implementation

**Description:**
Advanced preview capabilities for textures and buffers with multiple visualization modes.

**Key Features:**
- Multi-channel texture viewing (R/G/B/A isolation)
- HDR tone mapping for preview
- Buffer data hex/structured viewing
- Zoom/pan controls for texture inspection
- Mip level and array slice navigation
- Export capabilities for external analysis

**Implementation Notes:**
- Extend ResourceInspector with advanced preview modes
- Add proper HDR handling and tone mapping
- Implement efficient texture streaming for large textures
- Support structured buffer interpretation

---

## 🟢 LOW PRIORITY

### 7. Frame Capture & Replay System
**Priority:** Low  
**Estimated Effort:** 8-10 days  
**Dependencies:** Command recording system, serialization

**Description:**
RenderDoc-style frame capture and replay for detailed frame analysis.

**Key Features:**
- Complete frame state capture
- Frame replay with step-through debugging
- Save/load capture files
- Replay with modified parameters
- Integration with all debugging tools

**Implementation Notes:**
- Complex feature requiring command stream recording
- Need robust serialization for all render state
- Consider implementing as separate tool/mode
- High memory requirements for capture storage

---

### 8. Advanced Visualizations
**Priority:** Low  
**Estimated Effort:** 4-5 days  
**Dependencies:** Shader system, framebuffer access

**Description:**
Collection of advanced debugging visualizations for various rendering aspects.

**Key Features:**
- Depth buffer visualization with near/far plane adjustment
- Normal vector visualization
- Wireframe overlay modes
- Texture usage heatmaps
- Light volume visualization
- Custom debug shader injection

**Implementation Notes:**
- Implement as collection of debug passes
- Add toggle system for different visualization modes
- Use compute shaders where appropriate for analysis
- Integrate with existing render pipeline

---

## Technical Considerations

### OpenGL 4.5+ Features to Leverage:
- **Timer Queries:** For accurate GPU profiling
- **Debug Output:** For state change tracking
- **Compute Shaders:** For analysis passes
- **Texture Views:** For resource inspection
- **Direct State Access:** For efficient state queries

### Performance Considerations:
- Minimize GPU stalls during resource inspection
- Use async texture downloads for preview
- Implement query pool management for timing
- Cache inspection results where possible
- Provide debug-only compilation flags

### Integration Points:
- Extend existing ImGui debug panels
- Integrate with current profiler data
- Hook into shader compilation system
- Connect with command packet system
- Maintain singleton pattern consistency
- Should probably follow the macro system we implemented in our Instrumentor, if possible

---

## Success Metrics
- **Functionality:** All high-priority features implemented and stable
- **Performance:** <5% overhead when debugging tools are active
- **Usability:** Intuitive ImGui interface matching existing tools
- **Integration:** Seamless integration with existing codebase
- **Documentation:** Clear usage documentation for each tool

---

*Last Updated: June 10, 2025*
*Estimated Total Effort: 3-4 weeks for high/medium priority