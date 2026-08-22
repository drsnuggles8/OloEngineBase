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
// Output is blended back into the ping-pong chain and also written into the
// persistent history FB by the pass wrapper (via glBlitFramebuffer, no extra
// shader pass needed). On first frame (history == black) TAA decays back to
// the current frame automatically via the neighborhood clip.
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
#define u_DepthTexture OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_Current;
layout(binding = 1) uniform sampler2D u_History;
layout(binding = 2) uniform sampler2D u_Velocity;
layout(binding = 19) uniform sampler2D u_DepthTexture;
#endif

// The shared temporal kernel (issue #706). TAA was the original hand-rolled
// implementation of this; the functions below now live in one header so SSR,
// SSGI and the cloudscape resolve instantiate the same reprojection /
// neighbourhood-clip / feedback logic instead of each carrying a variant.
#include "include/TemporalResolve.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;


layout(std140, binding = 8) uniform MotionBlurMatrices
{
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

layout(std140, binding = 32) uniform TAAParams
{
    vec4 u_TAA_FeedbackSharpnessHasVelocity; // x=feedback, y=sharpness, z=hasVelocity (0/1), w=pad
    vec4 u_TAA_TexelSize;                    // xy=1/size, zw=pad
};

#define u_Feedback           (u_TAA_FeedbackSharpnessHasVelocity.x)
#define u_Sharpness          (u_TAA_FeedbackSharpnessHasVelocity.y)
#define u_HasVelocityTexture (int(u_TAA_FeedbackSharpnessHasVelocity.z))
#define u_TexelSize          (u_TAA_TexelSize.xy)

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
    return uv - prevUV; // current - prev (matches sign convention of RT3 velocity)
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

void main()
{
    vec2 uv = v_TexCoord;

    // 1) Sample velocity (G-Buffer RT3) or reconstruct camera motion
    vec2 velocity;
    if (u_HasVelocityTexture != 0)
    {
        vec2 velocityUV = GetDilatedVelocityUV(uv);
        velocity = texture(u_Velocity, velocityUV).rg;
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

    // Guard history against sampling outside the viewport (first frame / disocclusion)
    if (!OloTemporalHistoryUVValid(prevUV))
    {
        o_Color = vec4(currentColor, 1.0);
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
    OloTemporalStats stats;
    OLO_TEMPORAL_GATHER_3X3(u_Current, uv, u_TexelSize, stats);
    vec3 clampedHistory = OloYCoCgToRGB(OloTemporalClipHistory(OloRGBToYCoCg(historyColor), stats, 1.25));

    // 4) Feedback-weighted blend. Scale feedback down when velocity is large
    // to reduce ghosting around fast motion. The "motion" must be measured
    // in *pixels*, not UV — and with a sub-pixel dead zone so the Halton
    // jitter delta (always ~1 px frame-to-frame) doesn't keep dragging
    // feedback toward 0.5 even when the camera is stationary. Without the
    // dead zone TAA still half-converges, but ~10–15 % of the current
    // jittered frame bleeds through every frame, visible as a faint shake.
    //
    // Velocity is in UV space; divide by TexelSize to get pixels. The dead
    // zone ramp starts at 1 px (anything sub-pixel = static, no ghosting
    // risk) and saturates at ~5 px (definitely real motion).
    vec2 velocityPixels = velocity / u_TexelSize;
    float effectiveFeedback = OloTemporalMotionFeedback(u_Feedback, velocityPixels, 1.0, 5.0, 0.5);

    vec3 resolved = OloTemporalBlend(currentColor, clampedHistory, effectiveFeedback, 1.0);

    // 5) Optional sharpen (unsharp mask on luma) to offset TAA blur
    if (u_Sharpness > 0.001)
    {
        vec3 blurred = vec3(0.0);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                blurred += texture(u_Current, uv + vec2(x, y) * u_TexelSize).rgb;
        blurred /= 9.0;
        resolved += (currentColor - blurred) * u_Sharpness;
    }

    o_Color = vec4(max(resolved, vec3(0.0)), 1.0);
}
