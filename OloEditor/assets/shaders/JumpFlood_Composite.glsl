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

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). The BODY below is
// byte-identical between the two variants.
#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_JFAResult OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_SceneColor;
layout(binding = 1) uniform sampler2D u_JFAResult;
#endif

layout(std140, binding = 29) uniform JumpFloodUBO
{
    vec4  u_TexelSize;
    vec4  u_OutlineColor;           // rgb = color, a = opacity
    float u_OutlineThicknessInner;  // smoothstep inner edge
    float u_OutlineThicknessOuter;  // smoothstep outer edge
    int   u_Step;
    int   _pad0;
};

void main()
{
    vec4 sceneColor = texture(u_SceneColor, v_TexCoord);
    vec4 jfa = texture(u_JFAResult, v_TexCoord);

    // Convert squared distance to linear distance
    float dist = sqrt(jfa.z);

    // Anti-aliased outline band via smoothstep (outer → inner = transparent → opaque)
    float alpha = smoothstep(u_OutlineThicknessOuter, u_OutlineThicknessInner, dist);

    if (alpha == 0.0)
    {
        o_Color = sceneColor;
        return;
    }

    // Alpha-blend outline color over scene
    float blendAlpha = alpha * u_OutlineColor.a;
    o_Color = vec4(mix(sceneColor.rgb, u_OutlineColor.rgb, blendAlpha), sceneColor.a);
}
