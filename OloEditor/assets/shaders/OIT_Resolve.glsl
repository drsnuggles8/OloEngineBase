// OIT_Resolve.glsl — Composite weighted-blended OIT accumulation buffers
// over the scene colour. Runs as a fullscreen draw after all transparent
// passes have accumulated into OITBuffer.
//
// Inputs (sampler bindings match ShaderBindingLayout):
//   binding 48 : u_OITAccum     (RGBA16F) sum(Ci * ai * wi) + sum(ai * wi)
//   binding 49 : u_OITRevealage (R component) prod(1 - ai)
//
// Output blending (configured by OITResolveRenderPass):
//   Target : scene FB RT0 (RGBA16F HDR)
//   Blend  : GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA
//   Where SRC.a carries the revealage factor so the opaque background is
//   modulated by prod(1 - ai) and the fragment RGB adds the weighted sum.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead. OLO_VULKAN is
// defined only on the Vulkan shaderc route; the GL branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). The BODY below is
// byte-identical between the two variants — only these declarations move, and
// each names the same TEX_* constant OITResolveRenderPass binds with.
#ifdef OLO_BINDLESS
#define u_OITAccum OLO_HEAP_TEX_2D(48)     // TEX_OIT_ACCUM
#define u_OITRevealage OLO_HEAP_TEX_2D(49) // TEX_OIT_REVEALAGE
#else
layout(binding = 48) uniform sampler2D u_OITAccum;
layout(binding = 49) uniform sampler2D u_OITRevealage;
#endif

void main()
{
    vec4 accum = texture(u_OITAccum, v_TexCoord);
    float revealage = texture(u_OITRevealage, v_TexCoord).r;

    // If revealage is 1.0 the pixel is fully transparent — discard to
    // avoid any floating-point churn in the blend pipeline.
    if (revealage >= 1.0 - 1e-5)
        discard;

    // Average colour = sum(Ci * ai * wi) / sum(ai * wi).
    float denom = max(accum.a, 1e-4);
    vec3 averageColor = accum.rgb / denom;

    // Emit colour modulated by (1 - revealage) so the blend equation
    //   dst' = dst * SRC.a + SRC.rgb * (1 - SRC.a)
    //        = dst * revealage + averageColor * (1 - revealage)
    // matches the WB-OIT composite (with blend factors
    // SRC=GL_ONE_MINUS_SRC_ALPHA, DST=GL_SRC_ALPHA).
    o_Color = vec4(averageColor, revealage);
}
