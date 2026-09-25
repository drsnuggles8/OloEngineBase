#ifndef SNOW_DIFFUSION_COMMON_GLSL
#define SNOW_DIFFUSION_COMMON_GLSL

// =============================================================================
// SnowDiffusionCommon.glsl — the snow half of the diffusion hand-off lane,
// issue #1451.
//
// Standalone ON PURPOSE, like SkinDiffusionCommon.glsl beside it: the producers
// (every lit pass that shades snow) and the consumer (SSS_Blur.glsl, a
// fullscreen pass that wants no UBO declarations) only have to agree about the
// encoding, so the encoding is the only thing in here.
//
// ONE LANE, TWO DISJOINT RANGES. Scene attachment 4 (RGBA16F) carries the
// DIFFUSE half of a pixel's lighting in .rgb and, in .a, who should blur it:
//
//   a == 0          nothing — every surface that neither diffuses nor snows
//   a == (s+1)/8    skin profile slot s in [0, 6] (SkinDiffusionCommon.glsl)
//   a <  0          snow, with weight -a in (0, 1]
//
// The ranges are disjoint, so each consumer decodes its own and reads the other
// as "not mine": oloSkinDiffusionSlot maps every a <= 0 to "no profile", and
// oloSnowDiffusionWeight below maps every a >= 0 to "no snow".
//
// WHY THIS IS NOT THE "LAST WRITER WINS" CHANNEL ADR 0024 REJECTED. That was
// scene-colour alpha, which every writer — skin, snow, blended glass, opaque
// PBR — writes for its own reason, so the value a consumer read depended on
// whichever ran last. This lane has ONE writer per pixel for this quantity:
// the surface shader, which decides per pixel whether it hands over skin or
// snow (a snow-covered skin pixel hands over snow — the snow layer replaced the
// skin's diffuse), and a partitioned value range means the decision is visible
// in the value itself. See docs/adr/0024-material-kind-is-not-the-closure-version.md.
//
// A half float stores -w for w in (0, 1] with an 11-bit mantissa, so the weight
// round-trips to within 2^-11 of what the producer wrote.
// =============================================================================

// Below this a pixel is not snow, on both sides of the hand-off: the producers
// treat a weight at or under it as "no snow layer" and the consumer as "no
// blur". The same number the forward shaders always gated the overlay on.
#define OLO_SNOW_MIN_WEIGHT 0.001

// The lane value a snow pixel of weight `weight` hands over: -weight, so it can
// never be mistaken for a skin slot or for the cleared 0.
float oloSnowDiffusionEncodeWeight(float weight)
{
    return -clamp(weight, 0.0, 1.0);
}

// The snow weight an aux texel names, 0 when it names none (a skin slot, the
// cleared 0, or anything a transparent over the surface blended in).
float oloSnowDiffusionWeight(float encoded)
{
    return clamp(-encoded, 0.0, 1.0);
}

#endif // SNOW_DIFFUSION_COMMON_GLSL
