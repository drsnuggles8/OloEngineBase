// =============================================================================
// DDGI_Capture.glsl — probe hit-point-cache capture (issue #632)
//
// Rasterizes DDGI mesh casters into one cube face cell of the 3x2 capture
// grid around a probe (DDGIProbeUpdatePass sets the per-face camera UBO +
// viewport). Writes the mini-G-buffer the resample pass converts to the
// octahedral hit cache:
//   RT0 (RGBA8)   — albedo.rgb (baseColor * texture), a = gl_FrontFacing
//                   flag (1.0 front / 0.5 back; sky texels keep the 0 clear)
//   RT1 (RGBA16F) — rg = ddgiOctEncode(worldNormal, flipped toward the viewer
//                   for backfaces), b = linear distance from the probe,
//                   a = 0 (the resample canonicalizes the flag into HitGeo.a)
//
// Culling is disabled by the pass for every caster — backface texels are the
// in-wall-probe classification signal, tagged via gl_FrontFacing here.
// All positions are render-origin-relative (issue #429): the pass shifts the
// model matrix and the probe position by the same origin.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Per-draw capture data — mirrors DDGIPassDataUBO in DDGIProbeUpdatePass.cpp.
layout(std140, binding = 7) uniform DDGIPassData
{
    mat4 u_DDGIModel;         // render-relative model matrix
    mat4 u_DDGINormalMatrix;  // transpose(inverse(model))
    vec4 u_DDGIBaseColor;     // material base color factor
    vec4 u_DDGIProbePosition; // xyz = render-relative probe position, w = probe linear index
};

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    vec4 worldPos = u_DDGIModel * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = mat3(u_DDGINormalMatrix) * a_Normal;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

#include "include/DDGICommon.glsl"

layout(std140, binding = 7) uniform DDGIPassData
{
    mat4 u_DDGIModel;
    mat4 u_DDGINormalMatrix;
    vec4 u_DDGIBaseColor;
    vec4 u_DDGIProbePosition;
};

layout(binding = 0) uniform sampler2D u_AlbedoMap; // white fallback when the caster has no texture

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_AlbedoFlag; // rgb = albedo, a = front/back flag
layout(location = 1) out vec4 o_GeoDist;    // rg = oct normal, b = linear distance, a = 0

void main()
{
    vec3 albedo = u_DDGIBaseColor.rgb * texture(u_AlbedoMap, v_TexCoord).rgb;

    // Flip the normal toward the viewer for backfaces: single-sided geometry
    // seen from behind still reports a meaningful surface orientation, and the
    // 0.5 flag marks it as a backface hit for classification/relight.
    vec3 n = normalize(v_Normal);
    if (!gl_FrontFacing)
    {
        n = -n;
    }
    float flag = gl_FrontFacing ? DDGI_HIT_FRONTFACE : DDGI_HIT_BACKFACE;

    float dist = length(v_WorldPos - u_DDGIProbePosition.xyz);

    o_AlbedoFlag = vec4(albedo, flag);
    o_GeoDist = vec4(ddgiOctEncode(n), dist, 0.0);
}
