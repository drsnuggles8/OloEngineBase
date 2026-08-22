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

// Gather a 3x3 neighbourhood of SAMPLER around UV and fill an OloTemporalStats.
// SAMPLER is a name, not a value — see the header comment for why.
//
// Samples are converted to YCoCg here so every statistic, and therefore the
// clip box, lives in one space. Feed OloTemporalClipHistory the same space.
#define OLO_TEMPORAL_GATHER_3X3(SAMPLER, UV, TEXEL, OUT_STATS)                                  \
    {                                                                                            \
        vec3 oloTgM1 = vec3(0.0);                                                                \
        vec3 oloTgM2 = vec3(0.0);                                                                \
        vec3 oloTgMin = vec3(1.0e10);                                                            \
        vec3 oloTgMax = vec3(-1.0e10);                                                           \
        for (int oloTgY = -1; oloTgY <= 1; ++oloTgY)                                             \
        {                                                                                        \
            for (int oloTgX = -1; oloTgX <= 1; ++oloTgX)                                         \
            {                                                                                    \
                vec3 oloTgS = OloRGBToYCoCg(                                                     \
                    texture(SAMPLER, (UV) + vec2(float(oloTgX), float(oloTgY)) * (TEXEL)).rgb);  \
                oloTgM1 += oloTgS;                                                               \
                oloTgM2 += oloTgS * oloTgS;                                                      \
                oloTgMin = min(oloTgMin, oloTgS);                                                \
                oloTgMax = max(oloTgMax, oloTgS);                                                \
            }                                                                                    \
        }                                                                                        \
        (OUT_STATS).Mean = oloTgM1 / 9.0;                                                        \
        (OUT_STATS).StdDev =                                                                     \
            sqrt(max(vec3(0.0), oloTgM2 / 9.0 - (OUT_STATS).Mean * (OUT_STATS).Mean));           \
        (OUT_STATS).MinC = oloTgMin;                                                             \
        (OUT_STATS).MaxC = oloTgMax;                                                             \
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

// The final blend. `confidence` is the product of every rejection term the
// caller computed; at 0 the result is the current frame exactly, which is the
// correct answer for a disoccluded pixel (there is no history to keep).
vec3 OloTemporalBlend(vec3 current, vec3 clampedHistory, float feedback, float confidence)
{
    return mix(current, clampedHistory, clamp(feedback, 0.0, 0.98) * clamp(confidence, 0.0, 1.0));
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

#endif // OLO_TEMPORAL_RESOLVE_GLSL
