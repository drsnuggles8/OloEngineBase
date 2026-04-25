// Debug visualisation for RT1.z (roughness), RT0.a (metallic), RT1.w (AO).
// Rendered full-screen into the scene framebuffer's colour attachment 0
// when DeferredSettings::DebugChannel == 3. The underlying blit used by
// the other debug channels can only copy one attachment at a time, so
// this tiny shader gathers the three channels into RGB manually.

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

void main()
{
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;

layout(binding = 43) uniform sampler2D u_GAlbedo;    // RT0: albedo.rgb + metallic.a
layout(binding = 44) uniform sampler2D u_GNormal;    // RT1: octNormal.xy + roughness.z + AO.w

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4 g0 = texelFetch(u_GAlbedo, coord, 0);
    vec4 g1 = texelFetch(u_GNormal, coord, 0);

    float roughness = g1.z;
    float metallic  = g0.a;
    float ao        = g1.w;

    o_Color = vec4(roughness, metallic, ao, 1.0);
}
