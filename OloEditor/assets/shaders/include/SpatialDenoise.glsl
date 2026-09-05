// =============================================================================
// SpatialDenoise.glsl — the guided spatial filters of the SSGI / SSR denoiser
// chain (issue #708)
//
// This header owns the WEIGHTS and the SAMPLE PATTERN of the chain's three
// spatial stages; it deliberately owns no texture declarations and no taps, so
// the same math serves a half-resolution pre-blur, a half-resolution post-blur
// and the full-resolution guided upscale without any of them having to agree on
// a binding layout.
//
//   pre-blur   — small radius, depth + normal guided, runs BEFORE the temporal
//                resolve so the history accumulates something already stable.
//   post-blur  — variance and history-length guided radius, runs AFTER it: wide
//                where the estimate is still noisy or freshly disoccluded,
//                narrow where it converged.
//   upscale    — a 3x3 tent over the half-resolution result, with the same
//                plane/normal weights rejecting taps from other surfaces.
//
// Derived from the Denoisinator in Timberdoodle
// (D:\repos\Timberdoodle\src\rendering\rtgi, Apache-2.0): the Poisson-disc set,
// the golden-angle per-frame rotation and the plane-distance/normal weight pair
// are its constructions, ported from HLSL compute to GLSL fullscreen draws.
// Two deliberate departures are noted at their functions: the radius guide, and
// the geometry weight's tolerance.
//
// WHY PLANE DISTANCE AND NOT A DEPTH DIFFERENCE. A tap on the same flat floor
// as the centre pixel, seen at a grazing angle, differs in view depth by metres
// while lying exactly in the centre pixel's plane — it is the single most
// valuable tap there is and a depth-difference test throws it away. Measuring
// the tap's distance ALONG THE CENTRE NORMAL keeps it and still rejects the
// wall behind it. That is why every geometry weight here needs a normal.
//
// The whole file is mirrored on the CPU by ScreenSpaceDenoiseMathTest, which is
// what pins these formulas without a GL context.
// =============================================================================

#ifndef OLO_SPATIAL_DENOISE_GLSL
#define OLO_SPATIAL_DENOISE_GLSL

// -----------------------------------------------------------------------------
// The sky sentinel
// -----------------------------------------------------------------------------
// The view depth the trace stores in its signal alpha for a pixel with no
// surface. Every stage of the chain must agree on this exact value, which is
// why it lives here and not in each shader: a pre-blur that treated the sky as
// an ordinary surface would drag the black sky into the silhouette of every
// object against it, and a post-blur using a different threshold than the
// pre-blur would put the seam somewhere else again.
//
// 60000 and not a round 1e6 because the value travels through RGBA16F targets
// and half-float tops out at 65504; a larger sentinel would be stored as +Inf
// and poison every weight that touched it. 60000 is exactly representable in
// half (1875 x 32) and far beyond any real view distance.
const float OLO_DENOISE_SKY_VIEW_DEPTH = 60000.0;

bool OloDenoiseIsSky(float viewDepth)
{
    // Not an equality test: the sentinel round-trips through a half-float
    // attachment, and `>= sentinel` is also FALSE for a NaN, which is the
    // conservative answer for a depth that has already gone wrong.
    return !(viewDepth < OLO_DENOISE_SKY_VIEW_DEPTH);
}

// -----------------------------------------------------------------------------
// The sample pattern
// -----------------------------------------------------------------------------
// An 8-point Poisson disc (Timberdoodle's g_Poisson8, xy only — its z is a
// precomputed radius this chain does not use). Poisson rather than a box or a
// Gaussian grid because at 8 taps a grid aliases into a visible cross and a
// disc does not; the points are near-uniform over the unit disc with a minimum
// separation, so no two taps are redundant.
//
// Eight and not more: this pattern is applied twice per frame (pre and post)
// and the temporal resolve between them averages over the per-frame rotation
// below, so the effective footprint is far larger than eight taps of one frame.
const vec2 OLO_DENOISE_POISSON_8[8] = vec2[8](
    vec2(-0.4706069, -0.4427112),
    vec2(-0.9057375, +0.3003471),
    vec2(-0.3487388, +0.4037880),
    vec2(+0.1023042, +0.6439373),
    vec2(+0.5699277, +0.3513750),
    vec2(+0.2939128, -0.1131226),
    vec2(+0.7836658, -0.4208784),
    vec2(+0.1564120, -0.8198990));

const int OLO_DENOISE_POISSON_COUNT = 8;

