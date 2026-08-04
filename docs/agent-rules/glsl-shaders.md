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

### 5a. The heap-bindless alternative — opt-in, and `SSAO.glsl` is the worked example

Issue #691 Phase 3 added a second way to reach a texture. It is **off by
default** (`OLO_RHI_BINDLESS=1` enables it) and only shaders that opt in take
it, so the rule for an unrelated shader is still: keep writing
`layout(binding = N)`.

To convert one, wrap the declarations and leave the body alone:

```glsl
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_DepthTexture OLO_HEAP_TEX_2D(19)   // TEX_POSTPROCESS_DEPTH
#define u_NoiseTexture OLO_HEAP_TEX_2D(21)   // TEX_SSAO_NOISE
#else
layout(binding = 19) uniform sampler2D u_DepthTexture;
layout(binding = 21) uniform sampler2D u_NoiseTexture;
#endif

// …every `texture(u_DepthTexture, uv)` below is UNCHANGED.
```

and on the C++ side swap `context.BindTexture(SLOT, tex)` for
`context.BindTextureOrHeapOffset(SLOT, tex, lifetime[, sampler])`, then call
`context.FlushHeapOffsets()` once before the draw.

**The `TEX_*` number is the whole trick.** `OLO_HEAP_TEX_2D` takes the *slot
constant*, not an offset, and looks it up in the shared offset table the pass
just wrote. So both variants name the same constant and cannot disagree about
which texture is which — and the body stays byte-identical, which is what makes
a conversion reviewable. What disappears is the bind call, not the number.

**Pick the lifetime deliberately** — it is the one judgement per site.
`FrameTransient` for anything graph-owned (attachments, pooled targets), which
retires at the frame boundary so a held offset reports stale; `Persistent` for
pass- or asset-owned textures, whose offsets are memoised and stable.

**The constraint that shapes all of this.** A shader whose bindless branch is
active cannot travel the normal compile path at all. §1's pipeline runs GLSL
through `shaderc` targeting **Vulkan** first, and `GL_ARB_bindless_texture` is a
GLSL-only extension predating SPIR-V with no representation in that environment
— so it is rejected at the first hop, not degraded. The engine therefore
compiles the bindless variant through `glShaderSource` on the original GLSL
(`OpenGLShader::CreateProgramFromRawGLSL`), bypassing SPIR-V and SPIRV-Cross,
and with them the SPIR-V cache tiers and the reflection the binding-validation
tests read. The linked-program (`.pgr`) cache still works, under a
variant-keyed filename.

`BindlessShaderPipelineTest` pins this and fails if it ever changes — including
if it changes to *yes*, because that would delete a whole compile route.

Practical rules:

- `include/BindlessHeap.glsl` is **inert without `OLO_BINDLESS`**, so including
  it costs a shader nothing on the default path, and `ShaderCompilationTest`
  keeps compiling that path through the Vulkan target as usual.
- Do **not** write `#extension GL_ARB_bindless_texture` in a `.glsl` yourself —
  not even inside the `#ifdef`. The engine injects it right after `#version`,
  because GLSL requires `#extension` to precede every non-preprocessor token and
  that would otherwise dictate where your `#include` may sit.
- The five SPIR-V-reading shader tests still cover a converted file's **shared**
  declarations via the default variant; only the bindless branch is unvalidated
  by them, which is why the GPU test drives the real seam instead.
