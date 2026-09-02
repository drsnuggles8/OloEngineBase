// =============================================================================
// SkyDepth.glsl — the one sky test for conventional depth (issue #1008)
//
// A cleared depth buffer is NOT guaranteed to read back as exactly 1.0. On
// AMD (Navi 10, radeonsi) a cleared DEPTH24_STENCIL8 sampled through a
// sampler2D reads 0.99999994, one fp32 ULP under 1.0, on every sky pixel;
// NVIDIA reads 1.0. Every `depth < 1.0` / `depth >= 1.0` sky test therefore
// sees geometry at the far plane on AMD. In the cloud raymarch that clamped
// every horizon ray at 4 km and cut the deck off at a fixed elevation.
//
// The epsilon is 1e-6: ~17 quanta of a 24-bit buffer, so a whole ULP of slop
// on either side still lands on the sky side, while the farthest real
// geometry this repo renders (Drift's ridge, ~0.999904) stays ~1600 quanta
// away on the geometry side. Do not widen it toward the 1e-4 the fog pass
// once used; that swallows real far geometry.
// =============================================================================

#ifndef OLO_SKY_DEPTH_GLSL
#define OLO_SKY_DEPTH_GLSL

const float kOloSkyDepthEpsilon = 1.0e-6;

bool oloDepthIsSky(float depth)
{
    return depth >= 1.0 - kOloSkyDepthEpsilon;
}

#endif // OLO_SKY_DEPTH_GLSL
