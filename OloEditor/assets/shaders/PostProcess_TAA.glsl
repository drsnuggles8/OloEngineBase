// =============================================================================
// PostProcess_TAA.glsl — Temporal Anti-Aliasing
//
// Velocity-reprojected temporal accumulation with 3x3 neighborhood colour
// clipping (YCoCg variance clip). Consumes:
//   - slot 0: current-frame scene colour
//   - slot 1: history (previous TAA output)
//   - slot 2: velocity (RG16F) — valid when u_HasVelocityTexture != 0 (Deferred)
//   - slot 19 (TEX_POSTPROCESS_DEPTH): scene depth for camera-only velocity
//     reconstruction in Forward / Forward+ paths
//
// Motion-blur UBO (binding 8) supplies InverseViewProjection + PrevViewProjection
// so camera-only reprojection works even when RT3 is unavailable.
//
// Two outputs (#1533): o_Color, the frame the chain shows (the resolve,
// sharpened), and o_History, the resolve before the sharpen, which the graph
// copies into the persistent history for next frame (TAARenderPass::Setup).
// On first frame (history == black) TAA decays back to the current frame
// automatically via the neighborhood clip.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): vertex pulling from the engine-wide binding 57.
// The stream is the standard 20-byte {vec3 position, vec2 uv}; this shader's
// GL branch consumes only the position and DERIVES its UV — the pull branch
// reproduces that derivation exactly rather than reading floats 3–4, so the
// two routes cannot disagree.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec2 position = vec2(b_Vertices.v[base + 0], b_Vertices.v[base + 1]);
    v_TexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_Position.xy * 0.5 + 0.5;
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}
#endif

#type fragment
#version 460 core

// Texture inputs. Under heap-bindless (issue #691) these become heap
// lookups keyed by the SAME slot numbers the bindful branch declares, so the two
// variants cannot disagree about which texture is which — and the shader BODY
// below is unchanged between them. Inert without OLO_BINDLESS.
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Current OLO_HEAP_TEX_2D(0)
#define u_History OLO_HEAP_TEX_2D(1)
#define u_Velocity OLO_HEAP_TEX_2D(2)
#define u_PrevSurface OLO_HEAP_TEX_2D(3)
#define u_DepthTexture OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_Current;
layout(binding = 1) uniform sampler2D u_History;
layout(binding = 2) uniform sampler2D u_Velocity;
// Previous frame's G-Buffer RT3 (#1256): velocity .rg, coverage .b,
// material profile .a. Same layout as u_Velocity, one frame older.
layout(binding = 3) uniform sampler2D u_PrevSurface;
layout(binding = 19) uniform sampler2D u_DepthTexture;
#endif

// The shared temporal kernel (issue #706). TAA was the original hand-rolled
// implementation of this; the functions below now live in one header so SSR,
// SSGI and the cloudscape resolve instantiate the same reprojection /
// neighbourhood-clip / feedback logic instead of each carrying a variant.
#include "include/TemporalResolve.glsl"
// The separated history-rejection model (#1256). TAA uses only its
// COVERAGE and MATERIAL-PROFILE terms — see the confidence block below.
#include "include/SurfaceHistory.glsl"
// RT3 .a's stochastic mark: a coverage that is one draw of an estimator.
#include "include/SurfaceCoverageMark.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;
layout(location = 1) out vec4 o_History;


layout(std140, binding = 8) uniform MotionBlurMatrices
{
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
    // xy this frame's TAA jitter offset, zw the previous frame's, in velocity
    // units (MotionBlurUBOData::JitterUV): the reconstruction below takes them
    // out as every velocity writer does, so a still camera reads zero (#1552).
    vec4 u_MotionJitterUV;
};

layout(std140, binding = 32) uniform TAAParams
{
    vec4 u_TAA_FeedbackSharpnessHasVelocity; // x=feedback, y=sharpness, z=hasVelocity (0/1), w=pad
    vec4 u_TAA_TexelSize;                    // xy=1/size, z=hasHistory (0/1, #1348), w=pad
};

