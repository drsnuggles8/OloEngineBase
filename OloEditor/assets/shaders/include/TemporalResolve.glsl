// =============================================================================
// TemporalResolve.glsl — the reusable temporal accumulation kernel (issue #706)
//
// A temporal resolve is always the same four questions:
//
//   1. WHERE was this pixel last frame?          -> reprojection
//   2. Is that history the SAME SURFACE?         -> disocclusion rejection
//   3. Is that history's VALUE still plausible?  -> neighbourhood clip
//   4. How much of it do I keep?                 -> feedback blend
//
// Before this header the engine answered them twice, differently:
// PostProcess_TAA.glsl (velocity dilation, YCoCg variance clip, motion-scaled
// feedback, no depth test) and PostProcess_CloudscapeResolve.glsl (world-point
// reprojection, RGB min/max clamp, fixed feedback). Neither was wrong; they
// simply could not share a fix. SSR and SSGI would have made it four.
//
// #706 brought TAA across, #902 brought SSR and SSGI, and #903 brought the
// cloudscape — whose signal is RGBA, alpha carrying transmittance rather than a
// colour. That is what the OloTemporalScalarStats half of this header is for;
// see "Channels that are NOT colour" below.
//
// -----------------------------------------------------------------------------
// WHY FUNCTIONS AND A MACRO RATHER THAN ONE BIG CALL
// -----------------------------------------------------------------------------
// The obvious shape — one OloTemporalResolve(current, history, velocity, …) —
// cannot be written portably, because gathering the neighbourhood needs the
// CALLER's sampler and GLSL has no portable way to hand a sampler to a function
// through a struct. So the gather is a macro that takes the sampler name, and
// everything downstream of it is a plain function over already-fetched data.
// That split is deliberate: the macro is the one piece that touches a sampler,
// and every piece that carries actual math is a function a CPU test can mirror.
//
// -----------------------------------------------------------------------------
// THE ONE THING TO GET RIGHT
// -----------------------------------------------------------------------------
// Clipping the history toward the neighbourhood mean along the segment joining
// it to the current colour (OloTemporalClipToAABB) is NOT the same as clamping
// it componentwise. Clamping moves the history to the nearest point of the box,
// which can be a hue the neighbourhood never contained; clipping keeps it on the
// line to a colour that IS in the frame, so a rejected history desaturates
// toward the current pixel instead of shifting colour. That difference is
// invisible on a grey test scene and obvious on a saturated one, which is why
// TAA's componentwise clamp survived as long as it did.
// =============================================================================

#ifndef OLO_TEMPORAL_RESOLVE_GLSL
#define OLO_TEMPORAL_RESOLVE_GLSL

