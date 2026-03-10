---
description: "Use when writing, editing, reviewing, or debugging GLSL shader code for OloEngine. Covers SPIR-V constraints, UBO layout rules, MRT output, texture/SSBO bindings, and include conventions."
applyTo: "**/*.glsl"
---

# OloEngine GLSL Shader Authoring Rules

## SPIR-V Compilation Constraints

The engine compiles all GLSL through **shaderc → SPIR-V**. This imposes stricter rules than plain OpenGL GLSL:

- **No bare uniforms.** Every `uniform` must live inside a named `uniform` block (`layout(std140, binding = N) uniform BlockName { ... };`). A standalone `uniform mat4 u_Foo;` will fail with: *"non-opaque uniforms outside a block: not allowed when using GLSL for Vulkan"*.
- **No `gl_FragColor`.** Use explicit `layout(location = N) out` declarations.
- **No default-block interface variables.** All varyings must use explicit `layout(location = N)`.
- **No implicit casts.** Use explicit constructors (`float(intVar)`, `vec3(1.0)` not `vec3(1)`).

## Shader File Structure

Each `.glsl` file contains multiple stages separated by `#type` markers:

```glsl
#type vertex
#version 460 core
// vertex stage...

#type fragment
#version 460 core
// fragment stage...
```

Supported stages: `vertex`, `fragment`, `tess_control` (or `tesscontrol`), `tess_evaluation` (or `tesseval`).

## UBO Blocks (std140)

All UBO blocks **must** use `layout(std140, binding = N)`. Block names and members follow established conventions:

| Binding | Block Name | Key Members |
|---------|-----------|-------------|
| 0 | `CameraMatrices` | `u_ViewProjection`, `u_View`, `u_Projection`, `u_CameraPosition` |
| 1 | `LightProperties` | Direction, color, intensity, shadow params |
| 2 | `MaterialProperties` | Albedo, roughness, metallic, emission |
| 3 | `ModelMatrices` | `u_Model`, `u_NormalMatrix`, `u_EntityID` |
| 4 | `AnimationMatrices` | Bone matrices array |
| 5 | `MultiLightData` | Light array for multi-light passes |
| 6 | `ShadowData` | CSM + spot/point shadow VP matrices |
| 9 | `SSAOParams` | Radius, bias, intensity, sample count |
| 10 | `TerrainData` | Height scale, layer tiling, tessellation |
| 11 | `BrushPreview` | Brush position, radius, color |
| 12 | `FoliageParams` | Wind influence, sway, tint |
| 13 | `SnowParams` | Snow accumulation settings |
| 14 | `SSSParams` | Subsurface scattering params |
| 15 | `WindData` | Global wind direction, speed, gusts |
| 16 | `SnowAccumulation` | Height/slope thresholds |
| 17 | `FogData` | Fog mode, density, color, scattering |
| 18 | `PrecipitationData` | Rain/snow particle settings |
| 19 | `PrecipitationScreen` | Screen-space precipitation |
| 20 | `FogVolumeData` | Volumetric fog boxes |
| 21 | `DecalData` | Decal projection transforms |
| 22 | `LightProbeData` | SH probe volume params |
| 23 | `WaterParams` | Wave params, colors, visual settings |

### std140 Padding Rules

- `vec3` occupies 16 bytes (same as `vec4`). Always pad or use `vec4`.
- Scalars after `vec3` start at the next 16-byte boundary — add explicit `float _paddingN;`.
- Arrays of scalars: each element rounds up to 16 bytes.
- Struct total size must be a multiple of 16 bytes.
- Match C++ struct layout in `ShaderBindingLayout.h` exactly.

### Naming Convention

- Block names: PascalCase (`ModelMatrices`, `CameraMatrices`)
- Members: `u_` prefix (`u_ViewProjection`, `u_Model`)
- Padding fields: `_paddingN` or `_padN`

## MRT Output (Forward Pass)

All forward-rendered geometry must output to **3 render targets**:

```glsl
layout(location = 0) out vec4 o_Color;       // RGBA16F — final shaded color
layout(location = 1) out int  o_EntityID;     // R32I   — entity ID for editor picking
layout(location = 2) out vec2 o_ViewNormal;   // RG16F  — octahedrally-encoded view-space normal (for SSAO)
```

Omitting `o_EntityID` breaks editor selection. Omitting `o_ViewNormal` breaks SSAO.

Use octahedral encoding for normals:
```glsl
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return n.xy * 0.5 + 0.5;
}
```

## Texture Bindings

Use explicit `layout(binding = N)` — never rely on `glUniform1i` for sampler assignment.

| Binding | Name | Type |
|---------|------|------|
| 0 | `u_DiffuseMap` | `sampler2D` |
| 1 | `u_SpecularMap` | `sampler2D` |
| 2 | `u_NormalMap` | `sampler2D` |
| 3 | `u_HeightMap` | `sampler2D` |
| 4 | `u_AmbientMap` | `sampler2D` |
| 5 | `u_EmissiveMap` | `sampler2D` |
| 6 | `u_RoughnessMap` | `sampler2D` |
| 7 | `u_MetallicMap` | `sampler2D` |
| 8 | `u_ShadowMap` | `sampler2DArrayShadow` |
| 9 | `u_EnvironmentMap` | `samplerCube` |
| 10–12 | `u_UserTexN` | user-defined |

Higher slots (13–31) are reserved for shadows, terrain layers, post-process, wind field, etc. Check `ShaderBindingLayout.h` for the full list before assigning.

## SSBO Bindings (std430)

```glsl
layout(std430, binding = N) buffer BufferName { ... };
```

Slots 0–8 are reserved (particles, foliage instances, light probes, snow deformers, indirect draw).

## Include System

The engine resolves `#include` directives with cycle detection:

```glsl
#include "include/PBRCommon.glsl"
#include "include/FogCommon.glsl"
```

Available includes in `assets/shaders/include/`:
`CameraCommon.glsl`, `PBRCommon.glsl`, `WaterCommon.glsl`, `FogCommon.glsl`, `FogVolumeCommon.glsl`, `SnowCommon.glsl`, `PrecipitationCommon.glsl`, `WindSampling.glsl`, `LightProbeSampling.glsl`, `SphericalHarmonics.glsl`

## Common Mistakes

1. **Bare `uniform` outside a block** → SPIR-V compile error. Wrap in a UBO block.
2. **Mismatched UBO binding** → silent data corruption. Cross-check `ShaderBindingLayout.h`.
3. **Missing MRT outputs** → broken entity picking or SSAO. Always write all 3 outputs.
4. **`vec3` without padding** in UBOs → misaligned reads. Use `vec4` or add explicit padding.
5. **Binding slot collision** → two resources fighting for the same slot. Check the tables above.
6. **Integer literal in float context** → SPIR-V error. Use `1.0` not `1`, `vec3(0.0)` not `vec3(0)`.
