# GLSL Shader Authoring Rules

Applies to: `**/*.glsl`

The engine compiles all GLSL through **shaderc → SPIR-V**, which imposes stricter rules than plain OpenGL GLSL. Follow these or compilation fails.

---

## 1. SPIR-V constraints

- **No bare non-opaque uniforms.** Every non-opaque `uniform` (`float`, `vec*`, `mat*`, `int`, …) must live inside a named UBO block: `layout(std140, binding = N) uniform BlockName { ... };`. A standalone `uniform mat4 u_Foo;` fails with *"non-opaque uniforms outside a block: not allowed when using GLSL for Vulkan"*.
- **Opaque types may be standalone.** Samplers, images, textures, subpass inputs: `layout(binding = N) uniform sampler2D u_Tex;` is fine.
- **No `gl_FragColor`.** Use explicit `layout(location = N) out` declarations.
- **No default-block interface variables.** All varyings need explicit `layout(location = N)`.
- **No implicit casts.** Use explicit constructors: `float(intVar)`, `vec3(1.0)` not `vec3(1)`.

---

## 2. File structure

One `.glsl` file contains multiple stages separated by `#type` markers:

```glsl
#type vertex
#version 460 core
// vertex stage...

#type fragment
#version 460 core
// fragment stage...
```

Supported stages: `vertex`, `fragment`, `tess_control` (or `tesscontrol`), `tess_evaluation` (or `tesseval`).

---

## 3. UBO blocks (std140)

All UBO blocks use `layout(std140, binding = N)`. Block names and members follow established conventions — match `OloEngine/src/OloEngine/Renderer/ShaderBindingLayout.h` exactly:

| Binding | Block | Key members |
|---|---|---|
| 0 | `CameraMatrices` | `u_ViewProjection`, `u_View`, `u_Projection`, `u_CameraPosition` |
| 1 | `LightProperties` | direction, color, intensity, shadow params |
| 2 | `MaterialProperties` | albedo, roughness, metallic, emission |
| 3 | `ModelMatrices` | `u_Model`, `u_Normal`, `u_EntityID` |
| 4 | `AnimationMatrices` | bone matrices array |
| 5 | `MultiLightBuffer` | light array for multi-light passes |
| 6 | `ShadowData` | CSM + spot/point shadow VP matrices |
| 9 | `SSAOParams` | radius, bias, intensity, sample count |
| 10 | `TerrainData` | height scale, layer tiling, tessellation |
| 11 | `BrushPreview` | brush position, radius, color |
| 12 | `FoliageParams` | wind influence, sway, tint |
| 13 | `SnowParams` | snow accumulation settings |
| 14 | `SSSParams` | subsurface scattering params |
| 15 | `WindData` | global wind direction, speed, gusts |
| 16 | `SnowAccumulation` | height/slope thresholds |
| 17 | `FogData` | fog mode, density, color, scattering |
| 18 | `PrecipitationData` | rain/snow particle settings |
| 19 | `PrecipitationScreen` | screen-space precipitation |
| 20 | `FogVolumeData` | volumetric fog boxes |
| 21 | `DecalData` | decal projection transforms |
| 22 | `LightProbeData` | SH probe volume params |
| 23 | `WaterParams` | wave params, colors, visual settings |

### std140 padding rules

- `vec3` occupies 16 bytes (same as `vec4`). Always pad or use `vec4`.
- A scalar after a `vec3` starts at the next 16-byte boundary — add an explicit `float _paddingN;`.
- Arrays of scalars: each element rounds up to 16 bytes.
- Struct total size must be a multiple of 16 bytes.
- **C++ struct layout in `ShaderBindingLayout.h` must match exactly.**

### Naming

- Block names: PascalCase (`ModelMatrices`, `CameraMatrices`).
- Members: `u_` prefix (`u_ViewProjection`, `u_Model`).
- Padding fields: `_paddingN` or `_padN`.

---

## 4. MRT output (forward pass)

All forward-rendered geometry outputs **three render targets**:

```glsl
layout(location = 0) out vec4 o_Color;       // RGBA16F — final shaded color
layout(location = 1) out int  o_EntityID;    // R32I   — entity ID for editor picking
layout(location = 2) out vec2 o_ViewNormal;  // RG16F  — octahedral view-space normal (SSAO input)
```

Omitting `o_EntityID` breaks editor selection. Omitting `o_ViewNormal` breaks SSAO.

Octahedral encoding for normals:

```glsl
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return n.xy * 0.5 + 0.5;
}
```

---

## 5. Texture bindings

Use explicit `layout(binding = N)` — **never** `glUniform1i` for sampler assignment.

| Binding | Name | Type |
|---|---|---|
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

Slots 13–31 are reserved for shadows, terrain layers, post-process, wind field, etc. — consult `ShaderBindingLayout.h` before assigning.

---

## 6. SSBO bindings (std430)

```glsl
layout(std430, binding = N) buffer BufferName { ... };
```

Slots 0–8 are reserved (particles, foliage instances, light probes, snow deformers, indirect draw).

---

## 7. Include system

`#include` is resolved by the engine with cycle detection:

```glsl
#include "include/PBRCommon.glsl"
#include "include/FogCommon.glsl"
```

Available headers in `OloEditor/assets/shaders/include/`:
`CameraCommon.glsl`, `PBRCommon.glsl`, `WaterCommon.glsl`, `FogCommon.glsl`, `FogVolumeCommon.glsl`, `SnowCommon.glsl`, `PrecipitationCommon.glsl`, `WindSampling.glsl`, `LightProbeSampling.glsl`, `SphericalHarmonics.glsl`.

---

## 8. Common mistakes

1. **Bare non-opaque uniform outside a block** → SPIR-V compile error. Wrap in a UBO block; only opaque types (samplers, images) may be standalone.
2. **Mismatched UBO binding** → silent data corruption. Cross-check `ShaderBindingLayout.h`.
3. **Missing MRT outputs** → broken entity picking or SSAO. Always write all three.
4. **`vec3` without padding** in UBOs → misaligned reads. Use `vec4` or add explicit padding.
5. **Binding-slot collision** → two resources fighting for the same slot. Check the tables above.
6. **Integer literal in float context** → SPIR-V error. Use `1.0` not `1`, `vec3(0.0)` not `vec3(0)`.
