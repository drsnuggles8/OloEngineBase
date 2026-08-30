// =============================================================================
// WaterWakeCommon.glsl - the GLSL half of the wake-shape contract (issue #968).
//
// GLSL twin of OloEngine/src/OloEngine/Renderer/Water/WaterWake.h. That header
// is the source of truth; read its comment block before changing anything here.
// Every function below mirrors one of the same name there, expression for
// expression, INCLUDING the `max(x, 1e-4)` guards rather than any isfinite-style
// form - non-finite inputs are rejected one layer up, in
// WaterWakeSystem::SubmitHull, precisely so these two can stay literal mirrors.
//
// Pinned by WaterWakeParityTest, which drives THIS text on the GPU (through
// tests/ShaderUnit_WaterWake.glsl, which includes this same file rather than
// copying it) against WaterWake::Evaluate on the CPU.
//
// ---- How a consumer supplies the hull records -------------------------------
//
// The records live in different buffers for different consumers - WaterUBO's
// `u_WakeHulls[]` in the water stages, an SSBO in the parity probe - so this
// file does not name a buffer. It declares the prototype
//
//     vec4 waterWakeFetch(int index);
//
// and the includer defines it. That is what keeps ONE copy of the walk over the
// records: an alternative where each consumer wrote its own loop would make the
// probe test its own loop rather than the production one, which is the exact
// shape of a test that passes while the shipped path is wrong.
// =============================================================================

#ifndef WATER_WAKE_COMMON_GLSL
#define WATER_WAKE_COMMON_GLSL

// The capsule falloff and its guards are shared with the #967 foam splats so
// the height ridge and the foam riding it cannot drift apart.
#include "WaterDisturbanceCommon.glsl"

// Mirrors WaterWake::kMaxHulls / kMaxArmSamples / the vec4 layout offsets.
#define WATER_WAKE_MAX_HULLS 4
#define WATER_WAKE_MAX_ARM_SAMPLES 8
#define WATER_WAKE_ARM_SAMPLE_VEC4 2
#define WATER_WAKE_VEC4_PER_HULL 20
#define WATER_WAKE_HULL_VEC4_COUNT 80

#define WATER_WAKE_OFFSET_CENTRE_FORWARD 0
#define WATER_WAKE_OFFSET_SHAPE 1
#define WATER_WAKE_OFFSET_BOUND 2
#define WATER_WAKE_OFFSET_CONTROL 3
#define WATER_WAKE_OFFSET_ARMS 4

// tan(19.47 deg) = 1 / (2 * sqrt(2)). Mirrors WaterWake::kKelvinTanHalfAngle.
const float WATER_WAKE_KELVIN_TAN_HALF_ANGLE = 0.35355339;
const float WATER_WAKE_FOOTPRINT_FADE_END = 1.35;
const float WATER_WAKE_ARM_SOFTNESS = 2.5;
const float WATER_WAKE_MAX_HEIGHT_METRES = 1.5;

// The includer MUST define this after including the file. See the header block.
vec4 waterWakeFetch(int index);

// Mirrors WaterWake::Bump. Exactly zero outside |u| >= 1, which is what keeps
// the whole ocean from carrying a millimetre offset that reads as the water
// plane having moved rather than as a wake.
float waterWakeBump(float u)
{
    float t = 1.0 - u * u;
    return (t <= 0.0) ? 0.0 : (t * t);
}

// Mirrors WaterWake::BandLimit. `vertexSpacing <= 0` means "no mesh" and
// disables the limit - that is the CPU/physics case, not a disabled feature.
float waterWakeBandLimit(float featureMetres, float vertexSpacing)
{
    if (!(vertexSpacing > 0.0))
        return 1.0;
    return clamp(2.0 * featureMetres / max(vertexSpacing, 1e-4) - 1.0, 0.0, 1.0);
}

