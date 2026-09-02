// Shared surface-history validity and signal-generic moment contract (#976).
// Keep rejection bit values in lock-step with Renderer/SurfaceHistory.h.
#ifndef OLO_SURFACE_HISTORY_GLSL
#define OLO_SURFACE_HISTORY_GLSL

const uint OLO_SURFACE_REJECT_NONE             = 0u;
const uint OLO_SURFACE_REJECT_NO_HISTORY       = 1u << 0u;
const uint OLO_SURFACE_REJECT_OFF_SCREEN       = 1u << 1u;
const uint OLO_SURFACE_REJECT_NON_FINITE       = 1u << 2u;
const uint OLO_SURFACE_REJECT_DEPTH            = 1u << 3u;
const uint OLO_SURFACE_REJECT_GEOMETRIC_NORMAL = 1u << 4u;
const uint OLO_SURFACE_REJECT_SHADING_NORMAL   = 1u << 5u;
const uint OLO_SURFACE_REJECT_INSTANCE         = 1u << 6u;
const uint OLO_SURFACE_REJECT_PRIMITIVE        = 1u << 7u;
const uint OLO_SURFACE_REJECT_MATERIAL         = 1u << 8u;
const uint OLO_SURFACE_REJECT_ROUGHNESS        = 1u << 9u;
const uint OLO_SURFACE_REJECT_MOTION           = 1u << 10u;
const uint OLO_SURFACE_REJECT_REACTIVE         = 1u << 11u;
const uint OLO_SURFACE_REJECT_DISOCCLUDED      = 1u << 12u;
const uint OLO_SURFACE_REJECT_HIT_DISTANCE     = 1u << 13u;
const uint OLO_SURFACE_REJECT_IDENTITY_MISSING = 1u << 14u;

const uint OLO_SURFACE_FLAG_REACTIVE         = 1u << 0u;
const uint OLO_SURFACE_FLAG_DISOCCLUDED      = 1u << 1u;
const uint OLO_SURFACE_FLAG_HAS_HIT_DISTANCE = 1u << 2u;

const uint OLO_SURFACE_TEST_GEOMETRIC_NORMAL = 1u << 0u;
const uint OLO_SURFACE_TEST_SHADING_NORMAL   = 1u << 1u;
const uint OLO_SURFACE_TEST_INSTANCE         = 1u << 2u;
const uint OLO_SURFACE_TEST_PRIMITIVE        = 1u << 3u;
const uint OLO_SURFACE_TEST_MATERIAL         = 1u << 4u;
const uint OLO_SURFACE_TEST_ROUGHNESS        = 1u << 5u;
const uint OLO_SURFACE_TEST_MOTION           = 1u << 6u;
const uint OLO_SURFACE_TEST_HIT_DISTANCE     = 1u << 7u;

struct OloSurfaceHistoryRecord
{
    float LinearDepth;
    vec3 GeometricNormal;
    vec3 ShadingNormal;
    float Roughness;
    uint MaterialClass;
    vec2 Motion;
    uvec2 Instance;
    uvec2 Primitive;
    uvec2 Material;
    uint Flags;
    float HitDistance;
    uint PrimitiveLocalIndex;
};

struct OloSurfaceHistorySettings
{
    uint TestMask;
    float RelativeDepthThreshold;
    float GeometricNormalCosineThreshold;
    float ShadingNormalCosineThreshold;
    float RoughnessThreshold;
    float MotionThresholdPixels;
    float RelativeHitDistanceThreshold;
    vec2 PixelSize;
};

bool OloSurfaceFinite(float value) { return !isnan(value) && !isinf(value); }
bool OloSurfaceFinite(vec2 value) { return !any(isnan(value)) && !any(isinf(value)); }
bool OloSurfaceFinite(vec3 value) { return !any(isnan(value)) && !any(isinf(value)); }
bool OloSurfaceIdentityValid(uvec2 value) { return value.x != 0xffffffffu && value.y != 0u; }

