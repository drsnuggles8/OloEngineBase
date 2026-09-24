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

// Texture inputs. Under heap-bindless (issue #691) these become heap
// lookups keyed by the SAME slot numbers the bindful branch declares, so the two
// variants cannot disagree about which texture is which — and the shader BODY
// below is unchanged between them. Inert without OLO_BINDLESS.
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_DepthTexture OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#define u_GBufferNormal OLO_HEAP_TEX_2D(44) // TEX_GBUFFER_NORMAL
#else
layout(binding = 0) uniform sampler2D u_SceneColor;     // lit upstream HDR colour
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // RT1: rg = oct world normal, z = roughness, w = ao
#endif

// Screen-Space Contact Shadows — the DEBUG VIEW (issue #1336).
//
// The shadow itself is no longer composited here. It is a visibility for the
// primary directional light, and DeferredLighting multiplies it into that
// light's term (include/ContactShadowCommon.glsl has the march and the reason);
// multiplying the finished frame, as this pass did, also darkened ambient,
// emission, reflections and every other light. RenderPipeline runs this pass
// only for ContactShadowDebugView, where it shows the factor as greyscale — the
// SAME function the lighting pass evaluates, so the view shows what is applied.
// Outside the debug view it passes the colour through unchanged.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#define OLO_CONTACT_SHADOW_TAP_DEPTH(uv) texture(u_DepthTexture, (uv)).r
#include "include/ContactShadowCommon.glsl"

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl.
vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;
    if (u_ContactShadow.Flags.x < 0.5)
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    float depth = texture(u_DepthTexture, v_TexCoord).r;
    vec3 Nworld = OctDecode(texture(u_GBufferNormal, v_TexCoord).xy);
    float visibility = oloContactShadowVisibility(v_TexCoord, depth, Nworld, gl_FragCoord.xy);
    o_Color = vec4(vec3(visibility), 1.0);
}
