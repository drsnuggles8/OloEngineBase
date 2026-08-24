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

// SSGI composite (issue #902) — draw C of SSGIRenderPass.
//
// Adds the temporally-resolved indirect diffuse to the upstream lit colour.
// This runs AFTER the resolve on purpose: compositing first and resolving the
// composite is the failure #902 exists to avoid, because it would accumulate
// the base colour along with the stochastic term.
//
// Intensity is applied HERE rather than inside the accumulated signal, so
// dragging the slider does not have to wait for the history to converge.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_ResolvedSignal OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_SceneColor; // upstream lit HDR colour
layout(binding = 1) uniform sampler2D u_ResolvedSignal;   // resolved indirect diffuse (rgb)
#endif

// The SAME std140 block PostProcess_SSGI.glsl declares (SSGIUBOData).
layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;  // x = Intensity, y = RayCount, z = EdgeFade, w = unused
    vec4 u_ScreenParams;
    vec4 u_Flags;        // x = DebugView (0/1), y = FrameIndex, zw = pad
    vec4 u_TemporalParams;
};

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;
    vec3 indirectDiffuse = max(texture(u_ResolvedSignal, v_TexCoord).rgb, vec3(0.0)) * u_ShadeParams.x;

    if (u_Flags.x > 0.5) // debug: the resolved indirect-diffuse contribution in isolation
    {
        o_Color = vec4(indirectDiffuse, 1.0);
        return;
    }

    o_Color = vec4(baseColor + indirectDiffuse, 1.0);
}
