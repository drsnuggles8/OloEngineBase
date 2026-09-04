// =============================================================================
// WaterFoamCommon.glsl — the GLSL half of the foam-advection contract
// (issue #1034, §2.2).
//
// GLSL twin of OloEngine/src/OloEngine/Renderer/Water/WaterFoam.h. That header
// is the source of truth; read its comment block before changing anything here.
// The rules it states and this file must honour:
//
//   * the foam is a CHANNEL (.g) of the world-anchored, toroidally stored
//     disturbance field, on that field's lattice — this file does not define an
//     addressing scheme of its own, it calls WaterDisturbanceCommon.glsl's;
//   * an advection tap outside the window contributes ZERO, never the wrapped
//     value: the storage is a torus, so one texel past the edge is content from
//     hundreds of metres away, and advecting it in renders as a ghost of the
//     foam trailing the camera;
//   * the velocity is CLAMPED. It is a division by the frame's dt, so a single
//     stalled frame otherwise backtraces to somewhere unrelated.
//
// BOTH halves live here on purpose — the writer
// (compute/WaterDisturbance_Update.comp) and the reader (Water.glsl) include
// this same file, so there is exactly ONE GLSL text for the C++ header to be
// checked against rather than two that can drift apart unnoticed.
// =============================================================================

#ifndef WATER_FOAM_COMMON_GLSL
#define WATER_FOAM_COMMON_GLSL

#include "WaterDisturbanceCommon.glsl"

// Mirrors WaterFoam::kMaxVelocityMetresPerSecond.
const float WATER_FOAM_MAX_VELOCITY = 8.0;

// Mirrors WaterFoam::kFoamSaturationFold — the MEASURED fold a storm sea peaks
// at (0.345), not 1.0, which `saturate(1 - J)` never reaches on a plausible
// surface. Ramping to 1.0 leaves the deposit topped out around a third of its
// range and the foam permanently dim.
const float WATER_FOAM_SATURATION_FOLD = 0.35;

// Mirrors WaterFoam::ClampVelocity.
vec2 waterFoamClampVelocity(vec2 velocity)
{
    float speedSq = dot(velocity, velocity);
    if (speedSq <= WATER_FOAM_MAX_VELOCITY * WATER_FOAM_MAX_VELOCITY || speedSq <= 0.0)
        return velocity;
    return velocity * (WATER_FOAM_MAX_VELOCITY / sqrt(speedSq));
}

// Mirrors WaterFoam::SurfaceVelocity. `deltaSeconds <= 0` drops the orbital
// term rather than dividing by it — a paused editor ticks at dt 0, and a NaN
// velocity would poison the field permanently through the max() below.
vec2 waterFoamSurfaceVelocity(vec2 currentDisplacement, vec2 previousDisplacement,
                              vec2 windDrift, float deltaSeconds)
{
    vec2 orbital = vec2(0.0);
    if (deltaSeconds > 0.0)
        orbital = (currentDisplacement - previousDisplacement) / deltaSeconds;
    return waterFoamClampVelocity(orbital + windDrift);
}

// Mirrors WaterFoam::Backtrace.
vec2 waterFoamBacktrace(vec2 worldXZ, vec2 velocity, float deltaSeconds)
{
    return (deltaSeconds > 0.0) ? (worldXZ - velocity * deltaSeconds) : worldXZ;
}

// Mirrors WaterFoam::DepositFromFold. A ramp rather than a step: a step makes
// the foam boundary an exact iso-contour of the Jacobian, which on a smooth
// field is a smooth curve and reads as a drawn outline.
float waterFoamDepositFromFold(float fold, float threshold)
{
    float lo = clamp(threshold, 0.0, 0.99);
    float span = max(WATER_FOAM_SATURATION_FOLD - lo, 1e-3);
    return clamp((fold - lo) / span, 0.0, 1.0);
}

// -----------------------------------------------------------------------------
// The writer's half — where a bilinear read of the PREVIOUS field lands
// -----------------------------------------------------------------------------

