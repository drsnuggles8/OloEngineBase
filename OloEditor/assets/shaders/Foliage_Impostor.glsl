// =============================================================================
// Foliage_Impostor.glsl — octahedral impostor card for distant foliage (issue
// #433), FORWARD variant. Replaces the flat Y-rotated billboard with a fully
// camera-facing card that samples a baked octahedral atlas (albedo +
// object-normal + depth), blending the 3 lattice frames around the current view
// direction (no slice pop), applying single-step depth parallax (kills the
// flat-card look), and relighting from the baked object normal. A
// distance-driven detail ramp across [ImpostorStart, ImpostorStart+Band] fades
// parallax + cross-frame blend in with range so there is no visible transition.
// Forward pass: composites into SceneColor after opaque.
//
// The card's placement (vertex stage) and its atlas sampling + discard rule
// (fragment body) live in shared includes with the deferred sibling,
// Foliage_Impostor_GBuffer.glsl (#1225): the two paths must place and sample
// the same card or it jumps at the Forward/Deferred seam. This file owns only
// its OUTPUT — the self-relight and SceneColor write.
// =============================================================================

#type vertex
#version 460 core

// Nothing in this fragment stage reads the instance index — declare no
// varying for it (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning). The deferred sibling omits this define.
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageImpostorVertexStage.glsl"

#type fragment
#version 460 core

layout(location = 0) out vec4 FragColor;
layout(location = 3) out vec2 o_Velocity;

layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 6) in vec3 v_PrevCardWorld;
layout(location = 7) in float v_Radius; // WORLD-space card radius

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

layout(std140, binding = 5) uniform MultiLightData
{
    int u_NumLights;
    int _ml_pad0;
    int _ml_pad1;
    int _ml_pad2;
    vec4 u_Light0_Position;
    vec4 u_Light0_Direction;
    vec4 u_Light0_ColorIntensity;
    vec4 u_Light0_Params;
    vec4 u_Light0_Params2;
};

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
    vec3 u_FoliageBaseColor;
    float _foliagePad2;
    vec4 u_ImpostorParams0; // x=framesPerAxis, y=hemi, z=startDistance, w=transitionBand
    vec4 u_ImpostorParams1; // x=enabled, y=meshRadius, z=parallaxScale, w=unused
};

#include "include/FoliageImpostorSampling.glsl"

void main()
{
    ImpostorSample card = SampleImpostorCard();

    // Relight from the baked object-space normal (dynamic sun direction). This
    // is the forward path's ONE light plus a flat ambient; the deferred sibling
    // hands the same normal to DeferredLightingPass instead.
    vec3 worldN = normalize(rotateY(card.LocalNormal, v_Rotation));
    vec3 lightDir = normalize(-u_Light0_Direction.xyz);
    float NdotL = max(dot(worldN, lightDir), 0.0);
    if (NdotL < 0.01)
        NdotL = max(dot(-worldN, lightDir), 0.0) * 0.5; // two-sided foliage

    vec3 lightColor = u_Light0_ColorIntensity.rgb * u_Light0_ColorIntensity.w;
    vec3 ambient = card.Albedo * 0.3;
    vec3 diffuse = card.Albedo * lightColor * NdotL;
    vec3 litColor = ambient + diffuse;

    // Foliage blends are OFF (opaque alpha-tested), so this alpha is never
    // seen; the visible fade is the discard SampleImpostorCard applies.
    FragColor = vec4(litColor, card.Coverage * card.DistFade);

    // Camera-motion velocity (impostor has no per-instance prev history).
    vec4 clipCurr = u_ViewProjection * vec4(v_CardWorld, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevCardWorld, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
