// =============================================================================
// ShaderUnit_ShadowVisualize.glsl
//
// 2D shadow-mask visualization probe for scene-level golden tests. Unlike
// `ShaderUnit_ShadowSelfShadow.glsl` (which sweeps depth along one screen
// axis and sample-XY along the other to assert self-shadow / peter-panning
// invariants), this probe maps screen UV directly onto shadow UV and uses
// a single UBO-provided reference depth for the entire image. That makes a
// 2D-authored shadow map render as a 2D shadow mask — exactly what a scene
// integration golden wants to validate (PCF kernel, sampler2DArrayShadow
// binding, HDR output path).
//
// UV encoding:
//   v_TexCoord.xy → projCoords.xy  (shadow map sample position)
//   projCoords.z  = u_ShadowParams.y  (single reference depth for all pixels)
//
// UBO layout (std140, binding 18) — identical to ShaderUnit_ShadowSelfShadow
// so host-side UBO plumbing can be shared:
//   x = bias
//   y = reference depth (projCoords.z for every fragment)
//   z, w = unused
//   int = shadow map resolution
//
// Output:
//   R: PCF shadow factor (0 = shadowed, 1 = lit; 3×3 kernel, values in
//      {0, 1/9, ..., 1})
//   G: reference depth (echoed)
//   B: bias (echoed)
//   A: 1
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

// Matches production TEX_SHADOW binding.
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;

layout(std140, binding = 18) uniform ShadowProbeUBO
{
    // x=bias, y=reference depth, z=unused, w=unused.
    vec4 u_ShadowParams;
    // Must match the shadow-map texture resolution set up by the host.
    int  u_ShadowResolution;
    int  u_Pad0;
    int  u_Pad1;
    int  u_Pad2;
};

// Byte-for-byte replica of PBRCommon.glsl::sampleShadowPCF — same kernel
// as the production path and as ShaderUnit_ShadowSelfShadow.
float sampleShadowPCF(sampler2DArrayShadow shadowMap, vec3 projCoords,
                      float layer, float bias, int resolution)
{
    float shadow = 0.0;
    float texelSize = 1.0 / float(resolution);

    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += texture(shadowMap, vec4(projCoords.xy + offset, layer,
                                              projCoords.z - bias));
        }
    }
    return shadow / 9.0;
}

void main()
{
    // Direct screen-UV → shadow-UV mapping so a 2D-authored shadow map
    // renders as a 2D shadow mask.
    vec3 projCoords = vec3(v_TexCoord, u_ShadowParams.y);
    float bias = u_ShadowParams.x;

    // Force cascade 0; cascade-selection logic is exercised independently
    // by ShaderUnit_ShadowBounds.glsl.
    float shadow = sampleShadowPCF(u_ShadowMapCSM, projCoords, 0.0, bias,
                                   u_ShadowResolution);

    o_Color = vec4(shadow, u_ShadowParams.y, bias, 1.0);
}