// Mirrors WaterWake::ArmOffset - the Kelvin law. Not used by the evaluator
// below (the CPU has already placed the arm points), but kept here so the shader
// side of the contract states the law it was built from.
float waterWakeArmOffset(float halfBeam, float speed, float ageSeconds)
{
    return halfBeam + WATER_WAKE_KELVIN_TAN_HALF_ANGLE * abs(speed) * max(ageSeconds, 0.0);
}

// Mirrors WaterWake::Evaluate. Returns (heightMetres, flatten).
//
//   .x = metres to ADD to the surface height (signed; the stern trough is
//        negative), already clamped to WATER_WAKE_MAX_HEIGHT_METRES;
//   .y = how much of the base ocean displacement to REMOVE, in [0, 1].
//
// `heightScale <= 0` IS the disabled state - it zeroes the flatten too, so a
// frame that publishes the disabled form cannot leave a stale hull footprint
// pressed into the sea.
vec2 waterWakeEvaluate(float hullCount, float heightScale, float flattenStrength,
                       vec2 worldXZ, float vertexSpacing)
{
    if (!(heightScale > 0.0))
        return vec2(0.0);

    int live = clamp(int(hullCount), 0, WATER_WAKE_MAX_HULLS);
    float height = 0.0;
    float flatten = 0.0;

    for (int h = 0; h < live; ++h)
    {
        int base = h * WATER_WAKE_VEC4_PER_HULL;

        // Bounding-circle rejection first - the branch almost every sample on
        // an ocean takes. Anything ahead of it is paid for by the whole sea.
        vec4 bound = waterWakeFetch(base + WATER_WAKE_OFFSET_BOUND);
        vec2 toBound = worldXZ - bound.xy;
        if (dot(toBound, toBound) > bound.z * bound.z)
            continue;

        vec4 centreForward = waterWakeFetch(base + WATER_WAKE_OFFSET_CENTRE_FORWARD);
        vec4 shape = waterWakeFetch(base + WATER_WAKE_OFFSET_SHAPE);
        vec4 control = waterWakeFetch(base + WATER_WAKE_OFFSET_CONTROL);

        vec2 centre = centreForward.xy;
        vec2 forward = centreForward.zw;
        // forward rotated -90 deg about +Y. Getting this backwards mirrors the
        // wake, which on a symmetric hull looks entirely correct (issue #897).
        vec2 starboard = vec2(-forward.y, forward.x);

        vec2 rel = worldXZ - centre;
        float lateral = dot(rel, starboard);
        float along = dot(rel, forward);

        float halfBeam = max(shape.x, 1e-4);
        float halfLength = max(shape.y, 1e-4);

        // --- 1. Hull footprint suppression --------------------------------
        {
            float r = max(abs(lateral) / halfBeam, abs(along) / halfLength);
            float t = clamp((WATER_WAKE_FOOTPRINT_FADE_END - r) /
                                max(WATER_WAKE_FOOTPRINT_FADE_END - 1.0, 1e-4),
                            0.0, 1.0);
            float mask = t * t * (3.0 - 2.0 * t); // smoothstep
            flatten = max(flatten, mask * clamp(control.y, 0.0, 1.0) * clamp(flattenStrength, 0.0, 1.0));
        }

        // --- 2. Bow bump --------------------------------------------------
        if (shape.z > 0.0)
        {
            float reach = max(control.z, 1e-4);
            float lon = (along - halfLength) / reach;
            float lat = lateral / (halfBeam * 1.6);
            height += shape.z * waterWakeBump(lon) * waterWakeBump(lat) *
                      waterWakeBandLimit(reach, vertexSpacing);
        }

        // --- 3. Stern trough ----------------------------------------------
        // Centred one reach BEHIND the stern: a bump centred on the stern is
        // symmetric, so it would dig the same trough forward under the hull and
        // cancel the bow bump.
        if (shape.w > 0.0)
        {
            float reach = max(control.w, 1e-4);
            float lon = (along + halfLength + reach) / reach;
            float lat = lateral / (halfBeam * 1.4);
            height -= shape.w * waterWakeBump(lon) * waterWakeBump(lat) *
                      waterWakeBandLimit(reach, vertexSpacing);
        }

        // --- 4. The diverging arms ----------------------------------------
        // `max` across segments, not a sum: consecutive capsules overlap at
        // their shared endpoint, and summing there beads the arm at every
        // sample.
        {
            int samples = clamp(int(control.x), 0, WATER_WAKE_MAX_ARM_SAMPLES);
            float arm = 0.0;
            for (int i = 0; i + 1 < samples; ++i)
            {
                int s0 = base + WATER_WAKE_OFFSET_ARMS + WATER_WAKE_ARM_SAMPLE_VEC4 * i;
                int s1 = s0 + WATER_WAKE_ARM_SAMPLE_VEC4;

                vec4 starboard0 = waterWakeFetch(s0);
                vec4 starboard1 = waterWakeFetch(s1);
                vec4 port0 = waterWakeFetch(s0 + 1);
                vec4 port1 = waterWakeFetch(s1 + 1);

                float radius = max(starboard0.w, 1e-4);
                float amplitude = starboard0.z * waterWakeBandLimit(radius, vertexSpacing);
                if (!(amplitude > 0.0))
                    continue;

                float weightStarboard = waterDisturbanceSplatWeight(
                    worldXZ, starboard0.xy, starboard1.xy, radius, WATER_WAKE_ARM_SOFTNESS);
                float weightPort = waterDisturbanceSplatWeight(
                    worldXZ, port0.xy, port1.xy, radius, WATER_WAKE_ARM_SOFTNESS);
                arm = max(arm, amplitude * max(weightStarboard, weightPort));
            }
            height += arm;
        }
    }

    height *= heightScale;
    return vec2(clamp(height, -WATER_WAKE_MAX_HEIGHT_METRES, WATER_WAKE_MAX_HEIGHT_METRES),
                clamp(flatten, 0.0, 1.0));
}

