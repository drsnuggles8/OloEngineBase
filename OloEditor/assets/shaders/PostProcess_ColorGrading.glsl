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
// below is unchanged between them. Inert without OLO_BINDLESS; the engine only
// defines it on the raw-GLSL compile route.
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#define u_LUT OLO_HEAP_TEX_2D(18) // TEX_POSTPROCESS_LUT
#else
layout(binding = 0) uniform sampler2D u_Texture;
layout(binding = 18) uniform sampler2D u_LUT; // 3D LUT stored as strip (e.g., 256x16)
#endif

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;


// LUT size: assumes 16x16x16 LUT stored as a horizontal strip of 16 tiles, each 16x16
const float LUT_SIZE = 16.0;

void main()
{
    vec3 color = texture(u_Texture, v_TexCoord).rgb;
    color = clamp(color, 0.0, 1.0);

    // Blue channel selects tile
    float blueIndex = color.b * (LUT_SIZE - 1.0);
    float tileLow = floor(blueIndex);
    float tileHigh = min(tileLow + 1.0, LUT_SIZE - 1.0);
    float blueFrac = blueIndex - tileLow;

    // Map red/green to UV within a tile
    vec2 uvLow;
    uvLow.x = (tileLow + color.r * (LUT_SIZE - 1.0) / LUT_SIZE + 0.5 / (LUT_SIZE * LUT_SIZE)) / LUT_SIZE;
    uvLow.y = (color.g * (LUT_SIZE - 1.0) + 0.5) / LUT_SIZE;

    vec2 uvHigh;
    uvHigh.x = (tileHigh + color.r * (LUT_SIZE - 1.0) / LUT_SIZE + 0.5 / (LUT_SIZE * LUT_SIZE)) / LUT_SIZE;
    uvHigh.y = uvLow.y;

    vec3 colorLow = texture(u_LUT, uvLow).rgb;
    vec3 colorHigh = texture(u_LUT, uvHigh).rgb;

    o_Color = vec4(mix(colorLow, colorHigh, blueFrac), 1.0);
}
