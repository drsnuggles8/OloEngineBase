# The OpenGL shader route: GLSL text where NVIDIA's SPIR-V ingestion is slow

Rules for how an OpenGL program reaches the driver (#1533, E2). Read before touching
`OpenGLShader::FinalizeGL`, `CreateProgram`, `CreateProgramFromGlslText`, or adding
`OLO_GL_GLSL_ROUTE` to a shader.

## The rules

1. **A GL program takes one of two routes, and the shader decides.** The default compiles the
   SPIRV-Cross GLSL of the Vulkan SPIR-V to OpenGL SPIR-V with glslang and hands that to
   `glShaderBinary` + `glSpecializeShader`. A shader that names `OLO_GL_GLSL_ROUTE` outside
   comments (`#define OLO_GL_GLSL_ROUTE 1` in its first stage) hands the same SPIRV-Cross GLSL to
   `glShaderSource` instead (`CreateProgramFromGlslText`, also the route AMD drivers older than
   23.5.2 take). Vulkan never reads the token.

2. **Opt a shader in only on a measured A/B, and say what was measured.** The routes compile one
   GLSL text, but NVIDIA's two front ends do not produce one program. On an RTX 4090 with driver
   617.14, in the dog scene's Play view at 1920x1080:

   | shaders on the text route | effect |
   |---|---|
   | `Foliage_Instance`, `Foliage_Impostor` (forward lawn) | FoliagePass 3.6 -> 1.5 ms |
   | `Terrain_PBR`, `Terrain_Depth` | ScenePass 2.3 -> 1.6 ms, ScenePrepass 1.6 -> 1.1 ms |
   | the groom (`GroomStrand*`) | GroomPass 7.3 -> 9.0 ms, **slower** |
   | depth, depth-normal and G-Buffer foliage, mesh PBR, post passes | no change |

   So the route is per shader, not global: the first two rows carry the token, and so do the
   depth-normal prepass twins of the first, which must share its route (rule 5) and cost the same on
   either.

3. **The text route emits only the resources a stage uses.** The GLSL front end counts every
   DECLARED uniform block against `GL_MAX_<stage>_UNIFORM_BLOCKS` (14 on NVIDIA), and the terrain's
   fragment stage declares more than that through its includes: on the text route it failed to
   link ("no buffers available for bindable uniform") while the SPIR-V route linked it.
   `set_enabled_interface_variables(get_active_interface_variables())` drops the unused
   declarations; bindings are explicit, so nothing the program reads moves.

4. **`OLO_GL_SHADERS_FROM_GLSL` is the A/B.** Unset, each shader's token decides; `1` puts every
   program on the text route (narrowed by `OLO_GL_SHADERS_FROM_GLSL_MATCH=Foliage_,Terrain`);
   `0` puts none. It is read as each program is created, so set it before launch; `Reload()`
   re-decides, so a test can swap routes in one process.

5. **A colour program and the prepass it depth-tests against take one route.** `invariant
   gl_Position` holds only within one compiler. The forward foliage colour pass draws at `GL_LEQUAL`
   against its depth-normal prepass, and with `Foliage_Instance` on the text route and
   `Foliage_Instance_DepthNormal` on SPIR-V, NVIDIA's two front ends rounded the same position
   apart: with AO on (which runs the prepass) 30158 pixels of `AOFoliageParityTest`'s stand got
   brighter instead of darker, because the colour pass lost leaf fragments to the prepass's depth.
   With both twins on the text route the count is 131. The pairs are `Foliage_Instance` /
   `Foliage_Instance_DepthNormal`, `Foliage_Impostor` / `Foliage_Impostor_DepthNormal` and
   `Terrain_PBR` / `Terrain_Depth`; `OpenGLShaderRouteTest` holds each pair to one route. Shadow
   programs are not twins: a shadow map is never depth-tested against the main view.

## What was ruled out

- **Optimising the GL tier's SPIR-V** (`shaderc_optimization_level_performance` on the recompile)
  left the frame at 19.7 ms against 19.9 and broke `FluidThickness.glsl`'s async link.
- **Early depth testing.** Forcing `early_fragment_tests` in the foliage colour pass, which writes
  no depth after the prepass, changed FoliagePass by nothing.

## Measured

E2 (p50 <= 16.7 ms) at the three framings, 1024 frames of the profiler ring, same build and same
views, the scene camera's own field of view:

| GL p50 / p95 ms | Face close-up | Full body | Walk |
|---|---|---|---|
| every shader on SPIR-V (`OLO_GL_SHADERS_FROM_GLSL=0`) | 18.93 / 19.94 | 17.81 / 18.74 | 17.56 / 18.44 |
| as shipped (the token) | 15.97 / 16.81 | 14.29 / 15.25 | 13.29 / 14.26 |
| as shipped, again | 16.03 / 16.93 | 14.33 / 15.28 | 13.36 / 14.46 |
| Vulkan, for scale | 12.28 / 12.82 | 10.81 / 11.34 | 9.06 / 9.82 |

The pixels: the foliage habitat, LOD-coverage and leaf-transmission suites and the terrain
virtual-texture suite, run twice as shipped and once with every shader on SPIR-V. Where a suite is
deterministic (the second run identical to the first), the routes differ by 0.02-0.45 of 255 on
average forward and up to 1.0 deferred, with at most 3.9% of pixels more than 8 levels apart, all
on blade edges and dithered fades -- two compilers rounding at a threshold. The terrain frames are
identical. Every suite passes on both routes.

## Things that will bite

- **The text route links synchronously.** It has no `KHR_parallel_shader_compile` path; an
  opted-in shader adds its link to startup.
- **Its program-binary cache is keyed by the Vulkan SPIR-V**, the input it builds from, so it can
  never be served the SPIR-V route's binary (or the reverse), and its reflection reads that SPIR-V.
- **A token in a comment does nothing**, by design (`MentionsOutsideComments`), as with `OLO_BINDLESS`.

## Where the evidence lives

- `OpenGLShaderRouteTest.cpp` (shaderpipe): every shader that names the token links on the text
  route, the tessellated terrain among them; a shader that does not stays on SPIR-V; the lever
  forces both ways.
- The pixel and timing runs above are reproduced with the lever and the four evidence suites.