// The four storage texels and weights a bilinear read at ABSOLUTE world XZ
// touches. Mirrors WaterFoam::BilinearTaps / WaterFoam::PrevTaps.
//
// ADDRESSING IS SEPARATE FROM FETCHING, and that split is not stylistic: under
// OLO_BINDLESS the compute pass's images are declared as LOCALS inside main()
// (BindlessHeap.glsl's OLO_HEAP_IMAGE), so a file-scope helper can neither see
// one nor take one as a parameter — a function parameter cannot carry the
// format layout qualifier imageLoad needs. Returning taps lets the shared code
// own the part that can be wrong (the toroidal addressing) while the imageLoad
// stays where the image is.
struct WaterFoamTaps
{
    ivec2 Storage[4];
    float Weight[4];
};

// A tap whose lattice texel is OUTSIDE the window gets weight 0 and storage
// (0,0) — in range, so the caller can fetch it unconditionally and multiply by
// zero. Out of window must NOT wrap: the storage is a torus, so one texel past
// the edge is content from the opposite edge, hundreds of metres away.
//
// The -0.5 is the half-texel convention: lattice texel `a` has its centre at
// (a + 0.5) * texelSize, so subtracting it puts integers on texel centres and
// makes floor() the correct lower tap. Dropping it shifts every advection step
// by half a texel, which accumulates into a drift indistinguishable from a
// wrong wind direction.
WaterFoamTaps waterFoamPrevTaps(vec2 absoluteWorldXZ, float texelSize, ivec2 latticeMin,
                                int resolution)
{
    WaterFoamTaps taps;
    vec2 g = absoluteWorldXZ / texelSize - 0.5;
    ivec2 base = ivec2(floor(g));
    vec2 frac = g - vec2(base);

    int index = 0;
    for (int dz = 0; dz <= 1; ++dz)
    {
        for (int dx = 0; dx <= 1; ++dx, ++index)
        {
            taps.Storage[index] = ivec2(0);
            taps.Weight[index] = 0.0;

            ivec2 lattice = base + ivec2(dx, dz);
            if (!waterDisturbanceWindowContains(lattice, latticeMin, resolution))
                continue;

            float wx = (dx == 0) ? (1.0 - frac.x) : frac.x;
            float wz = (dz == 0) ? (1.0 - frac.y) : frac.y;
            taps.Storage[index] = ivec2(waterDisturbanceWrapIndex(lattice.x, resolution),
                                        waterDisturbanceWrapIndex(lattice.y, resolution));
            taps.Weight[index] = wx * wz;
        }
    }
    return taps;
}

// -----------------------------------------------------------------------------
// The reader's half — sampling the advected field for shading
// -----------------------------------------------------------------------------

// Sample the advected foam density, edge fade included, at absolute world XZ.
//
// `params`  = (windowCentreX, windowCentreZ, invFieldExtent, intensity)
// `edgeFadeStart` is WaterDisturbance::kEdgeFadeStart, carried in
// u_WakeFieldParams2.z — the same window, so the same fade.
//
// `intensity <= 0` is the disabled state. There is no separate enable flag, so
// a frame in which the compute did not run cannot leave a stale foam field
// showing, and a scene with advection off reads exactly the sea it used to.
//
// Sampled with an explicit LOD 0, for the reason
// WaterDisturbanceCommon.glsl::sampleWaterDisturbance records: an implicit-LOD
// fetch inside a caller's branch takes a derivative in non-quad-uniform control
// flow, which is undefined and is the defect
// docs/agent-rules/water-shading-nyquist.md §2 was written about.
float sampleWaterAdvectedFoam(vec2 absoluteWorldXZ, sampler2D field, vec4 params,
                              float edgeFadeStart)
{
    if (params.w <= 0.0)
        return 0.0;

    float raw = textureLod(field, waterDisturbanceFieldUV(absoluteWorldXZ, params.z), 0.0).g;
    float fade = waterDisturbanceEdgeFade(absoluteWorldXZ, params.xy, params.z, edgeFadeStart);
    return clamp(raw, 0.0, 1.0) * fade * params.w;
}

#endif // WATER_FOAM_COMMON_GLSL
