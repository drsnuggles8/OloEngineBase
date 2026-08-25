#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
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

// SSR composite (issue #902) — draw C of SSRRenderPass.
//
// Adds the temporally-resolved reflection DELTA to the upstream lit colour.
// The delta is (reflection - base) * blend, so `base + delta` reproduces the
// old `mix(base, reflection, blend)` exactly — the same replace/mix resolve,
// just with the stochastic half of it accumulated first. This runs AFTER the
// resolve on purpose: compositing first and resolving the composite is the
// failure #902 exists to avoid.
//
// Unlike SSGI, SSR's intensity is NOT applied here. It rides inside `blend`
// upstream because the blend is clamped to [0,1] after the multiply, and
// hoisting the multiply out would change what an intensity above 1 does.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_ResolvedSignal OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_SceneColor; // upstream lit HDR colour
layout(binding = 1) uniform sampler2D u_ResolvedSignal;   // resolved reflection delta (rgb)
#endif

// The SAME std140 block PostProcess_SSR.glsl declares (SSRUBOData).
layout(std140, binding = 38) uniform SSRParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;
    vec4 u_ScreenParams;
    vec4 u_Flags;        // x = DebugView (0/1), y = FrameIndex, zw = pad
    vec4 u_HZBParams;
    vec4 u_TemporalParams;
};

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;
    vec3 reflectionDelta = texture(u_ResolvedSignal, v_TexCoord).rgb;

    if (u_Flags.x > 0.5) // debug: the resolved reflection delta in isolation
    {
        // Clamped like the composite below, and for the same reason: the delta
        // is deliberately SIGNED (a reflection darker than the surface it
        // replaces is legitimate), but this branch writes straight into
        // SSRColor, which bloom and the tonemapper then consume. The pre-#902
        // debug output was non-negative by construction; keep that property
        // rather than making a debug toggle able to push negative HDR radiance
        // down the chain.
        o_Color = vec4(max(reflectionDelta, vec3(0.0)), 1.0);
        return;
    }

    // Radiance cannot go negative even though the delta can: a strong negative
    // delta on a nearly-black surface would otherwise push the composite below
    // zero and make the tonemapper's job undefined.
    o_Color = vec4(max(baseColor + reflectionDelta, vec3(0.0)), 1.0);
}
