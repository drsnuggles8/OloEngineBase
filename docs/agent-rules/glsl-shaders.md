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

All forward-rendered geometry outputs **four render targets**:

```glsl
layout(location = 0) out vec4 o_Color;       // RGBA16F — final shaded color
layout(location = 1) out int  o_EntityID;    // R32I   — entity ID for editor picking
layout(location = 2) out vec2 o_ViewNormal;  // RG16F  — octahedral view-space normal (SSAO input)
layout(location = 3) out vec2 o_Velocity;    // RG16F  — screen-space motion vector (TAA / motion blur)
```

Omitting `o_EntityID` breaks editor selection. Omitting `o_ViewNormal` breaks SSAO.
Omitting `o_Velocity` ghosts moving objects under TAA and drops per-object motion blur.

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

## 4a. Motion vectors / velocity buffer (G-Buffer RT3 / forward attachment 3)

Per-object screen-space velocity is written by every opaque geometry shader
(`PBR_GBuffer*.glsl` deferred, `PBR_MultiLight*.glsl` forward) and consumed by
TAA (`PostProcess_TAA.glsl`) and motion blur (`PostProcess_MotionBlur.glsl`).

Convention — the velocity stored is a **UV-space delta**, current minus previous:

```glsl
vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;   // u_ViewProjection     * u_Model     * pos
vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;   // u_PrevViewProjection * u_PrevModel * pos
o_Velocity   = (ndcCurr - ndcPrev) * 0.5;            // = uvCurr - uvPrev  → consumers do prevUV = uv - velocity
```

Per-frame previous data comes from the renderer: `u_PrevModel` (per-entity
prev transform, `ModelMatrices` tail), `u_PrevViewProjection` (`MotionBlurMatrices`,
binding 8), and `u_PrevBoneTransforms` (`PrevBoneMatrices`, binding 31) for skinned
meshes. All alias their current value on the first frame, so velocity is zero for
newly-spawned geometry — never undefined.

Three gotchas for any **consumer** of this buffer:

- **It is geometry-only.** Background / sky pixels are never written, so they keep
  the buffer's clear value (zero). A consumer that wants *camera* motion on the
  background (e.g. motion blur streaking the sky as the camera turns) must
  depth-gate: where `depth == 1.0` (far plane), fall back to camera-only
  reconstruction (`InverseViewProjection` + `PrevViewProjection` from binding 8)
  instead of sampling the velocity buffer.
- **It carries TAA jitter.** Both `u_ViewProjection` and `u_PrevViewProjection`
  bake in their frame's Halton jitter (so TAA history reprojection stays
  self-consistent). Consumers inherit a sub-pixel (~1 px) jitter velocity on
  static geometry — harmless for motion blur, deliberately kept for TAA; don't
  "unjitter" it in one consumer without accounting for the other.