float OloSurfaceRelativeDifference(float lhs, float rhs)
{
    return abs(lhs - rhs) / max(max(abs(lhs), abs(rhs)), 1.0e-4);
}

float OloSurfaceNormalCosine(vec3 lhs, vec3 rhs)
{
    float product = dot(lhs, lhs) * dot(rhs, rhs);
    return product > 1.0e-12 ? dot(lhs, rhs) * inversesqrt(product) : -1.0;
}

uint OloEvaluateSurfaceHistory(OloSurfaceHistoryRecord current,
                               OloSurfaceHistoryRecord previous,
                               vec2 reprojectedUV,
                               bool historyAvailable,
                               OloSurfaceHistorySettings settings)
{
    uint reasons = OLO_SURFACE_REJECT_NONE;
    if (!historyAvailable)
        reasons |= OLO_SURFACE_REJECT_NO_HISTORY;
    if (!OloSurfaceFinite(reprojectedUV) || any(lessThan(reprojectedUV, vec2(0.0))) ||
        any(greaterThan(reprojectedUV, vec2(1.0))))
        reasons |= OLO_SURFACE_REJECT_OFF_SCREEN;

    if (!OloSurfaceFinite(current.LinearDepth) || !OloSurfaceFinite(previous.LinearDepth) ||
        ((settings.TestMask & OLO_SURFACE_TEST_GEOMETRIC_NORMAL) != 0u &&
         (!OloSurfaceFinite(current.GeometricNormal) || !OloSurfaceFinite(previous.GeometricNormal))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_SHADING_NORMAL) != 0u &&
         (!OloSurfaceFinite(current.ShadingNormal) || !OloSurfaceFinite(previous.ShadingNormal))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_ROUGHNESS) != 0u &&
         (!OloSurfaceFinite(current.Roughness) || !OloSurfaceFinite(previous.Roughness))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_MOTION) != 0u && !OloSurfaceFinite(current.Motion)) ||
        ((settings.TestMask & OLO_SURFACE_TEST_HIT_DISTANCE) != 0u &&
         (!OloSurfaceFinite(current.HitDistance) || !OloSurfaceFinite(previous.HitDistance))))
        reasons |= OLO_SURFACE_REJECT_NON_FINITE;

    if (OloSurfaceRelativeDifference(current.LinearDepth, previous.LinearDepth) > settings.RelativeDepthThreshold)
        reasons |= OLO_SURFACE_REJECT_DEPTH;
    if ((settings.TestMask & OLO_SURFACE_TEST_GEOMETRIC_NORMAL) != 0u &&
        OloSurfaceNormalCosine(current.GeometricNormal, previous.GeometricNormal) < settings.GeometricNormalCosineThreshold)
        reasons |= OLO_SURFACE_REJECT_GEOMETRIC_NORMAL;
    if ((settings.TestMask & OLO_SURFACE_TEST_SHADING_NORMAL) != 0u &&
        OloSurfaceNormalCosine(current.ShadingNormal, previous.ShadingNormal) < settings.ShadingNormalCosineThreshold)
        reasons |= OLO_SURFACE_REJECT_SHADING_NORMAL;

    bool testedIdentityUnavailable =
        ((settings.TestMask & OLO_SURFACE_TEST_INSTANCE) != 0u &&
         (!OloSurfaceIdentityValid(current.Instance) || !OloSurfaceIdentityValid(previous.Instance))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_PRIMITIVE) != 0u &&
         (!OloSurfaceIdentityValid(current.Primitive) || !OloSurfaceIdentityValid(previous.Primitive))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_MATERIAL) != 0u &&
         (!OloSurfaceIdentityValid(current.Material) || !OloSurfaceIdentityValid(previous.Material)));
    if (testedIdentityUnavailable)
        reasons |= OLO_SURFACE_REJECT_IDENTITY_MISSING;
    if ((settings.TestMask & OLO_SURFACE_TEST_INSTANCE) != 0u && any(notEqual(current.Instance, previous.Instance)))
        reasons |= OLO_SURFACE_REJECT_INSTANCE;
    if ((settings.TestMask & OLO_SURFACE_TEST_PRIMITIVE) != 0u &&
        (any(notEqual(current.Primitive, previous.Primitive)) || current.PrimitiveLocalIndex != previous.PrimitiveLocalIndex))
        reasons |= OLO_SURFACE_REJECT_PRIMITIVE;
    if ((settings.TestMask & OLO_SURFACE_TEST_MATERIAL) != 0u &&
        (any(notEqual(current.Material, previous.Material)) || current.MaterialClass != previous.MaterialClass))
        reasons |= OLO_SURFACE_REJECT_MATERIAL;
    if ((settings.TestMask & OLO_SURFACE_TEST_ROUGHNESS) != 0u &&
        abs(current.Roughness - previous.Roughness) > settings.RoughnessThreshold)
        reasons |= OLO_SURFACE_REJECT_ROUGHNESS;

    vec2 motionPixels = current.Motion / max(settings.PixelSize, vec2(1.0e-8));
    if ((settings.TestMask & OLO_SURFACE_TEST_MOTION) != 0u &&
        dot(motionPixels, motionPixels) > settings.MotionThresholdPixels * settings.MotionThresholdPixels)
        reasons |= OLO_SURFACE_REJECT_MOTION;
    if ((current.Flags & OLO_SURFACE_FLAG_REACTIVE) != 0u)
        reasons |= OLO_SURFACE_REJECT_REACTIVE;
    if ((current.Flags & OLO_SURFACE_FLAG_DISOCCLUDED) != 0u)
        reasons |= OLO_SURFACE_REJECT_DISOCCLUDED;

    bool currentHasHit = (current.Flags & OLO_SURFACE_FLAG_HAS_HIT_DISTANCE) != 0u;
    bool previousHasHit = (previous.Flags & OLO_SURFACE_FLAG_HAS_HIT_DISTANCE) != 0u;
    if ((settings.TestMask & OLO_SURFACE_TEST_HIT_DISTANCE) != 0u && currentHasHit != previousHasHit)
        reasons |= OLO_SURFACE_REJECT_HIT_DISTANCE;
    else if ((settings.TestMask & OLO_SURFACE_TEST_HIT_DISTANCE) != 0u && currentHasHit &&
             OloSurfaceRelativeDifference(current.HitDistance, previous.HitDistance) > settings.RelativeHitDistanceThreshold)
        reasons |= OLO_SURFACE_REJECT_HIT_DISTANCE;
    return reasons;
}

struct OloTemporalMoments
{
    vec4 First;
    vec4 Second;
    float HistoryLength;
};

OloTemporalMoments OloAccumulateTemporalMoments(vec4 current, OloTemporalMoments previous,
                                                 bool historyAccepted, float maximumHistoryLength)
{
    if (!historyAccepted)
    {
        OloTemporalMoments reset;
        reset.First = current;
        reset.Second = current * current;
        reset.HistoryLength = 1.0;
        return reset;
    }
    float previousLength = max(previous.HistoryLength, 0.0);
    float length = min(previousLength + 1.0, max(maximumHistoryLength, 1.0));
    float currentWeight = 1.0 / length;
    float historyWeight = 1.0 - currentWeight;
    OloTemporalMoments result;
    result.First = previous.First * historyWeight + current * currentWeight;
    result.Second = previous.Second * historyWeight + current * current * currentWeight;
    result.HistoryLength = length;
    return result;
}

vec4 OloTemporalVariance(OloTemporalMoments moments)
{
    return max(moments.Second - moments.First * moments.First, vec4(0.0));
}

#endif // OLO_SURFACE_HISTORY_GLSL