// The integer texel a disc tap lands on, pushed OFF the centre.
//
// `center + ivec2(round(offset))` looks right and is not: the Poisson points
// have magnitudes from 0.16 to 0.95, so at a radius of 1-2 texels three or four
// of the eight round to (0,0) and land back on the centre. The centre then gets
// counted four or five times, the kernel is far weaker than its eight taps
// suggest, and the shape is centre-heavy in a way no comment would tell you —
// the same "one sample weighted many times" failure the out-of-bounds branch in
// each blur exists to prevent.
//
// Rounding the magnitude AWAY FROM ZERO on each axis keeps every tap at least
// one texel out while preserving the pattern's direction, so a radius of 1
// really is a ring of eight neighbours rather than a lump on the centre.
ivec2 OloDenoiseTapOffset(vec2 offset)
{
    // sign() is 0 at exactly 0.0, which would leave that axis on the centre;
    // that is correct and intended for an offset with no component on the axis.
    vec2 magnitude = max(abs(offset), vec2(1.0));
    return ivec2(sign(offset) * ceil(magnitude - 0.5));
}

// Rotate the disc by the golden angle per frame (and per stage). The pattern is
// the SAME for every pixel within a frame — deliberately, because a per-pixel
// rotation costs a noise fetch and buys nothing once the temporal resolve is
// averaging over frames anyway; what matters is that consecutive frames do not
// re-average the same eight directions. The golden angle spreads successive
// indices maximally, so any window of frames is close to a uniform cover.
mat2 OloDenoiseDiscRotation(uint frameIndex, uint stage)
{
    const float goldenAngle = 2.39996323; // radians
    float angle = (float(frameIndex) + float(stage) * 0.61803399) * goldenAngle;
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------
// View-space position of a pixel from its UV and its POSITIVE view depth,
// without a depth texture: every stage of this chain already carries the view
// depth in the signal's alpha, and re-sampling a depth buffer at a different
// resolution than the signal is how a half-res filter ends up guided by
// full-res silhouettes it cannot see.
//
// The construction rides the ray through the pixel out to the depth, so it is
// exact for any perspective projection and needs only the inverse projection
// the pass already has in its UBO.
vec3 OloDenoiseViewPosition(mat4 invProjection, vec2 uv, float viewDepth)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, -1.0, 1.0);
    vec4 nearPoint = invProjection * clip;
    vec3 ray = nearPoint.xyz / nearPoint.w;
    // ray.z is negative (view space looks down -z); scale the ray so that its
    // -z equals the requested depth. The guard keeps a degenerate ray from
    // producing an infinity that would poison every weight downstream.
    return ray * (viewDepth / max(-ray.z, 1.0e-6));
}

// How far the tap lies out of the centre pixel's tangent plane, in units of the
// tolerance — 1 at the centre, falling linearly to 0 at the tolerance and
// staying there. Linear rather than Gaussian on purpose: a Gaussian never quite
// reaches zero, so a wall three metres behind the floor keeps a small weight and
// bleeds through wherever the accepted taps are few.
//
// DEPARTURE FROM THE REFERENCE: Timberdoodle scales its tolerance by the
// world-space width of a pixel, which needs the near plane and the render target
// size in the same units the tracer used. This chain runs at two resolutions in
// the same frame (half-res filter, full-res upscale), so a pixel-width tolerance
// would mean two different tolerances for the same surface. A tolerance
// RELATIVE TO THE VIEW DEPTH is resolution-independent, which is what this chain
// needs.
float OloDenoisePlaneWeight(vec3 centerPositionVS, vec3 centerNormalVS, vec3 samplePositionVS,
                            float relativeTolerance, float centerViewDepth)
{
    float tolerance = max(relativeTolerance * max(centerViewDepth, 1.0e-4), 1.0e-4);
    float planeDistance = abs(dot(samplePositionVS - centerPositionVS, centerNormalVS));
    return 1.0 - clamp(planeDistance / tolerance, 0.0, 1.0);
}

// Normal agreement, as a soft suggestion rather than a cutoff. At the low
// resolutions this chain filters at, a hard normal threshold strips the taps
// off every curved surface; a raised cosine keeps a slightly-turned tap at a
// reduced weight so a curved surface still gets filtered.
float OloDenoiseNormalWeight(vec3 centerNormal, vec3 sampleNormal, float power)
{
    float cosine = max(dot(centerNormal, sampleNormal), 0.0);
    return pow(cosine, max(power, 1.0));
}

