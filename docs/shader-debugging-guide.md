# Shader Debugging Guide

## Overview
This guide contains lessons learned and best practices for debugging shader-related rendering issues in OloEngine.

## Key Debugging Strategies

### 1. Visual Debugging with Obvious Colors
**Strategy:** Use extremely obvious visual indicators to confirm shader execution.
```glsl
// Example: Bright white override to confirm shader is running
FragColor = vec4(1.0, 1.0, 1.0, 1.0); // BRIGHT WHITE for debugging
return;
```

**When to Use:**
- When you suspect shader changes aren't taking effect
- To confirm which shader variant is actually being used
- To verify shader compilation and loading

**Benefits:**
- Immediately visible - no ambiguity about results
- Works regardless of lighting/material complexity
- Helps distinguish between shader selection vs shader logic issues

### 2. Multi-Level Debug Logging
**Strategy:** Add debug logs at multiple pipeline levels to track data flow.

**Critical Points to Log:**
1. **Shader Selection Level** (Renderer3D.cpp):
   ```cpp
   OLO_CORE_INFO("Using shader: {} (ID: {})", shaderName, shaderID);
   ```

2. **Command Dispatch Level** (CommandDispatch.cpp):
   ```cpp
   OLO_CORE_INFO("DrawSkinnedMesh - Binding shader ID: {}", cmd->shader->GetRendererID());
   OLO_CORE_INFO("  Light Type: {}", std::to_underlying(light.Type));
   OLO_CORE_INFO("  Binding textures: Diffuse->slot {}, Specular->slot {}", 3, 4);
   ```

3. **GPU Data Level**:
   ```cpp
   OLO_CORE_INFO("  Material Ambient: ({:.2f}, {:.2f}, {:.2f})", 
                 ambient.x, ambient.y, ambient.z);
   ```

### 3. Shader Cache Awareness
**Critical Lesson:** Always consider shader caching when changes don't take effect.

**Common Symptoms:**
- Shader file modifications have no visual impact
- Debug code changes don't appear in rendering
- Fixes that should work still show original problem

**Solutions:**
1. **Manual Cache Clearing**: Delete shader cache files/directories
2. **Engine Restart**: Some engines cache shaders in memory
3. **Forced Recompilation**: Use engine-specific cache invalidation
4. **Timestamp Checking**: Verify shader file modification times

**Prevention:**
- Implement cache invalidation based on file modification time
- Add debug flags to force shader recompilation
- Use visual indicators (like version numbers) in shader output during development

### 4. Texture Binding Verification
**Strategy:** Match runtime binding with shader layout declarations exactly.

**Common Issue:**
```glsl
// Shader expects:
layout(binding = 3) uniform sampler2D u_DiffuseMap;
layout(binding = 4) uniform sampler2D u_SpecularMap;

// But code was doing:
diffuseMap->Bind(0);   // WRONG! Should be Bind(3)
specularMap->Bind(1);  // WRONG! Should be Bind(4)
```

**Debugging Steps:**
1. **Check Shader Layout Bindings**: Look for `layout(binding = N)` declarations
2. **Verify Runtime Binding**: Ensure `texture->Bind(N)` matches layout binding
3. **Remove Redundant SetInt Calls**: Layout bindings make `SetInt("u_TextureName", slot)` unnecessary
4. **Log Binding Operations**: Add debug output for texture binding operations

### 5. Systematic Issue Isolation
**Strategy:** Use a methodical approach to narrow down the problem domain.

**Debugging Hierarchy:**
1. **Is the right shader being selected?** → Shader selection logging
2. **Are shader changes taking effect?** → Visual debug overrides
3. **Is data reaching the shader correctly?** → UBO/uniform logging
4. **Are textures bound correctly?** → Texture binding verification
5. **Is the lighting calculation correct?** → Step-by-step color coding

**Color-Coded Debug Strategy:**
```glsl
// Different colors for different code paths/issues
if (lightType == DIRECTIONAL_LIGHT) {
    result = mix(actualResult, vec3(0.0, 1.0, 0.0), 0.1); // Green tint
} else if (materialInvalid) {
    return vec3(1.0, 0.0, 0.0); // Red for material issues
} else if (textureInvalid) {
    return vec3(1.0, 0.5, 0.0); // Orange for texture issues
}
```

## Common Pitfalls and Solutions

### Layout Binding Mismatches
**Problem:** Runtime texture binding doesn't match shader layout binding.
**Solution:** Always check shader reflection or manually verify layout bindings match runtime binding calls.

### Shader Caching During Development
**Problem:** Changes to shader files don't take effect immediately.
**Solution:** Implement reliable cache invalidation or manual cache clearing workflows.

### Insufficient Debug Granularity
**Problem:** Can't determine where in the pipeline the issue occurs.
**Solution:** Add debug output at every major pipeline stage with distinctive visual/log indicators.

### Assuming Enum/Constant Alignment
**Problem:** Enum values in C++ don't match shader constants.
**Solution:** Always verify enum-to-shader constant mapping explicitly.

## Best Practices

### 1. Debug Infrastructure
- **Always add comprehensive logging** for complex rendering paths
- **Use distinctive visual indicators** that are impossible to miss
- **Log both input and computed values** at critical pipeline stages

### 2. Shader Development
- **Use layout bindings consistently** across all shaders
- **Include version/debug information** in shader output during development
- **Implement fallback visual indicators** for error conditions

### 3. Issue Documentation
- **Document the exact symptoms** (colors, behaviors, log output)
- **Record the debugging process** and what each step revealed
- **Note the final root cause and solution** for future reference

### 4. Validation Workflow
- **Test shader changes immediately** with obvious visual indicators
- **Verify cache invalidation** when changes don't appear
- **Cross-reference multiple debugging levels** to confirm findings

## Tools and Techniques

### Visual Debugging Patterns
```glsl
// Bright colors for obvious confirmation
vec4(1.0, 1.0, 1.0, 1.0)  // Bright white - shader running
vec4(1.0, 0.0, 1.0, 1.0)  // Magenta - error/invalid state
vec4(1.0, 0.0, 0.0, 1.0)  // Red - material/data issues
vec4(1.0, 0.5, 0.0, 1.0)  // Orange - texture issues
```

### Logging Patterns
```cpp
// Structured debug output
OLO_CORE_INFO("=== DrawSkinnedMesh Debug ===");
OLO_CORE_INFO("  Shader: {} (ID: {})", shaderName, shaderID);
OLO_CORE_INFO("  Light Type: {} (Expected: {})", actualType, expectedType);
OLO_CORE_INFO("  Textures: Diffuse->slot {}, Specular->slot {}", diffuseSlot, specularSlot);
```

### Cache Management
```bash
# Common shader cache locations to check/clear
./cache/shaders/
./temp/compiled_shaders/
%APPDATA%/[EngineeName]/shader_cache/
```

## Conclusion

Effective shader debugging requires a systematic approach combining visual confirmation, comprehensive logging, and awareness of common pitfalls like caching issues. The key is to verify assumptions at every level of the rendering pipeline and use obvious visual indicators to confirm when fixes take effect.