#define u_Feedback           (u_TAA_FeedbackSharpnessHasVelocity.x)
#define u_Sharpness          (u_TAA_FeedbackSharpnessHasVelocity.y)
#define u_HasVelocityTexture (int(u_TAA_FeedbackSharpnessHasVelocity.z))
#define u_TexelSize          (u_TAA_TexelSize.xy)
// Whether u_History holds a history at all (#1348). Without one the pass binds
// the CURRENT frame there, so reprojecting it would blend this frame with a
// copy of itself displaced by this frame's velocity.
#define u_HasHistory         (u_TAA_TexelSize.z > 0.5)
// w was pad; #1256 uses it as the has-surface-history flag.
#define u_HasSurfaceHistory  (u_TAA_FeedbackSharpnessHasVelocity.w > 0.5)

// (RGBToYCoCg / YCoCgToRGB moved to include/TemporalResolve.glsl as
// OloRGBToYCoCg / OloYCoCgToRGB — same matrices, one copy.)

// Reconstruct camera-motion velocity from depth (Forward / Forward+ path)
vec2 ReconstructCameraVelocity(vec2 uv)
{
    float depth = texture(u_DepthTexture, uv).r;
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos = u_InverseViewProjection * ndc;
    worldPos /= worldPos.w;
    vec4 prevClip = u_PrevViewProjection * worldPos;
    if (prevClip.w <= 0.0001)
        return vec2(0.0);
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    // current - prev, the jitter taken out (matches the convention of RT3 velocity)
    return (uv - prevUV) - (u_MotionJitterUV.xy - u_MotionJitterUV.zw);
}

// Find closest-depth pixel in 3x3 neighborhood — standard velocity-dilation
// trick that reduces foreground object ghosting against moving backgrounds.
vec2 GetDilatedVelocityUV(vec2 uv)
{
    vec2 bestUV = uv;
    float bestDepth = 1.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 sampleUV = uv + vec2(x, y) * u_TexelSize;
            float d = texture(u_DepthTexture, sampleUV).r;
            if (d < bestDepth)
            {
                bestDepth = d;
                bestUV = sampleUV;
            }
        }
    }
    return bestUV;
}

// Whether any of four coverages is FRACTIONAL, strictly between empty and full.
// An opaque surface writes 1 and a cleared texel reads 0; only a coverage
// estimator writes in between: a widened strand's alpha, an alpha-tested leaf's.
bool OloTaaAnyCoverageIsFractional(vec4 coverage)
{
    vec4 inside = step(vec4(0.001), coverage) * step(coverage, vec4(0.999));
    return dot(inside, vec4(1.0)) > 0.0;
}

// Whether any of four material profiles holds the stochastic mark. A profile
// is never negative; the mark is (include/SurfaceCoverageMark.glsl).
bool OloTaaAnyProfileIsStochastic(vec4 profile)
{
    return dot(step(profile, vec4(0.5 * OLO_STOCHASTIC_COVERAGE_MARK)), vec4(1.0)) > 0.0;
}

// What the 4x4 texels around UV say about their coverage, in eight gathers:
// whether any coverage (.b) is fractional, and whether any profile (.a) holds
// the stochastic mark. A gather returns texels whatever the sampler's filter,
// so a bilinear blend of an opaque edge cannot pass for an estimator's value.
// SAMPLER is a name, not a value; see include/TemporalResolve.glsl for why.
#define OLO_TAA_COVERAGE_KIND_4X4(SAMPLER, UV, TEXEL, OUT_FRACTIONAL, OUT_STOCHASTIC)                          \
    {                                                                                                            \
        OUT_FRACTIONAL = false;                                                                                  \
        OUT_STOCHASTIC = false;                                                                                  \
        for (int oloGy = 0; oloGy < 2; ++oloGy)                                                                  \
        {                                                                                                        \
            for (int oloGx = 0; oloGx < 2; ++oloGx)                                                              \
            {                                                                                                    \
                vec2 oloCorner = (UV) + (((vec2(float(oloGx), float(oloGy)) * 2.0) - 0.5) * (TEXEL));            \
                OUT_FRACTIONAL = OUT_FRACTIONAL || OloTaaAnyCoverageIsFractional(textureGather(SAMPLER, oloCorner, 2)); \
                OUT_STOCHASTIC = OUT_STOCHASTIC || OloTaaAnyProfileIsStochastic(textureGather(SAMPLER, oloCorner, 3)); \
            }                                                                                                    \
        }                                                                                                        \
    }