// -----------------------------------------------------------------------------
// The radius guide
// -----------------------------------------------------------------------------
// DEPARTURE FROM THE REFERENCE, stated plainly because the reference argues
// against it: the Denoisinator does NOT guide on variance, on the grounds that a
// raw per-pixel variance estimate of a one-ray-per-pixel signal chases its own
// noise. That objection is about variance measured on the RAW trace. The
// variance this chain reads is the one the temporal resolve already accumulates
// over up to 255 frames of history (OloAccumulateTemporalMoments), which is a
// far more stable quantity than a per-frame estimate — and issue #708 asks for
// exactly this. It is guarded two ways: the estimate is taken RELATIVE to the
// mean so exposure cannot move it, and the history-length term below dominates
// it wherever the history is too short for the variance to mean anything.
//
// Returns a radius in pixels between minRadius and maxRadius.
float OloDenoiseVarianceRadius(float variance, float mean, float minRadius, float maxRadius, float knee)
{
    float sigma = sqrt(max(variance, 0.0));
    float relative = sigma / max(mean, 1.0e-4);
    float t = clamp(relative / max(knee, 1.0e-4), 0.0, 1.0);
    return mix(minRadius, maxRadius, t);
}

// Freshly disoccluded pixels have no history to have converged, so their
// variance estimate is meaningless and their true error is at its worst. Widen
// on a SHORT history and let that dominate: 1 at zero accumulated frames,
// falling to 0 once the history reaches `targetLength`.
//
// This is the post-blur's half of "variance-driven history length" — the resolve
// drives the history length from the surface test, and the post-blur reads the
// resulting length back as the strength of the widening.
float OloDenoiseHistoryWidening(float historyLength, float targetLength)
{
    return 1.0 - clamp(historyLength / max(targetLength, 1.0), 0.0, 1.0);
}

// The post-blur's final radius: the wider of what the variance asks for and what
// the short history asks for. `max`, not a sum or a product — either reason
// alone is sufficient grounds to widen, and multiplying them would let a
// converged-but-noisy region cancel against a fresh-but-flat one.
float OloDenoisePostBlurRadius(float variance, float mean, float historyLength,
                               float minRadius, float maxRadius, float knee,
                               float targetHistoryLength)
{
    float varianceRadius = OloDenoiseVarianceRadius(variance, mean, minRadius, maxRadius, knee);
    float historyRadius = mix(minRadius, maxRadius,
                              OloDenoiseHistoryWidening(historyLength, targetHistoryLength));
    return max(varianceRadius, historyRadius);
}

// -----------------------------------------------------------------------------
// The specular radius guide
// -----------------------------------------------------------------------------
// SSR's radius comes from ROUGHNESS, not from variance, and that difference is
// the honest answer to "which stages transfer from the diffuse denoiser".
//
// Indirect diffuse is smooth everywhere, so blurring it more where it is noisier
// is always safe. A specular reflection is not: a mirror carries the sharpest
// detail in the frame, its reflection of a high-contrast edge has genuinely high
// variance, and a variance-guided filter would therefore blur precisely the
// pixels that must stay sharp. What sets the correct filter width for a
// reflection is the width of the specular lobe, and that is roughness.
//
// So: 0 radius below the knee (a mirror is left alone), ramping to the full
// radius at and above it. Smoothstep rather than a linear ramp so there is no
// visible boundary on a surface with a roughness gradient.
float OloDenoiseRoughnessRadius(float roughness, float maxRadius, float knee)
{
    return maxRadius * smoothstep(0.0, max(knee, 1.0e-4), roughness);
}

// Reject taps the SSR TRACE deliberately produced no reflection for.
//
// PostProcess_SSR.glsl early-outs to a zero delta above its roughness cutoff,
// but still writes a valid depth and normal into the guide — so a purely
// geometric filter reads those pixels as a legitimate "no reflection here" and
// averages the zeros in. Across a polished/rough material seam on ONE coplanar
// floor, every geometric test passes and the reflective side gets a dark band
// the width of the blur radius.
//
// Two rejections, both needed: a tap past the cutoff contributed nothing by
// construction, and a tap whose roughness is far from the centre's belongs to a
// different specular lobe even when it is on the same plane.
float OloDenoiseSpecularTapWeight(float centerRoughness, float tapRoughness, float maxRoughness)
{
    if (tapRoughness > maxRoughness)
        return 0.0;
    // Relative to the centre's own lobe width, so a mirror rejects far more
    // aggressively than a rough surface — which is the behaviour that keeps a
    // sharp reflection sharp right up to the seam.
    float tolerance = max(0.25 * max(centerRoughness, 0.02), 0.02);
    return 1.0 - clamp(abs(tapRoughness - centerRoughness) / tolerance, 0.0, 1.0);
}

// -----------------------------------------------------------------------------
// Octahedral normal decode
// -----------------------------------------------------------------------------
// Matches octEncodeGB() in PBR_GBuffer.glsl. The chain's guide attachment packs
// its normal the same way the G-Buffer does, so the upscale can compare a
// half-res guide normal against a full-res G-Buffer normal without a conversion
// step that could drift between the two.
vec3 OloDenoiseOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

#endif // OLO_SPATIAL_DENOISE_GLSL
