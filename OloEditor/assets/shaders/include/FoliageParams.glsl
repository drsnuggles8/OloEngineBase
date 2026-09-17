#ifndef OLO_FOLIAGE_PARAMS_GLSL
#define OLO_FOLIAGE_PARAMS_GLSL
// ShaderBindingLayout::FoliageUBO, 272 bytes. One declaration for every stage,
// including depth and impostors, prevents cross-stage block-link mismatches.
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;
    float u_WindHistoryValid;
    vec3 u_FoliageBaseColor;
    float _foliagePad2;
    vec4 u_ImpostorParams0;
    vec4 u_ImpostorParams1;
    vec4 u_MeshParams;
    vec4 u_MeshViewPos;
    vec4 u_LeafSurface;
    vec4 u_LeafTransmit;
    vec4 u_LeafLobe;
    vec4 u_LeafIds;
    vec4 u_WindWeights; // stiffness, branch, leaf, displacement debug
    vec4 u_WindDirection;
    vec4 u_WindGust;
    vec4 u_WindFlags; // absolute render origin, field enabled
    vec4 u_WindClock;
    vec4 u_PrevMeshViewPos;
};
#endif
