#version 460 core

// GPU contract probe for the production surface-history helpers (#976).
layout(local_size_x = 1, local_size_y = 1) in;

#include "../include/SurfaceHistory.glsl"

layout(std430, binding = 1) writeonly buffer Outputs
{
    uvec4 u_Reasons;
    vec4 u_FirstMoment;
    vec4 u_SecondMoment;
    vec4 u_Metadata;
};

OloSurfaceHistoryRecord MakeRecord()
{
    OloSurfaceHistoryRecord record;
    record.LinearDepth = 5.0;
    record.GeometricNormal = vec3(0.0, 0.0, 1.0);
    record.ShadingNormal = vec3(0.0, 0.0, 1.0);
    record.Roughness = 0.4;
    record.MaterialClass = 3u;
    record.Motion = vec2(0.0);
    record.Instance = uvec2(7u, 2u);
    record.Primitive = uvec2(11u, 4u);
    record.Material = uvec2(13u, 5u);
    record.Flags = 0u;
    record.HitDistance = 0.0;
    record.PrimitiveLocalIndex = 1u;
    return record;
}

OloSurfaceHistorySettings MakeSettings()
{
    OloSurfaceHistorySettings settings;
    settings.TestMask = OLO_SURFACE_TEST_INSTANCE | OLO_SURFACE_TEST_PRIMITIVE |
                        OLO_SURFACE_TEST_MATERIAL;
    settings.RelativeDepthThreshold = 0.02;
    settings.GeometricNormalCosineThreshold = 0.8;
    settings.ShadingNormalCosineThreshold = 0.8;
    settings.RoughnessThreshold = 0.1;
    settings.MotionThresholdPixels = 8.0;
    settings.RelativeHitDistanceThreshold = 0.1;
    settings.PixelSize = vec2(1.0 / 640.0, 1.0 / 360.0);
    return settings;
}

void main()
{
    OloSurfaceHistoryRecord current = MakeRecord();
    OloSurfaceHistoryRecord previous = MakeRecord();
    OloSurfaceHistorySettings settings = MakeSettings();

    previous.Instance = uvec2(8u, 2u);
    uint instanceMismatch = OloEvaluateSurfaceHistory(current, previous, vec2(0.5), true, settings);

    previous = MakeRecord();
    previous.Material = uvec2(13u, 6u);
    uint materialGenerationMismatch = OloEvaluateSurfaceHistory(current, previous, vec2(0.5), true, settings);

    previous = MakeRecord();
    previous.Instance = uvec2(0xffffffffu, 0u);
    uint missingIdentity = OloEvaluateSurfaceHistory(current, previous, vec2(0.5), true, settings);

    previous = MakeRecord();
    uint stable = OloEvaluateSurfaceHistory(current, previous, vec2(0.5), true, settings);
    u_Reasons = uvec4(instanceMismatch, materialGenerationMismatch, missingIdentity, stable);

    settings.TestMask |= OLO_SURFACE_TEST_HIT_DISTANCE;
    current.HitDistance = uintBitsToFloat(0x7fc00000u);
    previous.HitDistance = current.HitDistance;
    uint unusedOptionalHitDistance = OloEvaluateSurfaceHistory(current, previous, vec2(0.5), true, settings);

    OloTemporalMoments prior;
    prior.First = vec4(100.0);
    prior.Second = vec4(10000.0);
    prior.HistoryLength = 64.0;
    vec4 signal = vec4(0.25, 0.5, 0.75, 1.0);
    OloTemporalMoments firstFrame = OloAccumulateTemporalMoments(signal, prior, false, 32.0);
    u_FirstMoment = firstFrame.First;
    u_SecondMoment = firstFrame.Second;
    u_Metadata = vec4(firstFrame.HistoryLength, OloTemporalVariance(firstFrame).x,
                      float(unusedOptionalHitDistance), 0.0);
}