- **One source file CAN carry both variants** behind `#ifdef OLO_BINDLESS`.
  Verified: with the define absent the `#extension` line is preprocessed away
  before glslang sees it, so the slot-based variant compiles through the normal
  Vulkan-SPIR-V path from the very same file. What cannot be shared is the
  *compiled artefact* — two programs, two caches, two compilers — not the
  source. (An earlier version of this section said "a second shader, not a
  `#ifdef`", which overstated the cost: it is one file, two builds.)
- A useful consequence of the `#ifdef` form: the SPIR-V-reading tests
  (`ShaderReflectionBindingTest`, `ShaderUBOSizeConsistencyTest`,
  `ShaderStageInterfaceTest`, `ShaderStageContractTest`,
  `ShaderCrossConsistencyTest`) still cover the file's shared parts — UBO
  layouts, stage interfaces, the math — through the default variant. Only the
  bindless branch's own declarations go unvalidated.

### 5b. Storage images through the heap — three things differ, and each is forced

`imageLoad`/`imageStore` bindings go through the same heap, but they are a
**different descriptor kind** (issue #691 Phase 3 bucket 3, ADR 0011 amendments
(26)–(29)), and the recipe is not the sampler one with a different macro name.

```glsl
#include "include/BindlessHeap.glsl"

#ifndef OLO_BINDLESS
layout(r32f, binding = 0) uniform writeonly image2D u_Output;
#endif

void main()
{
#ifdef OLO_BINDLESS
    OLO_HEAP_IMAGE(r32f, writeonly, image2D, u_Output, 0);
#endif
    imageStore(u_Output, coord, value);   // <- byte-identical either way
}
```

and on the C++ side `RenderCommand::BindImageTexture(unit, tex, mip, layered,
layer, access, format)` becomes `HeapBinding::BindImageOrOffset(...)` with the
same arguments plus a lifetime, followed by `HeapBinding::FlushOffsets()`.

**1. The macro DECLARES; it is not an expression.** A sampler macro can be
`#define u_X OLO_HEAP_TEX_2D(19)` because a sampler constructor is an expression.
An image carries a format layout qualifier, which belongs to a *declaration*, and
an image initialised from a buffer read is not a constant expression — so it
cannot live at file scope. The declaration therefore moves **into the function
that uses the image**. In `HZB.comp` that is `WriteMip()`, not `main()`; a local
in `main()` would not be visible to the helper. The body still does not change,
which is the property that keeps a conversion reviewable.

**2. The memory qualifier travels too — but `readonly` is NOT passable.**
`writeonly` and `coherent` change what the compiler may assume, so they are the
macro's second argument. Pass `OLO_HEAP_IMAGE_RW` when the bindful declaration
had none — an empty macro argument is legal but not uniformly implemented across
GLSL preprocessors.

`readonly` is the exception and it is a hard one: the macro **declares and
initialises a local**, and initialising a `readonly` variable is a write, so the
compiler rejects it (`error C7504`). A read-only storage image therefore **stays
on the slot path** — keep the plain `layout(binding = N) readonly uniform
image2D` and convert the rest of the shader around it. Four compute shaders
silently fell back to the slot-based program before this was diagnosed; the
bindless route declines quietly by design, so the only symptom was the absence of
the "bindless route" log line.

**3. The number is an IMAGE UNIT, not a `TEX_*` slot.** GL image units and
texture units are separate namespaces that both start at zero, so the offset
table reserves a disjoint region and the macro rebases by
`ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE`. Both sides apply the base from that
one constant, so amendment (25)'s "the two variants cannot disagree" property
holds. Without the rebase, image unit 0 and `TEX_DIFFUSE` collide and each
renders the other's resource.

Two more rules that already cost debugging time:

- **Bind the shader BEFORE the image.** The seam asks
  `Shader::IsBoundProgramBindless()` to decide between writing an offset and
  issuing a bind, and that flag describes the program *in flight*. An image bound
  first silently takes the fallback path even with the heap on. Two call sites
  (`TerrainErosion`, `VirtualGeometryPass`'s colorize) had to be reordered.
- **A ping-pong needs a flush per iteration.** `FluidSmooth` and `GTAO_Denoise`
  swap source and destination every pass, so a flush hoisted out of the loop
  publishes only the final pair.

**Check that the name is not ALSO declared in an include before converting it.**
`#define u_X OLO_HEAP_TEX_2D(N)` rewrites *every* occurrence of that token in the
translation unit — including the `layout(binding = N) uniform sampler2D u_X;` in
a shared header, which becomes syntactic garbage. `DDGI_Relight.glsl` hit this:
it declares `u_ShadowMapCSMRaw` / `u_ShadowAtlasRaw` itself *and* includes
`DeferredLightingShared.glsl`, which declares them too under a different guard.
The bindless build failed and fell back **silently** — the only visible trace was
a `.bindless.failed.glsl` dump, which is why
`AssetContentValidity.AllCacheFilesMatchKnownPattern` catching a stray cache file
is load-bearing rather than housekeeping.

So the check before converting a declaration is
`grep -rn "<name>" assets/shaders/include/`. If a header owns it too, leave that
input on the slot path and convert the rest — §5a's "a shader can be moved input
by input" is what makes this cheap, and `PostProcess_Fog.glsl` is the worked
example (converted for depth + froxel, still binding `TEX_SHADOW` conventionally).

**Do NOT convert a G-Buffer shader's images.** The bindless route produces no
SPIR-V and therefore never runs `Reflect()`, so `m_IsDeferredCapable` stays false
and the shader is misrouted into the forward-overlay fallback.
`CreateProgramFromRawGLSL` detects and errors on this; `VirtualMeshGBuffer.glsl`
is deliberately left on the slot path for that reason.

The exact constructor spelling the macro uses is pinned against a live driver by
`BindlessHeapGpuTest.TheImageConstructorSpellingTheHeaderUsesIsAcceptedByTheDriver`,
which probes the alternatives and reports which ones work — so if it ever has to
change, the test says what to change it to.

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

## 10. Porting D3D/HLSL reference code: audit every screen-space Y convention

XeGTAO's reference `NDCToViewMul/Add` constants negate the Y pair
(`-2/proj11`, `+1/proj11`) because **D3D texture v = 0 is the TOP row**. This
engine's compute passes consume GL-convention inputs — a compute shader's
`pixCoord` row 0 addresses the framebuffer **bottom** (the HZB is a 1:1
`texelFetch` copy of the scene depth; the view-normals texture is fetched
with the same coordinates) — so porting the D3D constants verbatim **negated
view-space Y for every position reconstructed from depth** while the decoded
surface normals stayed correct. Every horizon angle reflected about the
horizontal plane. The failure was invisible at the poses used to verify the
pass (looking straight down, the scene is symmetric about the view axis and
the reflection cancels) and catastrophic at grazing views: a full-frame
visibility collapse to the 0.03 floor over the sea/quay.

That collapse also *amplified* a co-located but *independent* defect — the
noise-weave "goosebumps" described in the next section. Fixing the Y
convention lifted the AO deck from 9.6 to 132.6/255 and dropped the weave
energy from 0.875 to 0.277, which read as "mostly fixed" and led to the weave
being written off as residual quantisation. It was not: it had its own root
cause and needed its own fix. **A large improvement in a metric is not
evidence that a co-located symptom shared the cause you just fixed** — when a
user reports the symptom is still there, believe them over the metric.

Rules distilled:

- When porting any screen-space reference (XeGTAO, FidelityFX, Unreal
  snippets): list every constant and formula that encodes an **NDC/UV/texel
  Y direction** (unprojection mul/add pairs, `SV_Position`-based math,
  gather offsets) and re-derive each for GL's bottom-left origin. Do not
  trust that "it renders plausibly" at one camera angle — verify at a
  **grazing** angle across a large flat surface, where a Y-reflection is
  maximally asymmetric.
- A reconstruction-convention bug and an integrator-math bug look identical
  in the output (dark/noisy AO). Separate them by *probing the inputs*: with
  `olo_render_probe_pixel`, a flat up-facing plane must decode to the same
  view-space normal the camera pitch predicts, and `LinearizeDepth` of the
  probed device-Z must match the known camera-to-surface distance. If the
  inputs check out and the math is pinned by CPU tests, the remaining
  suspects are the **uniform values** — read the upload site, not the shader.
- Pinned by `GTAOMath.NDCToViewConstantsUseGLConventionOnBothAxes`.

## 11. Quasi-random sampling: a locality-preserving index is not noise

XeGTAO seeds its per-pixel slice rotation and sample distances from a 64×64
**Hilbert-curve LUT**. It is easy to read that as "the LUT is the noise
texture" and use the stored value directly. It is not — a Hilbert curve is
**locality-preserving by construction** (that is the entire reason it is
used), so neighbouring pixels hold indices one apart. The LUT stores an
**ordering**; the **R2 low-discrepancy sequence** —
`frac(0.5 + index * vec2(0.75487766624669276005, 0.5698402909980532659114))`
— is what converts that ordering into a value, mapping sequential indices to
maximally-separated points in `[0,1)`.

Shipping `((idx + noiseIndex) & 0xFF) / 256.0` instead produced (issue #438
follow-up, reported as "goosebumps" on water):

| property (64×64 tile) | index-as-value | R2 |
|---|---|---|
| neighbour correlation, slice noise | **+0.83** | −0.10 |
| neighbour correlation, sample noise | **+0.83** | −0.26 |
| sample-noise range / mean | **[0, 0.62]** / 0.31 | [0, 1.0] / 0.50 |
| frame-to-frame correlation | **+0.977** | −0.45 |

Consequences, each of which is a recognisable on-screen signature:

- **A smooth noise field plus `round()` gives contours, not dither.** GTAO
  snaps each sample offset to whole pixels (`round(omega * offsetPixels)`).
  With decorrelated noise those boundaries fall independently per pixel and
  read as dither the denoiser and TAA remove. With a field that varies by
  ~1/256 between neighbours they fall along *level sets*, drawing a woven
  lattice that carries the LUT's **64px tile period** — measurable as an
  autocorrelation spike at lag 64 in the AO buffer.
- **It looked like a distance-banded artifact.** The lattice is only visible
  where the projected effect radius puts those contours at a resolvable
  spacing, so it occupied a band that *moved when the AO radius changed* —
  which reads as a radius/mip bug and sends you to the wrong code.
- **TAA could not fix it.** Advancing the index by 1 per frame slides the same
  field along the curve rather than redrawing it, leaving consecutive frames
  ~98% correlated. Temporal averaging needs a field that is *redrawn* each
  frame; that is what the reference's `index += 288 * (temporalIndex % 64)`
  stride is for. "Enabling TAA doesn't help" is therefore evidence *about the
  noise's temporal correlation*, not evidence that the artifact is
  geometric.
- **Deriving the second noise channel from the first is not independence.**
  `fract(noiseSlice * golden)` on an already-quantised `k/256` collapses to
  `fract(k * 0.00241)` — a ramp confined to `[0, 0.62)`, biasing every sample
  distance toward the pixel centre and undersampling the outer radius. Take
  both channels from the two R2 dimensions.

Rules distilled:

- Before using any LUT as noise, ask what it *stores*. An index, a curve
  parameter, or a sort key needs a decorrelating map; only a pre-baked blue
  noise texture is directly usable.
- Test noise **as a field, not as a formula**: nearest-neighbour correlation
  over one tile (want ≈ 0, negative is better), value range and mean, and
  frame-to-frame correlation. All three are cheap CPU assertions and each one
  maps to a distinct visible artifact.
- Pinned by `GTAOMath.R2NoiseDecorrelatesNeighbouringPixels`,
  `.SampleDistanceNoiseSpansTheFullUnitRange`,
  `.TemporalStrideRedrawsTheFieldEachFrame`, and
  `.GtaoShaderMapsHilbertIndexThroughR2Sequence`. Each threshold is paired
  with an assertion that the *regressed* formulation fails it, so the guards
  cannot pass vacuously.