// Surface normal perturbation for the wake, by central differences of the height
// above. The renderer MUST call this wherever it applies waterWakeEvaluate to a
// vertex: a displacement whose normal does not carry the same factor shades as
// flat water with a bulge in it, which is docs/agent-rules/
// water-shading-nyquist.md's first rule and the reason a correct displacement
// can still look wrong.
//
// `baseNormal` is the ocean normal before the wake; the result is renormalised.
// Central rather than forward differences so the gradient is symmetric about the
// vertex - a forward difference biases every ridge half an epsilon downstream,
// which on a symmetric V shows up as one arm reading brighter than the other.
vec3 waterWakePerturbNormal(vec3 baseNormal, float hullCount, float heightScale,
                            vec2 worldXZ, float vertexSpacing, float epsilonMetres)
{
    float e = max(epsilonMetres, 1e-3);
    // Flatten strength is irrelevant to the HEIGHT, which is all the gradient
    // needs, so pass 0 rather than threading the setting through.
    float hxp = waterWakeEvaluate(hullCount, heightScale, 0.0, worldXZ + vec2(e, 0.0), vertexSpacing).x;
    float hxm = waterWakeEvaluate(hullCount, heightScale, 0.0, worldXZ - vec2(e, 0.0), vertexSpacing).x;
    float hzp = waterWakeEvaluate(hullCount, heightScale, 0.0, worldXZ + vec2(0.0, e), vertexSpacing).x;
    float hzm = waterWakeEvaluate(hullCount, heightScale, 0.0, worldXZ - vec2(0.0, e), vertexSpacing).x;

    float dhdx = (hxp - hxm) / (2.0 * e);
    float dhdz = (hzp - hzm) / (2.0 * e);

    // Convert the base normal to slope form, add the wake's slope, convert back.
    // Doing it in slope space rather than adding normals is what makes the two
    // contributions compose correctly when the base sea is already steep.
    float by = max(abs(baseNormal.y), 1e-4);
    vec3 n = vec3(baseNormal.x / by - dhdx, 1.0, baseNormal.z / by - dhdz);
    float lenSq = dot(n, n);
    return (lenSq > 1e-12) ? normalize(n) : vec3(0.0, 1.0, 0.0);
}

#endif // WATER_WAKE_COMMON_GLSL
