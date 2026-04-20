// =============================================================================
// ShaderUnit_SplatmapChannel.glsl
//
// Channel-isolation probe for terrain splatmap blending. Samples a
// sampler2DArray (4 layers, each a solid distinct color) and a single
// sampler2D splatmap where the fragment reads channel weights from the .rgba
// of texel (0, 0). Output = Σ weight_i * layer_i.
//
// Test cases:
//   - weights = (1, 0, 0, 0)  →  output == layer 0 color
//   - weights = (0, 1, 0, 0)  →  output == layer 1 color
//   - weights = (0, 0, 1, 0)  →  output == layer 2 color
//   - weights = (0, 0, 0, 1)  →  output == layer 3 color
//   - weights = (0.5, 0.5, 0, 0)  →  output == 0.5*layer0 + 0.5*layer1
//
// Production bug surface covered: swizzle inversions (weights.rgba mapped
// to wrong layer indices), array-layer indexing off-by-one, weight
// renormalization errors.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(binding = 20) uniform sampler2DArray u_LayerArray;
layout(binding = 24) uniform sampler2D u_Splatmap;

void main()
{
    vec4 w = texture(u_Splatmap, v_TexCoord);
    vec3 c0 = texture(u_LayerArray, vec3(v_TexCoord, 0.0)).rgb;
    vec3 c1 = texture(u_LayerArray, vec3(v_TexCoord, 1.0)).rgb;
    vec3 c2 = texture(u_LayerArray, vec3(v_TexCoord, 2.0)).rgb;
    vec3 c3 = texture(u_LayerArray, vec3(v_TexCoord, 3.0)).rgb;
    vec3 blended = w.r * c0 + w.g * c1 + w.b * c2 + w.a * c3;
    o_Color = vec4(blended, 1.0);
}
