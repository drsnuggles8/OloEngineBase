// =============================================================================
// GBufferFlagsResolve.glsl — the G-Buffer flags lane's MSAA resolve (issue #996)
//
// G-Buffer RT2 is `vec4(emissive.rgb, materialFlags)`. Three of those channels
// are RADIOMETRY and want the hardware's averaging resolve; the fourth is a
// BITFIELD (unlit in bit 0, the PBR closure model in bits 1.., see
// oloEncodeGBufferPbrFlags in include/PBRCommon.glsl) and averaging it is
// nonsense — a silhouette pixel half-covered by a ClosureV2 surface (a=2.0)
// over Legacy geometry (a=0.0) averaged to exactly the unlit code 1.0 and
// every v2 object grew a raw-emissive black fringe; over the skybox (a=1.0) it
// averaged to 1.5, where GLSL round()'s tie-break is implementation-defined
// and so could differ between vendors and between GL and Vulkan.
//
// `GBuffer::Resolve()` therefore keeps the average blit for RT2 — the emissive
// channels resolve exactly as they always did, which is what keeps a
// Legacy-only frame byte-identical — and then runs this pass with the colour
// mask set to ALPHA ONLY, replacing the averaged bitfield with the flags one
// real sample wrote. `texelFetch` at a fixed sample index is exactly defined on
// every backend and vendor, which no averaged encoding of a bitfield can be.
// Which sample is picked is documented at the loop below; the short version is
// that a lit sample always outranks an unlit one, so a pixel a real surface
// covers is never called unlit.
//
// The 1:1 texel mapping is not an assumption here: the resolve target is
// allocated from the same width/height as the multisample G-Buffer, and this
// pass addresses both by `gl_FragCoord`, never by a filtered UV.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 vertex pull. On the Vulkan route the pipeline has no
// vertex-input state, so attributes are READ from binding 57 (the engine-wide
// vertex-pull binding; the root struct carries this buffer's device address).
// This pass draws MeshPrimitives::GetFullscreenTriangle(), a 20-byte
// {vec3 position @0, vec2 uv @12} interleave, so the stride is 5 floats. The
// GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
#endif

layout(location = 0) out vec2 v_TexCoord;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 5;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
#endif
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

// The layout's one executable home. Included rather than transcribed so this
// pass cannot drift from the encode in the G-Buffer writers or from the decode
// in ComputeDeferredLit.
#include "include/PBRCommon.glsl"

// The multisample RT2 itself, on the slot its resolved twin uses everywhere
// else (ShaderBindingLayout::TEX_GBUFFER_EMISSIVE = 45) — the same
// slot-holds-one-resource convention DeferredLighting_MSAA.glsl already
// follows for its sampler2DMS G-Buffer inputs.
layout(binding = 45) uniform sampler2DMS u_GBufferEmissiveMS;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int sampleCount = max(textureSamples(u_GBufferEmissiveMS), 1);

    // WHICH sample speaks for the pixel. Sample 0 is the floor — an exact
    // value some real sample wrote — but it is not the answer on its own,
    // because the ONE thing this pass must never produce is a pixel that a
    // real surface covers being called UNLIT: that returns raw emissive
    // instead of shading it, which is the dark fringe issue #996 is about.
    // So: the first LIT sample wins, and sample 0 is the fallback for a pixel
    // where every sample is unlit (pure skybox / grid / light-cube), where all
    // the samples agree anyway.
    //
    // Note what this is NOT: a majority vote or any other blend. A blend of two
    // valid codes is a third code nobody wrote, which is the whole defect.
    float flags = texelFetch(u_GBufferEmissiveMS, pixel, 0).a;
    for (int s = 0; s < sampleCount; ++s)
    {
        float candidate = texelFetch(u_GBufferEmissiveMS, pixel, s).a;
        if (!oloGBufferFlagsAreUnlit(oloDecodeGBufferFlags(candidate)))
        {
            flags = candidate;
            break;
        }
    }

    // RGB is written only to keep the output complete; the caller's colour
    // mask discards it, so the averaged emissive already in the resolve target
    // survives untouched.
    o_Color = vec4(0.0, 0.0, 0.0, flags);
}