- **Gate optional velocity with a flag, don't assume it exists.** Forward and
  deferred both produce it today, but pass a `hasVelocity` flag (TAA's
  `TAAParams`, motion blur's `MotionBlurParams` at binding 42) so a path without
  a velocity buffer degrades to camera-only reconstruction rather than reading an
  unbound sampler.

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

## 6a. Instancing is shader-transparent — don't write a separate `*_Instanced` variant

Every mesh-rendering vertex/fragment stage — forward (`PBR_MultiLight*.glsl`) and
deferred (`PBR_GBuffer*.glsl`) alike — reads its per-draw model transform from the
`InstanceData[]` SSBO at `layout(std430, binding = 15)` via
`include/InstanceBlock_Vertex.glsl` (vertex stage, indexed by `gl_InstanceIndex`)
and `include/InstanceBlock.glsl` (fragment stage, indexed by the flat
`v_InstanceIndex` varying forwarded by `OLO_INSTANCE_FORWARD()`). A non-instanced
draw uploads a length-1 buffer and `gl_InstanceIndex` is always 0, so the exact
same shader binary handles both `glDrawElements` and
`glDrawElementsInstanced`/indirect-instanced draws with zero source changes.

Practical consequence (issue #515): when an instanced submission path picks the
wrong *existing* shader (e.g. always falling back to the forward default instead
of routing PBR materials to `PBRGBufferShader` on the Deferred path), the fix is
pure C++ shader-selection routing in `Renderer3DMeshSubmission.cpp` — mirror the
non-instanced `DrawMesh` routing logic exactly. Do **not** reach for a new
`PBR_GBuffer_Instanced.glsl` variant (the way `PBR_GBuffer_Skinned.glsl` exists
for skinning, which *does* need new vertex-stage bone-palette logic); the
existing `PBR_GBuffer.glsl` is already instancing-capable and adding a
duplicate variant would just be two copies of the same fragment logic to keep in
sync.

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

## 7a. Depth-prepass position invariance

The scene depth prepass binds minimal depth-only programs (`DepthPrepass*.glsl`)
in place of the four standard mesh programs (`PBR_MultiLight{,_Skinned}`,
`PBR_GBuffer{,_Skinned}`), then the color pass re-draws with
`glDepthFunc(GL_LEQUAL)` and depth writes off (see
`CommandDispatch::ResolveDepthPrepassShader`). Any rounding difference between
the two programs' `gl_Position` fails the depth test and punches pixel holes. So:

- The depth-prepass shaders replicate the standard programs' position math
  **expression-for-expression** (world position first, then view-projection;
  the same bone-accumulation loop for skinned).
- All of these vertex stages declare `invariant gl_Position;`.
- If you change position math in any standard mesh vertex stage, make the same
  change in the matching `DepthPrepass*.glsl` — and vice versa.
- A new mesh-drawing shader with different vertex math must NOT be added to the
  swap whitelist unless it gets its own matching depth-only variant; unswapped
  shaders safely run in full during the prepass.

MASK materials use `DepthPrepass_Mask{,Skinned}.glsl`, which must keep the exact
glTF alpha test from `PBR_MultiLight.glsl` (`baseColorFactor.a * albedo.a <
cutoff → discard`) so the prepass depth coverage matches the color pass.

## 8. Common mistakes

1. **Bare non-opaque uniform outside a block** → SPIR-V compile error. Wrap in a UBO block; only opaque types (samplers, images) may be standalone.
2. **Mismatched UBO binding** → silent data corruption. Cross-check `ShaderBindingLayout.h`.
3. **Missing MRT outputs** → broken entity picking or SSAO. Always write all three.
4. **`vec3` without padding** in UBOs → misaligned reads. Use `vec4` or add explicit padding.
5. **Binding-slot collision** → two resources fighting for the same slot. Check the tables above.
6. **Integer literal in float context** → SPIR-V error. Use `1.0` not `1`, `vec3(0.0)` not `vec3(0)`.
7. **Projecting `V` with a basis built from `V` itself is dead code — and so
   is measuring the projection *against* `V` again.** `GTAO.comp`'s per-slice
   horizon search (issue #533) had two layered instances of this:
   - `axisVS` was built as `cross(directionVS, viewNormal)`.
     `dot(viewNormal, cross(X, viewNormal)) == 0` is a pure vector-algebra
     identity for *any* `X` — crossing with the vector you're about to
     project is always perpendicular to it — so
     `projectedNormal = viewNormal - axisVS * dot(viewNormal, axisVS)`
     collapsed to `viewNormal` outright, and the derived tilt angle was
     exactly 0 for every slice on every pixel regardless of the surface's
     real orientation.
   - Fixing `axisVS` to use the camera's view vector instead (not the surface
     normal, not a value derived from it) stopped that collapse, but a
     *second*, quieter instance remained: `cosN` still divided
     `dot(projectedNormal, viewNormal)` by `|projectedNormal|`. For any
     orthogonal projection `P = V - axis·dot(V,axis)`, `dot(P, V) == |P|²` is
     also a plain linear-algebra identity — so that `cosN` always reduced
     back to `|projectedNormal|` again: non-degenerate this time (it varies
     per slice), but still not the intended "elevation relative to the slice
     plane's reference axis" quantity, so it silently washed out real
     occlusion the same way.
   Both are fixed by measuring consistently against the camera's per-pixel
   view vector (`viewVec = normalize(-pixCenterPos)`, not `viewNormal`, not a
   fixed screen axis) everywhere in the block: `orthoDirectionVS`, `axisVS`,
   and `cosN`'s dot target. General lesson: whenever a formula's basis,
   projection, *and* the dot product measuring the projection all reference
   the same vector `V`, stop and check which of those `dot(_, V)` terms are
   guaranteed by construction to reduce to something already known (0, or the
   projection's own length) — a bug at that point produces a plausible,
   non-obviously-wrong number, not a crash, and single-instance testing
   (checking only that the result is "nonzero" or "not exactly the old bug")
   can pass while a subtler version of the same mistake survives underneath.

---

## 9. Display-range vs HDR-linear post-process ordering

Some post-process kernels are written against the **[0,1] display range** and break
in unbounded HDR-linear space. The clearest example is **Contrast Adaptive
Sharpening** (`PostProcess_CAS.glsl`, `UpscalerRenderPass`): its contrast-headroom
term `min(mn, 2.0 - mx) / mx` and final `saturate()` both treat `1.0` as white. In
HDR-linear (pre-tonemap) the `2.0 - mx` term goes **negative** for any pixel brighter
than mid-grey, so amplitude clamps to 0 and *bright regions never sharpen* — the
effect silently disappears on exactly the highlights you most want crisp.

So CAS (and any sharpen / display-referred filter) runs **after `ToneMapPass`**, on
the LDR image (between `ToneMapPass` and `VignettePass` in the dynamic chain), not in
the HDR pre-tonemap band where MotionBlur/TAA/DOF live. When you insert a stage into
the post chain, every **downstream** consumer's `ReadFirstValidVersionedInputForPass`
candidate list must gain the new resource name *above* the stage it follows (CAS sits
above `ToneMapColor` in `VignettePass`/`FXAAPass`/`SelectionOutlinePass`/`UICompositePass`/`FinalPass`),
or the chain falls back past it and the stage's output is dropped. Placing CAS late
also means **fewer** candidate-list edits than the HDR band (5 consumers vs 11).

Future FSR1 EASU/RCAS *spatial upscale* (render below display res, then upscale) is
the opposite: EASU must run **early** (before display-res post), so when it lands it
splits — EASU pre-post, RCAS/CAS sharpen post-tonemap.
