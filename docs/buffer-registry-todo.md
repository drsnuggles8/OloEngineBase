# Uniform Buffer Registry Implementation Plan for OloEngineBase

## Overview
Implement a uniform buffer registry system similar to Hazel's `DescriptorSetManager` but adapted for OpenGL. This will provide automatic shader resource discovery, centralized binding management, and a clean API for setting shader resources.

## Goals
- Automatic resource discovery from SPIR-V reflection
- Type-safe resource binding with compile-time checks
- Centralized resource management across the engine
- Clean high-level API for materials and scene rendering
- Enhanced debugging with resource binding visualization
- Performance optimization through batched binding updates
- Future Vulkan compatibility (similar structure to Hazel's system)

## Phase 1: Core Registry Structure (1-2 days)

### 1.1 Create Base Types
- `ShaderResourceType` enum (`UniformBuffer`, `StorageBuffer`, `Texture2D`, `TextureCube`, `Image2D`)
- `ShaderResourceBinding` struct (`Type`, `BindingPoint`, `Set`, `Name`, `Size`, `IsActive`)
- `ShaderResourceInput` struct (`Type`, `Resources` vector, constructors for each type)

### 1.2 Main Registry Class
`UniformBufferRegistry` class with:
- Resource name to binding info mapping
- Binding point to actual resources mapping
- Dirty bindings tracking
- Associated shader reference
- `SetResource` methods for each resource type
- `GetResource` template method
- `DiscoverResources`, `ApplyBindings`, `Validate` methods
- Debug support methods

## Phase 2: SPIR-V Integration (1 day)

### 2.1 Extend Existing Reflection
- Enhance existing `OpenGLShader::Reflect()` method
- Extract uniform buffer bindings from SPIR-V
- Extract texture and storage buffer bindings
- Store reflection data in registry format
- Map Vulkan descriptor sets to OpenGL organization

### 2.2 Registry Integration
- Add `UniformBufferRegistry` member to `OpenGLShader`
- Add `GetResourceRegistry()` accessor method
- Add `SetShaderResource()` convenience methods
- Integrate with existing shader compilation pipeline

## Phase 3: Command System Integration (2-3 days)

### 3.1 Extend CommandDispatch
- Add registry management per shader ID
- Create `SetShaderResource` methods for different resource types
- Add `ApplyResourceBindings` method
- Add `GetShaderRegistry` accessor
- Track registries for active shaders

### 3.2 Command Integration
- Create `SetShaderResourceCommand` render command
- Type-safe resource setting based on resource type
- Integration with existing command queue system
- Batch resource binding updates

## Phase 4: High-Level API (1-2 days)

### 4.1 Material System Integration
- Add `UniformBufferRegistry` to `Material` class
- Template `SetResource` method for clean API
- `ApplyToShader` method for resource binding
- Material dirty state tracking
- Integration with existing material system

### 4.2 Scene Renderer Integration
- Add global `UniformBufferRegistry` for scene-wide resources
- `SetGlobalResource` methods
- `ApplyGlobalResources` for shader combination
- Integration with existing scene rendering pipeline
- Support for per-frame global resources

## Phase 5: Debug Integration (1 day)

### 5.1 Extend ShaderDebugger
- Add resource binding info to `ShaderInfo` struct
- Track resource binding status per resource name
- `UpdateResourceBindings` method
- `RenderResourceBindings` ImGui visualization
- Resource binding validation reporting

### 5.2 Debug Visualization
- Resource binding status visualization
- Missing resource warnings
- Resource type mismatch detection
- Binding point conflict detection
- Performance metrics for resource updates

## Implementation Priority
1. **Phase 1** - Core structure and base types
2. **Phase 2** - SPIR-V integration with existing reflection
3. **Phase 3** - Command system integration
4. **Phase 4** - High-level API for materials and scene rendering
5. **Phase 5** - Debug integration and visualization

## Files to Create

### New Files
- `OloEngine/src/OloEngine/Renderer/UniformBufferRegistry.h`
- `OloEngine/src/OloEngine/Renderer/UniformBufferRegistry.cpp`

## Files to Modify

### Core Integration
- `Platform/OpenGL/OpenGLShader.h` - Add registry member
- `OpenGLShader.cpp` - Enhance reflection, add registry methods
- `OloEngine/Renderer/CommandDispatch.h` - Add registry management
- `OloEngine/Renderer/CommandDispatch.cpp` - Add resource commands

### High-Level API
- `OloEngine/Renderer/Material.h` - Add registry API
- `OloEngine/Renderer/Material.cpp` - Implement resource setting
- `OloEngine/Scene/SceneRenderer.h` - Add global resource management
- `OloEngine/Scene/SceneRenderer.cpp` - Implement global resource binding

### Debug Support
- `ShaderDebugger.h` - Add resource debugging
- `ShaderDebugger.cpp` - Implement resource visualization

## Expected Benefits

### Developer Experience
- **Simple API**: `material.SetResource("u_Texture", texture)`
- Automatic resource discovery from shaders
- Type safety with compile-time checks
- Centralized resource management

### Performance
- Batched binding updates
- Reduced OpenGL state changes
- Efficient resource tracking
- Optimized binding point management

### Debugging
- Visual resource binding status
- Missing resource detection
- Resource type validation
- Performance monitoring

### Future-Proofing
- Similar structure to Hazel's Vulkan system
- Easy migration path to Vulkan
- Consistent API across graphics APIs
- Scalable resource management

## Testing Strategy

### Unit Tests
- Resource type validation
- Binding point management
- SPIR-V reflection accuracy
- Resource lifecycle management

### Integration Tests
- Material system integration
- Scene renderer integration
- Command system integration
- Debug system integration

### Performance Tests
- Binding update performance
- Memory usage tracking
- Resource lookup performance
- Batch update efficiency