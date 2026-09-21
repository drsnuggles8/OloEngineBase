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
const uint OLO_SURFACE_REJECT_COVERAGE         = 1u << 15u;

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
const uint OLO_SURFACE_TEST_COVERAGE         = 1u << 8u;

struct OloSurfaceHistoryRecord
{
    float LinearDepth;
    vec3 GeometricNormal;
    vec3 ShadingNormal;
    float Roughness;
    uint MaterialClass;
    // Twins of SurfaceHistoryRecord::Coverage / ::MaterialProfile. Coverage
    // is the fraction of the pixel the subject occupies (1 for an opaque
    // surface); MaterialProfile is the continuous profile axis the discrete
    // Material / MaterialClass identities cannot see.
    float Coverage;
    float MaterialProfile;
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
    float CoverageRejectThreshold;
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

    bool currentHasHit = (current.Flags & OLO_SURFACE_FLAG_HAS_HIT_DISTANCE) != 0u;
    bool previousHasHit = (previous.Flags & OLO_SURFACE_FLAG_HAS_HIT_DISTANCE) != 0u;
    if (!OloSurfaceFinite(current.LinearDepth) || !OloSurfaceFinite(previous.LinearDepth) ||
        ((settings.TestMask & OLO_SURFACE_TEST_GEOMETRIC_NORMAL) != 0u &&
         (!OloSurfaceFinite(current.GeometricNormal) || !OloSurfaceFinite(previous.GeometricNormal))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_SHADING_NORMAL) != 0u &&
         (!OloSurfaceFinite(current.ShadingNormal) || !OloSurfaceFinite(previous.ShadingNormal))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_ROUGHNESS) != 0u &&
         (!OloSurfaceFinite(current.Roughness) || !OloSurfaceFinite(previous.Roughness))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_MOTION) != 0u && !OloSurfaceFinite(current.Motion)) ||
        ((settings.TestMask & OLO_SURFACE_TEST_COVERAGE) != 0u &&
         (!OloSurfaceFinite(current.Coverage) || !OloSurfaceFinite(previous.Coverage))) ||
        ((settings.TestMask & OLO_SURFACE_TEST_HIT_DISTANCE) != 0u &&
         ((currentHasHit && !OloSurfaceFinite(current.HitDistance)) ||
          (previousHasHit && !OloSurfaceFinite(previous.HitDistance)))))
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
    if ((settings.TestMask & OLO_SURFACE_TEST_COVERAGE) != 0u &&
        abs(current.Coverage - previous.Coverage) > settings.CoverageRejectThreshold)
        reasons |= OLO_SURFACE_REJECT_COVERAGE;

    vec2 motionPixels = current.Motion / max(settings.PixelSize, vec2(1.0e-8));
    if ((settings.TestMask & OLO_SURFACE_TEST_MOTION) != 0u &&
        dot(motionPixels, motionPixels) > settings.MotionThresholdPixels * settings.MotionThresholdPixels)
        reasons |= OLO_SURFACE_REJECT_MOTION;
    if ((current.Flags & OLO_SURFACE_FLAG_REACTIVE) != 0u)
        reasons |= OLO_SURFACE_REJECT_REACTIVE;
    if ((current.Flags & OLO_SURFACE_FLAG_DISOCCLUDED) != 0u)
        reasons |= OLO_SURFACE_REJECT_DISOCCLUDED;

    if ((settings.TestMask & OLO_SURFACE_TEST_HIT_DISTANCE) != 0u && currentHasHit != previousHasHit)
        reasons |= OLO_SURFACE_REJECT_HIT_DISTANCE;
    else if ((settings.TestMask & OLO_SURFACE_TEST_HIT_DISTANCE) != 0u && currentHasHit &&
             OloSurfaceRelativeDifference(current.HitDistance, previous.HitDistance) > settings.RelativeHitDistanceThreshold)
        reasons |= OLO_SURFACE_REJECT_HIT_DISTANCE;
    return reasons;
}

// ---------------------------------------------------------------------------
// Reactive data: the three causes, kept apart (issue #1256)
//
// GLSL twin of TemporalReactivity / EvaluateTemporalReactivity in
// Renderer/SurfaceHistory.h. The C++ header carries the full rationale — why
// three scalars rather than one, why the coverage term needs a dead band (a
// stochastic strand estimator moves its coverage every frame BY CONSTRUCTION
// and averaging that away is the resolve's whole job), and why the three
// survival fractions multiply instead of taking a max. Keep the two in step;
// ShaderUnitTests pins the agreement.
// ---------------------------------------------------------------------------
struct OloTemporalReactivity
{
    float SurfaceMotion;
    float CoverageChange;
    float MaterialChange;
};

struct OloTemporalReactivitySettings
{
    float MotionDeadZonePixels;
    float MotionSaturationPixels;
    // Twin of TemporalReactivitySettings::MotionMaxReactivity — the 0.5
    // OloTemporalMotionFeedback already floors feedback at. Applies to the
    // ramp only; a non-finite motion still yields full reactivity.
    float MotionMaxReactivity;
    float CoverageNoiseDeadBand;
    float CoverageSaturation;
    float MaterialProfileDeadBand;
    float MaterialProfileSaturation;
    vec2 PixelSize;
};

// Linear ramp from 0 at `deadBand` to 1 at `saturation`, clamped. A
// non-finite magnitude reads as fully reactive: with the signal itself
// broken, the one safe answer is to keep no history.
float OloReactiveRamp(float magnitude, float deadBand, float saturation)
{
    if (!OloSurfaceFinite(magnitude))
        return 1.0;
    float span = max(saturation - deadBand, 1.0e-4);
    return clamp((magnitude - deadBand) / span, 0.0, 1.0);
}

float OloTemporalConfidence(OloTemporalReactivity reactivity)
{
    return (1.0 - reactivity.SurfaceMotion) * (1.0 - reactivity.CoverageChange) *
           (1.0 - reactivity.MaterialChange);
}

float OloTemporalCombinedReactivity(OloTemporalReactivity reactivity)
{
    return 1.0 - OloTemporalConfidence(reactivity);
}

OloTemporalReactivity OloEvaluateTemporalReactivity(OloSurfaceHistoryRecord current,
                                                    OloSurfaceHistoryRecord previous,
                                                    OloTemporalReactivitySettings settings)
{
    vec2 motionPixels = current.Motion / max(settings.PixelSize, vec2(1.0e-8));

    OloTemporalReactivity reactivity;
    // Tested before the length() rather than after: a non-finite motion must
    // read as fully reactive, and synthesising a NaN to push through the ramp
    // would be a constant division the compiler is free to fold away.
    reactivity.SurfaceMotion =
        OloSurfaceFinite(motionPixels)
            ? min(OloReactiveRamp(length(motionPixels), settings.MotionDeadZonePixels,
                                  settings.MotionSaturationPixels),
                  clamp(settings.MotionMaxReactivity, 0.0, 1.0))
            : 1.0;
    reactivity.CoverageChange = OloReactiveRamp(abs(current.Coverage - previous.Coverage),
                                                settings.CoverageNoiseDeadBand, settings.CoverageSaturation);
    reactivity.MaterialChange = OloReactiveRamp(abs(current.MaterialProfile - previous.MaterialProfile),
                                                settings.MaterialProfileDeadBand,
                                                settings.MaterialProfileSaturation);
    return reactivity;
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