// The mean coverage (.b) of the 8x8 texels around UV, in sixteen gathers, each
// on a texel corner and so returning the four texels that share it. The window
// is the texel UV falls in, four texels back and three on, for both frames
// compared. SAMPLER is a name, not a value.
#define OLO_TAA_MEAN_COVERAGE_8X8(SAMPLER, UV, TEXEL, OUT_MEAN)                                       \
    {                                                                                                  \
        float oloCoverageSum = 0.0;                                                                    \
        for (int oloGy = 0; oloGy < 4; ++oloGy)                                                        \
        {                                                                                              \
            for (int oloGx = 0; oloGx < 4; ++oloGx)                                                    \
            {                                                                                          \
                vec2 oloCorner = (UV) + (((vec2(float(oloGx), float(oloGy)) * 2.0) - 3.5) * (TEXEL));  \
                vec4 oloLanes = textureGather(SAMPLER, oloCorner, 2);                                  \
                oloCoverageSum += (oloLanes.x + oloLanes.y) + (oloLanes.z + oloLanes.w);               \
            }                                                                                          \
        }                                                                                              \
        OUT_MEAN = oloCoverageSum * (1.0 / 64.0);                                                      \
    }

// The COAT'S OWN share of the 8x8 coverage mean around UV (#1627): the
// coverage of the texels holding the stochastic mark, summed over all 64. The
// window is OLO_TAA_MEAN_COVERAGE_8X8's. SAMPLER is a name, not a value.
#define OLO_TAA_MEAN_COAT_COVERAGE_8X8(SAMPLER, UV, TEXEL, OUT_MEAN)                                       \
    {                                                                                                       \
        float oloCoatCoverageSum = 0.0;                                                                     \
        for (int oloGy = 0; oloGy < 4; ++oloGy)                                                             \
        {                                                                                                   \
            for (int oloGx = 0; oloGx < 4; ++oloGx)                                                         \
            {                                                                                               \
                vec2 oloCorner = (UV) + (((vec2(float(oloGx), float(oloGy)) * 2.0) - 3.5) * (TEXEL));       \
                vec4 oloCoat = step(textureGather(SAMPLER, oloCorner, 3), vec4(0.5 * OLO_STOCHASTIC_COVERAGE_MARK)); \
                oloCoatCoverageSum += dot(textureGather(SAMPLER, oloCorner, 2), oloCoat);                   \
            }                                                                                               \
        }                                                                                                   \
        OUT_MEAN = oloCoatCoverageSum * (1.0 / 64.0);                                                       \
    }

// The colour statistics of the 5x5 texels around UV, in YCoCg, for the clip
// box of a stochastic coat pixel (#1627; see step 3). SAMPLER is a name.
#define OLO_TAA_GATHER_5X5(SAMPLER, UV, TEXEL, OUT_STATS)                                          \
    {                                                                                              \
        vec3 oloG5M1 = vec3(0.0);                                                                  \
        vec3 oloG5M2 = vec3(0.0);                                                                  \
        vec3 oloG5Min = vec3(1.0e10);                                                              \
        vec3 oloG5Max = vec3(-1.0e10);                                                             \
        for (int oloG5Y = -2; oloG5Y <= 2; ++oloG5Y)                                               \
        {                                                                                          \
            for (int oloG5X = -2; oloG5X <= 2; ++oloG5X)                                           \
            {                                                                                      \
                vec3 oloG5S = OloRGBToYCoCg(                                                       \
                    texture(SAMPLER, (UV) + vec2(float(oloG5X), float(oloG5Y)) * (TEXEL)).rgb);    \
                oloG5M1 += oloG5S;                                                                 \
                oloG5M2 += oloG5S * oloG5S;                                                        \
                oloG5Min = min(oloG5Min, oloG5S);                                                  \
                oloG5Max = max(oloG5Max, oloG5S);                                                  \
            }                                                                                      \
        }                                                                                          \
        (OUT_STATS).Mean = oloG5M1 / 25.0;                                                         \
        (OUT_STATS).StdDev = sqrt(max(vec3(0.0), oloG5M2 / 25.0 - (OUT_STATS).Mean * (OUT_STATS).Mean)); \
        (OUT_STATS).MinC = oloG5Min;                                                               \
        (OUT_STATS).MaxC = oloG5Max;                                                               \
    }

