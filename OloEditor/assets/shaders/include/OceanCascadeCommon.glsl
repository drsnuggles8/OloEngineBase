// ============================================================================
// OceanCascadeCommon.glsl — the GPU half of the FFT ocean cascade sum
// (issue #969; docs/design/water-ocean.md §1.3).
//
// CPU TWIN: OceanFFTField::SampleCascades (Renderer/Ocean/OceanFFTField.cpp).
// The contract both obey — including which SPACE the argument is in, which is
// the half a same-point parity test cannot check — is written out once in
// Renderer/Ocean/OceanCascades.h. Read it before changing anything here.
//
//   THE ARGUMENT IS ABSOLUTE WORLD XZ. Camera-relative positions must have
//   u_RenderOrigin.xz added back first, and a vertex must be evaluated at the
//   position it ends up at, not the one it was authored at.
//
// This file is INCLUDED by the water vertex, tessellation-evaluation and
// fragment stages rather than copied into each. A test-only copy of a
// production function passes about the copy while the shipped shader does
// something else (docs/agent-rules/cpu-gpu-surface-parity.md §3), and three
// production copies fail the same way with nobody watching at all.
//
// The texture fetches go through a HOOK, for the same reason WaterWakeCommon
// takes its records through one: each stage declares the cascade samplers with
// its own bindless macro, and one walk over the cascades shared between them
// beats three walks that can disagree.
// ============================================================================

#ifndef OLO_OCEAN_CASCADE_COMMON_GLSL
#define OLO_OCEAN_CASCADE_COMMON_GLSL

// Maximum bands the preset can carry. Mirrors Ocean::kMaxOceanCascades.
#define OLO_OCEAN_MAX_CASCADES 3

// Hooks each including stage defines over its own sampler2DArray declarations.
// `layer` is the cascade index; `uv` is already in that cascade's tile space.
vec4 oceanCascadeFetchDisplacement(vec2 uv, int layer);
vec4 oceanCascadeFetchDerivatives(vec2 uv, int layer);

struct OceanCascadeSample
{
    vec3 Displacement; // (dx, height, dz) metres, world axes
    vec2 Slope;        // (dh/dx, dh/dz) summed, world axes
    float Foam;        // saturated sum of per-band folding
};

// Rotate `v` by the angle whose cosine/sine are `cs`. Negate cs.y for the
// inverse. Mirrors Ocean::RotateVec2.
vec2 oceanRotate(vec2 v, vec2 cs)
{
    return vec2(cs.x * v.x - cs.y * v.y, cs.y * v.x + cs.x * v.y);
}

// Sum the active cascades at absolute world XZ.
//
//   fftParams     — u_FFTParams:        x = cascade count (0 = FFT off),
//                                       y = 1 / L0 (broad tile), z/w unused here
//   cascadeParams — u_FFTCascadeParams: x = 1 / L1, y = 1 / L2,
//                                       z = cos(theta_mid), w = sin(theta_mid)
//
// Only the MID cascade is rotated — non-commensurate tiles kill the period, the
// rotation kills the shared axis, and one rotated domain is enough for both
// (Ocean/OceanCascades.h point 3). Rotating more would cost another UBO vec4
// and buy nothing the eye can find.
OceanCascadeSample sampleOceanCascades(vec2 worldXZ, vec4 fftParams, vec4 cascadeParams)
{
    OceanCascadeSample result;
    result.Displacement = vec3(0.0);
    result.Slope = vec2(0.0);
    result.Foam = 0.0;

    int count = int(fftParams.x + 0.5);
    // Per-cascade tile scales. Cascade 0 keeps the slot the single-cascade path
    // always used (u_FFTParams.y), so nothing that reads the old field changed.
    vec3 invPatch = vec3(fftParams.y, cascadeParams.x, cascadeParams.y);

    for (int i = 0; i < OLO_OCEAN_MAX_CASCADES; ++i)
    {
        if (i >= count)
            break;

        vec2 rot = (i == 1) ? cascadeParams.zw : vec2(1.0, 0.0);
        // Rotate INTO the band's sampling domain, then scale to its tile.
        vec2 uv = oceanRotate(worldXZ, rot) * invPatch[i];

        vec4 disp = oceanCascadeFetchDisplacement(uv, i);
        vec4 deriv = oceanCascadeFetchDerivatives(uv, i);

        result.Displacement.y += disp.y;

        // ...and rotate the vector quantities BACK out of it. The displacement
        // and the height gradient are both expressed in the band's axes, and
        // rotating one without the other leans the crests the wrong way.
        vec2 rotInv = vec2(rot.x, -rot.y);
        vec2 horizontal = oceanRotate(disp.xz, rotInv);
        result.Displacement.x += horizontal.x;
        result.Displacement.z += horizontal.y;

        // Slopes add; normals do not. -n.xz / n.y is exact against
        // Ocean_Assemble.comp's normalize(vec3(-sx, 1.0, -sz)).
        float ny = (abs(deriv.y) > 1e-5) ? deriv.y : 1.0;
        result.Slope += oceanRotate(vec2(-deriv.x / ny, -deriv.z / ny), rotInv);

        result.Foam += disp.w;
    }

    result.Foam = clamp(result.Foam, 0.0, 1.0);
    return result;
}

// The summed surface normal. Built from the summed SLOPE, which is the only
// way three bands' shading can describe the geometry three bands actually made
// — averaging their unit normals describes none of them
// (docs/agent-rules/water-shading-nyquist.md §1: a derived normal must carry
// every factor its displacement carries).
vec3 oceanCascadeNormal(OceanCascadeSample s)
{
    return normalize(vec3(-s.Slope.x, 1.0, -s.Slope.y));
}

#endif // OLO_OCEAN_CASCADE_COMMON_GLSL