// -----------------------------------------------------------------------------
// Colour space
//
// The clip happens in YCoCg because luma and chroma get separate variance there:
// a box built in RGB couples them, so a bright edge widens the chroma box too
// and lets a ghost through with the right brightness and the wrong colour.
// -----------------------------------------------------------------------------
vec3 OloRGBToYCoCg(vec3 c)
{
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)),
                dot(c, vec3(0.5, 0.0, -0.5)),
                dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 OloYCoCgToRGB(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// -----------------------------------------------------------------------------
// Neighbourhood statistics
// -----------------------------------------------------------------------------
struct OloTemporalStats
{
    vec3 Mean;   // first moment
    vec3 StdDev; // sqrt of the (clamped-nonnegative) second central moment
    vec3 MinC;   // hard neighbourhood minimum
    vec3 MaxC;   // hard neighbourhood maximum
};

// -----------------------------------------------------------------------------
// Channels that are NOT colour  (issue #903)
// -----------------------------------------------------------------------------
// A resolve whose signal is RGBA is not a four-channel colour resolve. The
// cloudscape's alpha is TRANSMITTANCE — the fraction of the scene behind the
// pixel that survives, consumed downstream as `scene * a + rgb`. It is an
// occupancy, not a hue.
//
// So it must not enter the YCoCg transform. That transform exists to give luma
// and chroma separate variance; pushing a fourth, non-colour quantity through it
// would mix transmittance into the chroma axes and build a clip box with an axis
// that means nothing physical — the history would then be rejected for having
// the wrong "colour" in a channel that has no colour.
//
// A non-colour channel therefore gets its own 1-D box. And in one dimension the
// segment clip and the componentwise clamp ARE THE SAME OPERATION: the failure
// clamping causes — landing on a value the neighbourhood never contained —
// needs at least two coupled axes to exist, and one axis has no second axis to
// leave. So the `clamp` below IS OloTemporalClipToAABB in its degenerate case,
// not a survivor of the hand-rolled clamp the RGB path exists to replace.
struct OloTemporalScalarStats
{
    float Mean;   // first moment
    float StdDev; // sqrt of the (clamped-nonnegative) second central moment
    float MinC;   // hard neighbourhood minimum
    float MaxC;   // hard neighbourhood maximum
};

// Gather a 3x3 neighbourhood of an RGBA SAMPLER once, and fill BOTH an
// OloTemporalStats (rgb, in YCoCg) and an OloTemporalScalarStats (alpha, left
// in its own linear space).
//
// One loop, nine taps — not two loops and eighteen. A caller that needs both
// sets of statistics is reading the same texels for both, and on a half-
// resolution pass the tap count is essentially the whole cost of the kernel.
#define OLO_TEMPORAL_GATHER_3X3_RGBA(SAMPLER, UV, TEXEL, OUT_STATS, OUT_ALPHA_STATS)                 \
    {                                                                                                \
        vec3 oloTgM1 = vec3(0.0);                                                                    \
        vec3 oloTgM2 = vec3(0.0);                                                                    \
        vec3 oloTgMin = vec3(1.0e10);                                                                \
        vec3 oloTgMax = vec3(-1.0e10);                                                               \
        float oloTgA1 = 0.0;                                                                         \
        float oloTgA2 = 0.0;                                                                         \
        float oloTgAMin = 1.0e10;                                                                    \
        float oloTgAMax = -1.0e10;                                                                   \
        for (int oloTgY = -1; oloTgY <= 1; ++oloTgY)                                                 \
        {                                                                                            \
            for (int oloTgX = -1; oloTgX <= 1; ++oloTgX)                                             \
            {                                                                                        \
                vec4 oloTgT =                                                                        \
                    texture(SAMPLER, (UV) + vec2(float(oloTgX), float(oloTgY)) * (TEXEL));           \
                vec3 oloTgS = OloRGBToYCoCg(oloTgT.rgb);                                             \
                oloTgM1 += oloTgS;                                                                   \
                oloTgM2 += oloTgS * oloTgS;                                                          \
                oloTgMin = min(oloTgMin, oloTgS);                                                    \
                oloTgMax = max(oloTgMax, oloTgS);                                                    \
                oloTgA1 += oloTgT.a;                                                                 \
                oloTgA2 += oloTgT.a * oloTgT.a;                                                      \
                oloTgAMin = min(oloTgAMin, oloTgT.a);                                                \
                oloTgAMax = max(oloTgAMax, oloTgT.a);                                                \
            }                                                                                        \
        }                                                                                            \
        (OUT_STATS).Mean = oloTgM1 / 9.0;                                                            \
        (OUT_STATS).StdDev =                                                                         \
            sqrt(max(vec3(0.0), oloTgM2 / 9.0 - (OUT_STATS).Mean * (OUT_STATS).Mean));               \
        (OUT_STATS).MinC = oloTgMin;                                                                 \
        (OUT_STATS).MaxC = oloTgMax;                                                                 \
        (OUT_ALPHA_STATS).Mean = oloTgA1 / 9.0;                                                      \
        (OUT_ALPHA_STATS).StdDev = sqrt(                                                             \
            max(0.0, oloTgA2 / 9.0 - (OUT_ALPHA_STATS).Mean * (OUT_ALPHA_STATS).Mean));              \
        (OUT_ALPHA_STATS).MinC = oloTgAMin;                                                          \
        (OUT_ALPHA_STATS).MaxC = oloTgAMax;                                                          \
    }

// Gather a 3x3 neighbourhood of SAMPLER around UV and fill an OloTemporalStats.
// SAMPLER is a name, not a value — see the header comment for why.
//
// Samples are converted to YCoCg here so every statistic, and therefore the
// clip box, lives in one space. Feed OloTemporalClipHistory the same space.
//
// This is the RGBA gather with the alpha half thrown away, rather than a second
// copy of the same nine-tap loop. A colour-only caller pays nothing for it:
// texture() returns a vec4 regardless, and the alpha accumulators are local
// dead stores the compiler drops. The point of the delegation is that the
// colour statistics have exactly ONE implementation — a header that exists to
// stop two passes carrying two resolves must not itself carry two gathers.
#define OLO_TEMPORAL_GATHER_3X3(SAMPLER, UV, TEXEL, OUT_STATS)                                  \
    {                                                                                           \
        OloTemporalScalarStats oloTgDiscardedAlpha;                                             \
        OLO_TEMPORAL_GATHER_3X3_RGBA(SAMPLER, UV, TEXEL, OUT_STATS, oloTgDiscardedAlpha);       \
    }

// -----------------------------------------------------------------------------
// Clipping
// -----------------------------------------------------------------------------

// Move `history` along the segment joining it to the box CENTRE until it lands
// on the box surface. Returns `history` untouched when it is already inside.
// This is the Karis/Lottes clip; the centre is the anchor because it is the one
// point guaranteed to be inside, so the segment always terminates.
//
// The scale is by the LARGEST violating axis, applied to the whole offset —
// scaling per axis is precisely the componentwise clamp this exists to avoid,
// because it leaves the segment and can land on a colour the neighbourhood
// never contained.
vec3 OloTemporalClipToAABB(vec3 history, vec3 boxMin, vec3 boxMax)
{
    vec3 centre = 0.5 * (boxMax + boxMin);
    vec3 halfExtent = 0.5 * (boxMax - boxMin) + vec3(1.0e-6);
    vec3 offset = (history - centre) / halfExtent;

    float maxUnit = max(abs(offset.x), max(abs(offset.y), abs(offset.z)));
    if (maxUnit <= 1.0)
        return history;

    return centre + (history - centre) / maxUnit;
}

// Variance clip: build a box from the neighbourhood mean +/- gamma * stddev,
// intersected with the hard min/max, and clip the history into it.
//
// `gamma` is the ONLY tuning knob and it trades ghosting against noise
// directly — 1.0 is aggressive (crisp, noisier), 1.25 is the common default,
// 2.0+ barely rejects anything. Input and output are in YCoCg.
vec3 OloTemporalClipHistory(vec3 historyYCoCg, OloTemporalStats stats, float gamma)
{
    vec3 boxMin = max(stats.MinC, stats.Mean - gamma * stats.StdDev);
    vec3 boxMax = min(stats.MaxC, stats.Mean + gamma * stats.StdDev);
    // The variance box and the hard box can cross on a near-flat neighbourhood,
    // inverting the intersection; re-order so the box is always well formed.
    return OloTemporalClipToAABB(historyYCoCg, min(boxMin, boxMax), max(boxMin, boxMax));
}

// -----------------------------------------------------------------------------
// Validity and disocclusion
// -----------------------------------------------------------------------------

// The cheapest and most important test: did the reprojection leave the screen?
// Off-screen history is not "old", it is a different pixel entirely.
bool OloTemporalHistoryUVValid(vec2 prevUV)
{
    return all(greaterThanEqual(prevUV, vec2(0.0))) && all(lessThanEqual(prevUV, vec2(1.0)));
}

// Question 1, for a pass that knows a WORLD POINT rather than a velocity — the
// cloudscape, whose signal has no depth buffer and reprojects the view ray's
// cloud-layer midpoint instead.
//
// `valid` is false when the point is BEHIND the previous camera. That case
// needs its own answer rather than the caller's memory, because w <= 0 does not
// produce an obviously wrong UV: the perspective divide by a negative w
// mirrors the point back through the origin and lands somewhere perfectly
// plausible on screen, so the off-screen test downstream would pass it. The
// returned UV is (-1, -1) for exactly that reason — a caller who ignores
// `valid` still gets a UV that OloTemporalHistoryUVValid rejects.
//
// TAA does not use this: it has a velocity texture, dilates it, and falls back
// to reconstructing from depth. Different question-1, same question 2-4.
vec2 OloTemporalReprojectWorldPoint(mat4 prevViewProjection, vec3 worldPosition, out bool valid)
{
    vec4 prevClip = prevViewProjection * vec4(worldPosition, 1.0);
    valid = prevClip.w > 0.0;
    if (!valid)
        return vec2(-1.0);
    return (prevClip.xy / prevClip.w) * 0.5 + 0.5;
}

// Confidence in [0,1] that `prevViewDepth` describes the same surface as
// `currentViewDepth`. Both are POSITIVE view-space distances (-viewPos.z), not
// device depth: device depth is wildly nonlinear, so a fixed tolerance on it
// means centimetres near the camera and kilometres far away, and a
// disocclusion test that only works at one distance is worse than none.
//
// The tolerance is RELATIVE so it tracks depth precision automatically.
float OloTemporalDepthConfidence(float currentViewDepth, float prevViewDepth, float relativeTolerance)
{
    float reference = max(currentViewDepth, 1.0e-4);
    float relativeError = abs(currentViewDepth - prevViewDepth) / reference;
    return 1.0 - smoothstep(relativeTolerance, relativeTolerance * 2.0, relativeError);
}

// Confidence in [0,1] that two normals describe the same surface. Catches the
// case depth cannot: a silhouette where the front and back surfaces happen to
// be at almost the same distance.
float OloTemporalNormalConfidence(vec3 currentNormal, vec3 prevNormal, float minCos)
{
    float c = dot(currentNormal, prevNormal);
    return smoothstep(minCos, mix(minCos, 1.0, 0.5), c);
}

// -----------------------------------------------------------------------------
// Blending
// -----------------------------------------------------------------------------

// Scale feedback down under real motion, with a sub-pixel DEAD ZONE.
//
// The dead zone is not a nicety. Any jittered pass moves ~1px frame to frame by
// construction, so without it a stationary camera reads as motion, feedback
// collapses toward `motionFloor`, and a fraction of the current (noisy, or
// jittered) frame bleeds through every frame — visible as a faint shake that
// looks like the resolve is broken rather than mistuned.
//
// `motionFloor` is a FLOOR, taken through min(): motion may only ever reduce
// feedback. Mixing toward a bare constant — which is what the inline TAA code
// this was lifted from did — silently RAISES feedback whenever the caller's
// feedback is below that constant, so a deliberately-responsive setting would
// get more ghosty the faster the camera moved. TAA's own 0.9/0.5 pairing never
// hit it, but a shared header must not carry the trap forward.
float OloTemporalMotionFeedback(float feedback, vec2 velocityPixels, float deadZonePx, float saturatePx,
                                float motionFloor)
{
    float span = max(saturatePx - deadZonePx, 1.0e-4);
    float motion = clamp((length(velocityPixels) - deadZonePx) / span, 0.0, 1.0);
    return mix(feedback, min(feedback, motionFloor), motion);
}

// How much history survives: the caller's feedback, capped, times every
// rejection term it computed. ONE definition, because the 0.98 cap is a real
// decision (a feedback of 1.0 is a history that never updates) and a resolve
// that applied a different cap to a different channel would drift its channels
// apart. Every blend below goes through this.
float OloTemporalFeedbackWeight(float feedback, float confidence)
{
    return clamp(feedback, 0.0, 0.98) * clamp(confidence, 0.0, 1.0);
}

// The final blend. `confidence` is the product of every rejection term the
// caller computed; at 0 the result is the current frame exactly, which is the
// correct answer for a disoccluded pixel (there is no history to keep).
vec3 OloTemporalBlend(vec3 current, vec3 clampedHistory, float feedback, float confidence)
{
    return mix(current, clampedHistory, OloTemporalFeedbackWeight(feedback, confidence));
}

// The same blend for one non-colour channel — see OloTemporalScalarStats.
float OloTemporalBlendScalar(float current, float clampedHistory, float feedback, float confidence)
{
    return mix(current, clampedHistory, OloTemporalFeedbackWeight(feedback, confidence));
}

// -----------------------------------------------------------------------------
// The whole kernel, for callers that have already fetched everything
//
// current / history in RGB; stats in YCoCg from OLO_TEMPORAL_GATHER_3X3.
// -----------------------------------------------------------------------------
vec3 OloTemporalResolve(vec3 current,
                        vec3 history,
                        OloTemporalStats stats,
                        float gamma,
                        float feedback,
                        float confidence)
{
    vec3 clipped = OloYCoCgToRGB(OloTemporalClipHistory(OloRGBToYCoCg(history), stats, gamma));
    return OloTemporalBlend(current, clipped, feedback, confidence);
}

// Variance clip for one non-colour channel. Same box construction as
// OloTemporalClipHistory — mean +/- gamma * stddev intersected with the hard
// min/max, re-ordered so a near-flat neighbourhood cannot invert it — so a
// scalar channel is rejected on the same terms as the colour it travels with.
//
// The intersection with the hard min/max is LOAD-BEARING, not belt and braces.
// `sqrt(E[x^2] - mean^2)` does not return zero on a flat neighbourhood in fp32:
// one ULP of cancellation in the subtraction (5.96e-8 at x = 0.6) becomes
// 2.4e-4 once the sqrt is taken, because sqrt has unbounded slope at the
// origin. So the variance box over a uniform patch of sky is ~+/-4e-4 wide
// rather than degenerate, and only the hard box closes it back up. Drop the
// intersection as redundant and a flat neighbourhood silently starts admitting
// a sliver of history it should reject. Measured on the GPU at 3.2e-4 by
// ShaderUnitTemporalResolveTest.ColourSpreadDoesNotWidenTheAlphaBox.
float OloTemporalClipHistoryScalar(float history, OloTemporalScalarStats stats, float gamma)
{
    float boxMin = max(stats.MinC, stats.Mean - gamma * stats.StdDev);
    float boxMax = min(stats.MaxC, stats.Mean + gamma * stats.StdDev);
    return clamp(history, min(boxMin, boxMax), max(boxMin, boxMax));
}

// The whole kernel for an RGBA signal whose alpha is not a colour channel.
//
// rgb and alpha are clipped in their own spaces but blended with ONE feedback
// and ONE confidence, deliberately: the two travel together into a
// premultiplied composite (`scene * a + rgb`), so resolving them at different
// rates would make the radiance and the transmittance describe different
// instants and shift the composite's brightness on its own.
vec4 OloTemporalResolveRGBA(vec4 current,
                            vec4 history,
                            OloTemporalStats stats,
                            OloTemporalScalarStats alphaStats,
                            float gamma,
                            float feedback,
                            float confidence)
{
    vec3 clippedRGB = OloYCoCgToRGB(OloTemporalClipHistory(OloRGBToYCoCg(history.rgb), stats, gamma));
    float clippedA = OloTemporalClipHistoryScalar(history.a, alphaStats, gamma);

    return vec4(OloTemporalBlend(current.rgb, clippedRGB, feedback, confidence),
                OloTemporalBlendScalar(current.a, clippedA, feedback, confidence));
}

#endif // OLO_TEMPORAL_RESOLVE_GLSL
