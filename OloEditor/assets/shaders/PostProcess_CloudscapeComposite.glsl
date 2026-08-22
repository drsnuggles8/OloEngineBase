// =============================================================================
// PostProcess_CloudscapeComposite.glsl — cloud upsample + composite (pass C)
//
// Full resolution: depth-aware 2x2 upsample of the half-res resolved cloud
// buffer (mirrors PostProcess_FogUpsample's bilateral weighting — cloud
// edges against geometry need the depth guard, the open sky does not), then
// the standard transmittance composite over the scene colour:
//     result = sceneColor * cloudTransmittance + cloudInscatter
// This runs BEFORE the fog pass in the post chain, so the froxel fog +
// analytic tail apply aerial perspective over the clouds for free.
// =============================================================================

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

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)  // full-res upstream colour — TEX_DIFFUSE
#define u_CloudResolved OLO_HEAP_TEX_2D(1)  // half-res resolved clouds — TEX_SPECULAR
#define u_DepthTexture OLO_HEAP_TEX_2D(19)  // full-res scene depth — TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_SceneColor;   // full-res upstream colour
layout(binding = 1) uniform sampler2D u_CloudResolved; // half-res resolved clouds
layout(binding = 19) uniform sampler2D u_DepthTexture; // full-res scene depth
#endif

void main()
{
    vec3 scene = texture(u_SceneColor, v_TexCoord).rgb;

    // Depth-aware 2x2 gather: weight each half-res tap by how close its
    // (half-res) depth neighborhood is to this full-res pixel's depth,
    // mirroring the fog upsample's bilateral idea. Sky pixels (depth ~1)
    // dominate cloud coverage, where a plain bilinear tap is already right.
    float centerDepth = texture(u_DepthTexture, v_TexCoord).r;
    vec2 halfTexel = 1.0 / vec2(textureSize(u_CloudResolved, 0));

    vec4 cloud = vec4(0.0);
    float weightSum = 0.0;
    for (int y = 0; y <= 1; ++y)
    {
        for (int x = 0; x <= 1; ++x)
        {
            vec2 offset = (vec2(x, y) - 0.5) * halfTexel;
            vec2 uv = v_TexCoord + offset;
            // Compare against the full-res depth at the tap position — a
            // cheap stand-in for a half-res depth pyramid.
            float tapDepth = texture(u_DepthTexture, uv).r;
            float depthDelta = abs(tapDepth - centerDepth);
            float w = exp(-depthDelta * 400.0) + 1.0e-3;
            cloud += texture(u_CloudResolved, uv) * w;
            weightSum += w;
        }
    }
    cloud /= weightSum;

    o_Color = vec4(scene * clamp(cloud.a, 0.0, 1.0) + max(cloud.rgb, vec3(0.0)), 1.0);
}
