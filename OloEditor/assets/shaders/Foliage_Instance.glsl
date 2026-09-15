// =============================================================================
// Foliage_Instance.glsl - Instanced foliage rendering with wind animation
// Uses per-instance data for position, scale, rotation, and tint
// Supports alpha-to-coverage for grass/vegetation cutouts
//
// Draws the layer's flat card AND, up close, its authored plant mesh (issue
// #1233) — both from the shared vertex stage below, so the forward, deferred
// and shadow programs cannot place the same plant differently.
// =============================================================================

#type vertex
#version 460 core

// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageInstanceVertexStage.glsl"

#type fragment
#version 460 core

layout(location = 0) out vec4 FragColor;
// Scene FB RT3 velocity. Captures camera, per-instance motion, AND the
// per-fragment wind-sway reprojection (via v_PrevWorldPos from the vertex
// stage, which re-evaluates the wind function at `t - dt`).
layout(location = 3) out vec2 o_Velocity;
// Scene FB RT4: the diffuse half of a SKIN pixel's lighting, for the screen-space
// diffusion pass (issue #1241). This surface never shades skin, so it writes the
// "no diffusion here" code -- but it must WRITE it: an MRT output a shader leaves
// alone is undefined, not zero, and SkinDiffusion.glsl would blur the garbage
// into scene colour. See include/PBRCommon.glsl, "THE DIFFUSION HAND-OFF".
layout(location = 4) out vec4 o_SkinDiffuse;


// Inputs
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Fade;
layout(location = 6) in vec3 v_PrevWorldPos;
layout(location = 7) in float v_MeshCoverage;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// Multi-light UBO (binding 5)
layout(std140, binding = 5) uniform MultiLightData
{
    int u_NumLights;
    int _ml_pad0;
    int _ml_pad1;
    int _ml_pad2;
    // Light[0]
    vec4 u_Light0_Position;
    vec4 u_Light0_Direction;
    vec4 u_Light0_ColorIntensity;
    vec4 u_Light0_Params;
    vec4 u_Light0_Params2;
};

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
#endif

// Foliage UBO (binding 12) — shared with vertex stage
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
    vec4 _foliageImpostorParams0; // consumed by the impostor card only
    vec4 _foliageImpostorParams1; // consumed by the impostor card only
    // x = this draw is the authored mesh (1) or the flat card (0);
    // yz = the layer's mesh-to-card hand-over band (issue #1233).
    vec4 u_MeshParams;
    // xyz = the view position the hand-over is measured from, in the same
    // render-relative space as the instance pivots. NOT u_CameraPosition: the
    // shadow pass's camera is the light. See ShaderBindingLayout::FoliageUBO.
    vec4 u_MeshViewPos;
};

#include "include/FoliageInstanceGeometry.glsl"

void main()
{
    // Mesh-to-card hand-over (issue #1233). The layer's authored-mesh draw and
    // its card draw run this with the same coverage and the same dither, so
    // between them they cover each pixel exactly once — no stretch where a pine
    // and a billboard of that pine are both on screen.
    if (!foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy))
        discard;

    // Sample albedo
    vec4 texColor = texture(u_DiffuseTexture, v_TexCoord);
    vec4 color = vec4(texColor.rgb * v_Color, texColor.a);

    // Alpha test
    if (color.a < v_AlphaCutoff)
        discard;

    // Distance fade
    float dist = distance(v_WorldPos, u_CameraPosition);
    float fadeFactor = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (fadeFactor <= 0.0)
        discard;

    color.a *= fadeFactor * v_Fade;

    // Simple directional lighting (first light assumed directional)
    vec3 normal = normalize(v_Normal);
    vec3 lightDir = normalize(-u_Light0_Direction.xyz);
    float NdotL = max(dot(normal, lightDir), 0.0);

    // Two-sided lighting for foliage
    if (NdotL < 0.01)
    {
        NdotL = max(dot(-normal, lightDir), 0.0) * 0.5;
    }

    vec3 lightColor = u_Light0_ColorIntensity.rgb * u_Light0_ColorIntensity.w;
    vec3 ambient = color.rgb * 0.3;
    vec3 diffuse = color.rgb * lightColor * NdotL;

    vec3 litColor = ambient + diffuse;

    FragColor = vec4(litColor, color.a);

    // Camera-motion + wind-reprojection velocity. v_PrevWorldPos already
    // includes the prev-frame wind displacement (re-evaluated at u_PrevTime).
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos,     1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
    o_SkinDiffuse = vec4(0.0); // not skin -- see the declaration above (#1241)
}