// The mean velocity of the stochastic coat texels in the 4x4 around UV, in
// eight gathers (the window OLO_TAA_COVERAGE_KIND_4X4 reads), or FALLBACK when
// none of them holds the mark. A stochastic coat's pixel shows whichever strand
// survived this draw's hashed test, and each strand writes its own velocity, so
// the closest-depth velocity GetDilatedVelocityUV picks is itself a draw: two
// draws of the same frame reprojected into different history. The mean of the
// coat's texels is the motion the estimator converges on (#1615 / #1618, the
// Bernese walk from behind; temporal-coverage-means-not-samples.md).
vec2 OloTaaStochasticMeanVelocity(vec2 uv, vec2 fallback)
{
    vec2 sum = vec2(0.0);
    float count = 0.0;
    for (int gy = 0; gy < 2; ++gy)
    {
        for (int gx = 0; gx < 2; ++gx)
        {
            vec2 corner = uv + (((vec2(float(gx), float(gy)) * 2.0) - 0.5) * u_TexelSize);
            vec4 coat = step(textureGather(u_Velocity, corner, 3), vec4(0.5 * OLO_STOCHASTIC_COVERAGE_MARK));
            sum += vec2(dot(textureGather(u_Velocity, corner, 0), coat), dot(textureGather(u_Velocity, corner, 1), coat));
            count += dot(coat, vec4(1.0));
        }
    }
    return count > 0.0 ? sum / count : fallback;
}

