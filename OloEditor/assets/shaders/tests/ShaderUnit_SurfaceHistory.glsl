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
    // #1256: the coverage rejection bit, then the three SEPARATED reactive
    // causes. The CPU side asserts these against EvaluateTemporalReactivity
    // run on the same inputs rather than against hard-coded constants, so a
    // retune of the shipping dead bands cannot make the twins drift apart
    // while both still pass.
    vec4 u_CoverageReactivity;
};

OloSurfaceHistoryRecord MakeRecord()
{
    OloSurfaceHistoryRecord record;
    record.LinearDepth = 5.0;
    record.GeometricNormal = vec3(0.0, 0.0, 1.0);
    record.ShadingNormal = vec3(0.0, 0.0, 1.0);
    record.Roughness = 0.4;
    record.MaterialClass = 3u;
    record.Coverage = 1.0;
    record.MaterialProfile = 0.0;
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
    settings.CoverageRejectThreshold = 0.5;
    settings.PixelSize = vec2(1.0 / 640.0, 1.0 / 360.0);
    return settings;
}

OloTemporalReactivitySettings MakeReactivitySettings()
{
    OloTemporalReactivitySettings settings;
    settings.MotionDeadZonePixels = 1.0;
    settings.MotionSaturationPixels = 5.0;
    settings.MotionMaxReactivity = 0.5;
    settings.CoverageNoiseDeadBand = 0.12;
    settings.CoverageSaturation = 0.35;
    settings.MaterialProfileDeadBand = 0.02;
    settings.MaterialProfileSaturation = 0.25;
    settings.PixelSize = vec2(1.0 / 1280.0, 1.0 / 720.0);
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

    // #1256 — the coverage channel and the separated reactive causes.
    //
    // A 0.7 coverage collapse is past the 0.5 hard threshold and must raise
    // the coverage bit and nothing else; the reactivity case then drives all
    // three causes PARTIALLY, so the product that combines them is actually
    // exercised rather than being multiplied by a zero.
    OloSurfaceHistoryRecord coverageCurrent = MakeRecord();
    OloSurfaceHistoryRecord coveragePrevious = MakeRecord();
    OloSurfaceHistorySettings coverageSettings = MakeSettings();
    coverageSettings.TestMask |= OLO_SURFACE_TEST_COVERAGE;
    coverageCurrent.Coverage = 0.8;
    coveragePrevious.Coverage = 0.1;
    uint coverageMismatch =
        OloEvaluateSurfaceHistory(coverageCurrent, coveragePrevious, vec2(0.5), true, coverageSettings);

    OloSurfaceHistoryRecord reactiveCurrent = MakeRecord();
    OloSurfaceHistoryRecord reactivePrevious = MakeRecord();
    reactiveCurrent.Coverage = 0.85;
    reactiveCurrent.MaterialProfile = 0.09;
    reactiveCurrent.Motion = vec2(0.0015, 0.0);
    OloTemporalReactivity reactivity =
        OloEvaluateTemporalReactivity(reactiveCurrent, reactivePrevious, MakeReactivitySettings());

    u_CoverageReactivity = vec4(float(coverageMismatch), reactivity.SurfaceMotion, reactivity.CoverageChange,
                                reactivity.MaterialChange);
}
