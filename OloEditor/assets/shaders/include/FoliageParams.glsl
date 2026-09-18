#ifndef OLO_FOLIAGE_PARAMS_GLSL
#define OLO_FOLIAGE_PARAMS_GLSL
// ShaderBindingLayout::FoliageUBO, 1312 bytes. One declaration for every stage,
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
    // Local interaction bending (issue #1238).
    //   x = active influence count, y = this layer's response scale (0 = the
    //   layer does not react at all), z = the summed-push ceiling in world
    //   units — the same number FoliageInteractionMaximumDisplacement pads the
    //   instance AABB by, which is what makes a bent plant uncullable.
    vec4 u_InteractionParams;
    // OLO_FOLIAGE_INTERACTION_SLOTS influences, four vec4 each, laid out
    // exactly as FoliageInteractionSlot: Center/radius, Push/falloff,
    // PrevCenter/height, PrevPush. Flat vec4[] rather than a struct array
    // because std140's vec4 stride is already the struct's own.
    vec4 u_Interactions[64];
};
#endif