void main()
{
    vec2 uv = v_TexCoord;

    // 0) What the current 4x4 says about its coverage (see step 4): whether
    // any of it is fractional, and whether any of it is a stochastic coat.
    // Read first, because a stochastic coat reprojects by its mean velocity.
    bool estimatedCoverage = false;
    bool stochasticCoverage = false;
    if (u_HasVelocityTexture != 0 && u_HasSurfaceHistory)
    {
        OLO_TAA_COVERAGE_KIND_4X4(u_Velocity, uv, u_TexelSize, estimatedCoverage, stochasticCoverage);
    }

    // 1) Sample velocity (G-Buffer RT3) or reconstruct camera motion
    vec2 velocity;
    if (u_HasVelocityTexture != 0)
    {
        vec2 velocityUV = GetDilatedVelocityUV(uv);
        vec4 dilated = texture(u_Velocity, velocityUV);
        velocity = dilated.rg;
        // A stochastic coat's closest-depth velocity is one draw's strand
        // (OloTaaStochasticMeanVelocity). Measured on the Bernese walk from
        // behind, two draws of the same frames then differ on the face by 0.633
        // of what the samples alone do, 0.665 by the closest strand; ears 0.576
        // from 0.613, fringe 0.524 from 0.566, and the lag 0.248 from 0.251.
        //
        // Only where the dilated texel IS the coat: the 4x4 that flags the
        // pixel reaches a texel past the 3x3 the dilation reads, and an opaque
        // surface in front of the coat wins the dilation too. Such a pixel
        // keeps its own surface's motion, or it would reproject the background
        // beside a walking coat by the coat's.
        if (stochasticCoverage && dilated.a <= 0.5 * OLO_STOCHASTIC_COVERAGE_MARK)
        {
            velocity = OloTaaStochasticMeanVelocity(uv, velocity);
        }
    }
    else
    {
        // Forward / Forward+: camera-only reprojection. Moving objects will
        // ghost — accepted trade-off until the forward paths emit a velocity
        // buffer.
        velocity = ReconstructCameraVelocity(uv);
    }

    vec2 prevUV = uv - velocity;

    // 2) Sample current + history
    vec3 currentColor = texture(u_Current, uv).rgb;

    vec3 historyColor = texture(u_History, prevUV).rgb;

    // Guard history against sampling outside the viewport (disocclusion). Only
    // for a real history: a frame without one resolves to the current frame
    // at the blend below and is sharpened like every other frame.
    if (u_HasHistory && !OloTemporalHistoryUVValid(prevUV))
    {
        o_Color = vec4(currentColor, 1.0);
        o_History = o_Color;
        return;
    }

    // 3) 3x3 neighborhood variance clip (in YCoCg — reduces chroma artefacts).
    // Variance clip is tighter than min/max: it avoids excessive ghosting while
    // keeping thin-feature coverage. 1.25 is a common tuning.
    //
    // The clip is now a true clip toward the box centre rather than the
    // componentwise clamp this pass used to do — a rejected history now
    // desaturates along the segment instead of being able to land on a hue the
    // neighbourhood never contained. See include/TemporalResolve.glsl.
    //
    // A STOCHASTIC COAT'S BOX COMES FROM 5x5 AT 1.0 (#1627). Its current
    // frame is one draw: each pixel a different strand or a hole, so the
    // mean and spread of nine texels are themselves a draw, and clipping to
    // them pressed this frame's noise into the history. Twenty-five texels
    // estimate the box with 40% less noise (sqrt(9/25)), and the tighter gamma keeps a
    // real change as quick to let go as the 3x3 at 1.25 was. On the Bernese
    // walk from behind (two draws of the same frames against the samples
    // alone; bar 0.5), with the coverage term below: face 0.648 -> 0.492,
    // ears 0.545 -> 0.481, fringe 0.530 -> 0.458, lag 0.25 -> 0.32; the
    // still hair's gain 11.9x -> 12.7x, under upscale 7.1x -> 11.0x
    // (Quality) and 4.3x -> 8.0x (Performance); a coverage halving still
    // 0.49 stale a frame on (0.51). The 5x5 at 1.25 cut the noise more
    // (13.0x) and let go of that halving a frame later (0.34 stale after
    // four frames, 0.29 before); 2.0 at 3x3 changed little (face 0.501).
    OloTemporalStats stats;
    float clipGamma = 1.25;
    if (stochasticCoverage)
    {
        OLO_TAA_GATHER_5X5(u_Current, uv, u_TexelSize, stats);
        clipGamma = 1.0;
    }
    else
    {
        OLO_TEMPORAL_GATHER_3X3(u_Current, uv, u_TexelSize, stats);
    }
    vec3 clampedHistory = OloYCoCgToRGB(OloTemporalClipHistory(OloRGBToYCoCg(historyColor), stats, clipGamma));

    // 4) Feedback-weighted blend. Scale feedback down when velocity is large
    // to reduce ghosting around fast motion. The "motion" must be measured
    // in *pixels*, not UV — and with a sub-pixel dead zone. It was written
    // against the Halton jitter delta (always ~1 px frame-to-frame), which
    // every velocity carried until #1552 took it out at the writers; a still
    // camera now reads zero, and the dead zone keeps sub-pixel motion from
    // dragging feedback toward 0.5.
    //
    // Velocity is in UV space; divide by TexelSize to get pixels. The dead
    // zone ramp starts at 1 px (anything sub-pixel = static, no ghosting
    // risk) and saturates at ~5 px (definitely real motion).
    //
    // A STOCHASTIC ESTIMATOR KEEPS ITS FEEDBACK IN MOTION (#1552). The ramp
    // trades history for the current frame, which is a good trade where the
    // current frame is exact. Where it is one draw of noise it is not: a
    // stochastic coat's velocity is each strand's own, so lowering its
    // feedback threw away the stable signal and kept the noise. On the dog's
    // walk seen from behind the resolve kept 0.51 of what the samples alone
    // shimmer between two draws of the same frames on the tail and the sparse
    // fringe, and keeps 0.41 and 0.45 without the ramp there; how far it lags
    // the walk went from 0.37 to 0.44 of the distance to four frames back. The
    // colour clip and the coverage term below still bound the history.
    //
    // STOCHASTIC, NOT MERELY FRACTIONAL: an alpha-tested leaf writes
    // fractional coverage too, and its current frame is exact. Exempting it
    // left the meadow's over-blurred control ghosting where it should blur
    // (TemporalSubjectSequence's detail test), so the exemption keys on the
    // mark a stochastic writer leaves in the profile channel, read from the
    // current frame's 4x4 as the coverage term reads its coverage (step 0).
    vec2 velocityPixels = velocity / u_TexelSize;
    float effectiveFeedback =
        stochasticCoverage ? u_Feedback : OloTemporalMotionFeedback(u_Feedback, velocityPixels, 1.0, 5.0, 0.5);

    // 4b) COVERAGE AND PROFILE CONFIDENCE (#1256).
    //
    // This used to be a hard-coded 1.0 — TAA had no history-rejection term at
    // all. It now runs the shared separated model over G-Buffer RT3's two new
    // channels: .b coverage, .a material profile.
    //
    // ONLY those two terms. The model's third cause, surface motion, is
    // ALREADY applied above as `effectiveFeedback` via
    // OloTemporalMotionFeedback, and applying it again here would count the
    // same motion twice — feedback would fall as the square of the motion ramp
    // and a pan would lose far more history than either mechanism intends.
    // MotionMaxReactivity = 0 is how that is expressed: it zeroes the motion
    // term in the shared evaluator rather than hand-assembling the product
    // here, so this pass cannot drift from the model's definition of the other
    // two.
    float confidence = 1.0;
    if (u_HasVelocityTexture != 0 && u_HasSurfaceHistory)
    {
        vec4 currentSurface = texture(u_Velocity, uv);
        vec4 previousSurface = texture(u_PrevSurface, prevUV);

        // COMPARE COVERAGE AGAINST A NEIGHBOURHOOD, NOT A POINT.
        //
        // A point-vs-point coverage delta is unusable here, and measurably so:
        // on a STATIC camera 54.8 % of coverage-bearing foliage pixels exceeded
        // the 0.12 dead band (median 0.149). Disabling TAA's jitter halved that
        // to 0.067 / 39.4 %, which identifies the two culprits — neither of
        // which is the subject changing:
        //
        //   * JITTER. TAA jitters the projection, so RT3 is rasterised at a
        //     different sub-pixel offset every frame, and until #1552 every
        //     velocity carried the jitter delta, so `prevUV` landed
        //     off-texel-centre. `texture()` then bilinearly mixed four texels
        //     of a high-frequency coverage field, which at a blade edge is a
        //     completely different number from the point sample at `uv`. The
        //     raster still moves with the jitter, so the coverage at `uv`
        //     does too.
        //   * MOTION. Wind moves a leaf, so the reprojected fetch legitimately
        //     lands on different coverage — but motion is ALREADY handled by
        //     `effectiveFeedback`, so letting it through here is the same
        //     double-count MotionMaxReactivity = 0 exists to prevent, arriving
        //     through a second door.
        //
        // So the question is not "did the number change" but "is this pixel's
        // coverage still WITHIN the range the neighbourhood held last frame".
        // A resample — from jitter or from motion — lands inside that range by
        // construction. A genuine LOD step or an alpha flip moves the whole
        // neighbourhood and lands outside it.
        //
        // Clamping the CURRENT coverage into the previous 3x3 range and handing
        // the clamp back as `previousSurface.b` keeps the shared evaluator's
        // definition intact: |current - previous| becomes exactly the distance
        // OUTSIDE the range, and zero inside it. The screen-space resampling
        // concern stays here, in the pass that owns the reprojection, instead
        // of being baked into the model every other consumer shares.
        //
        // ...UNLESS AN ESTIMATOR IS AT WORK: THEN COMPARE ITS MEAN (#1552).
        //
        // The range test assumes coverage is a field that only moves. A
        // STOCHASTIC coat's is not: each frame a different strand survives at
        // each pixel, or none does, so every pixel draws a fresh sample and the
        // 3x3 range is a sample of nine draws. On TemporalSubjectSequence's
        // still hair the range test fired on 1.8 % of the coat's pixels every
        // frame (mean reactivity 0.014), mostly a hole whose nine neighbours
        // all drew a strand, and each firing threw a converged pixel back to
        // one sample: the resolve kept 7.5x of the 12x it reaches without the
        // term. It is also blind to what it is for. The holes put 0 in every
        // range, so halving every strand's width, which halves the coat's
        // coverage, moved 15 % of its pixels out of range.
        //
        // What a LOD step changes is the estimator's MEAN, and an 8x8 box mean
        // carries an eighth of its per-pixel noise. On the same capture the two
        // frames' means crossed the dead band on 0.1 % of the coat's pixels at
        // rest (mean reactivity under 0.0001), and on 92 % at that width step
        // (0.40, against the range test's 0.07). A neighbourhood of only 0s and
        // 1s, an opaque edge, keeps the range test: jitter moves its edge by up
        // to a texel, which shifts a box mean by up to an eighth and fired on
        // 0.6 % of a sphere's pixels, where the range test fires on none. The
        // decision, made above with the feedback, reads the current frame,
        // whose texels are exact.
        //
        // ...AND OF A STOCHASTIC COAT, THE COAT'S OWN SHARE OF THAT MEAN
        // (#1627). The window is reprojected by the coat's mean velocity, so
        // the coat holds still in it; whatever else it holds -- the lawn at 0,
        // the body at 1 -- moves by its own, and at a moving silhouette that
        // moved the mean past the dead band with no change to the coat at
        // all: on the Bernese walk from behind, the term cost the face 0.648
        // against 0.509 without it. The coat's share (coverage of the marked
        // texels, over all 64) still moves at a real step -- halving every
        // width halves the alphas, halving the strands halves the marked
        // texels -- and TemporalSubjectSequence reads the same to the digit
        // with it: the halving 0.508 stale a frame on, the still hair 11.9x.
        if (estimatedCoverage && stochasticCoverage)
        {
            OLO_TAA_MEAN_COAT_COVERAGE_8X8(u_Velocity, uv, u_TexelSize, currentSurface.b);
            OLO_TAA_MEAN_COAT_COVERAGE_8X8(u_PrevSurface, prevUV, u_TexelSize, previousSurface.b);
        }
        else if (estimatedCoverage)
        {
            OLO_TAA_MEAN_COVERAGE_8X8(u_Velocity, uv, u_TexelSize, currentSurface.b);
            OLO_TAA_MEAN_COVERAGE_8X8(u_PrevSurface, prevUV, u_TexelSize, previousSurface.b);
        }
        else
        {
            float prevCoverageMin = 1.0;
            float prevCoverageMax = 0.0;
            for (int cy = -1; cy <= 1; ++cy)
            {
                for (int cx = -1; cx <= 1; ++cx)
                {
                    float c = texture(u_PrevSurface, prevUV + vec2(float(cx), float(cy)) * u_TexelSize).b;
                    prevCoverageMin = min(prevCoverageMin, c);
                    prevCoverageMax = max(prevCoverageMax, c);
                }
            }
            previousSurface.b = clamp(currentSurface.b, prevCoverageMin, prevCoverageMax);
        }

        OloSurfaceHistoryRecord currentRecord;
        currentRecord.LinearDepth = 0.0;
        currentRecord.GeometricNormal = vec3(0.0, 0.0, 1.0);
        currentRecord.ShadingNormal = currentRecord.GeometricNormal;
        currentRecord.Roughness = 0.0;
        currentRecord.MaterialClass = 0u;
        currentRecord.Coverage = currentSurface.b;
        // max(.a, 0): the stochastic mark is not a profile, and a coat pixel
        // that shows what lies behind it next frame has not changed material.
        currentRecord.MaterialProfile = max(currentSurface.a, 0.0);
        currentRecord.Motion = velocity;
        currentRecord.Instance = uvec2(0xffffffffu, 0u);
        currentRecord.Primitive = uvec2(0xffffffffu, 0u);
        currentRecord.Material = uvec2(0xffffffffu, 0u);
        currentRecord.Flags = 0u;
        currentRecord.HitDistance = 0.0;
        currentRecord.PrimitiveLocalIndex = 0xffffffffu;

        OloSurfaceHistoryRecord previousRecord = currentRecord;
        previousRecord.Coverage = previousSurface.b;
        previousRecord.MaterialProfile = max(previousSurface.a, 0.0);
        previousRecord.Motion = previousSurface.rg;

        OloTemporalReactivitySettings reactivity;
        reactivity.MotionDeadZonePixels = 1.0;
        reactivity.MotionSaturationPixels = 5.0;
        reactivity.MotionMaxReactivity = 0.0; // see above — motion is already in the feedback
        reactivity.CoverageNoiseDeadBand = 0.12;
        reactivity.CoverageSaturation = 0.35;
        reactivity.MaterialProfileDeadBand = 0.02;
        reactivity.MaterialProfileSaturation = 0.25;
        reactivity.PixelSize = u_TexelSize;

        confidence = OloTemporalConfidence(
            OloEvaluateTemporalReactivity(currentRecord, previousRecord, reactivity));
    }

    // No history (the first frame, or any reset: a camera cut, a projection or
    // render-scale change, a restarted sampling sequence): the resolve IS the
    // current frame. u_History holds the current frame then, reprojected by
    // this frame's velocity, and these are exactly the frames with a large
    // velocity, so blending it smeared every reset (#1348).
    vec3 resolved =
        u_HasHistory ? OloTemporalBlend(currentColor, clampedHistory, effectiveFeedback, confidence) : currentColor;

    // The history keeps the resolve, not the sharpened frame (#1533): the
    // sharpen below adds the current frame's high-pass, and fed back into the
    // history a stochastic coat's sample noise accumulated there. Only the
    // shown frame is sharpened.
    o_History = vec4(max(resolved, vec3(0.0)), 1.0);

    // 5) Optional sharpen (an unsharp mask on the current frame) to offset TAA
    //    blur, HELD INSIDE THE 3x3 RANGE IT SHARPENS (#1533). The mask runs on
    //    linear HDR, so beside one very bright sample (a sun glint, a few
    //    hundred where the surface reads about one) a pixel's 3x3 mean is far
    //    above its own value: unbounded, the mask drove it below zero and the
    //    clamp below printed it black, a black ring in the glint's bloom. The
    //    bound is the neighbourhood's range widened to hold the resolved value,
    //    so the sharpen can neither overshoot nor undershoot and a zero delta
    //    leaves the resolve untouched.
    if (u_Sharpness > 0.001)
    {
        vec3 blurred = vec3(0.0);
        vec3 lowest = currentColor;
        vec3 highest = currentColor;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                vec3 neighbour = texture(u_Current, uv + vec2(x, y) * u_TexelSize).rgb;
                blurred += neighbour;
                lowest = min(lowest, neighbour);
                highest = max(highest, neighbour);
            }
        }
        blurred /= 9.0;
        resolved = clamp(resolved + (currentColor - blurred) * u_Sharpness, min(lowest, resolved),
                         max(highest, resolved));
    }

    o_Color = vec4(max(resolved, vec3(0.0)), 1.0);
}
