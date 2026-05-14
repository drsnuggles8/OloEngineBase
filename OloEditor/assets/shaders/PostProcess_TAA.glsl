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

layout(location = 0) in vec3 a_Position;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_Position.xy * 0.5 + 0.5;
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(binding = 0) uniform sampler2D u_Current;
layout(binding = 1) uniform sampler2D u_History;
layout(binding = 2) uniform sampler2D u_Velocity;
layout(binding = 19) uniform sampler2D u_DepthTexture;

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

vec3 RGBToYCoCg(vec3 c)
{
    float Y  = dot(c, vec3(0.25, 0.5, 0.25));
    float Co = dot(c, vec3(0.5, 0.0, -0.5));
    float Cg = dot(c, vec3(-0.25, 0.5, -0.25));
    return vec3(Y, Co, Cg);
}

vec3 YCoCgToRGB(vec3 c)
{
    float Y = c.x, Co = c.y, Cg = c.z;
    return vec3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

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
    bool historyValid = all(greaterThanEqual(prevUV, vec2(0.0))) &&
                        all(lessThanEqual(prevUV, vec2(1.0)));
    if (!historyValid)
    {
        o_Color = vec4(currentColor, 1.0);
        return;
    }

    // 3) 3x3 neighborhood variance clip (in YCoCg — reduces chroma artefacts)
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    vec3 minC = vec3(1e10);
    vec3 maxC = vec3(-1e10);
    const float N = 9.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec3 s = texture(u_Current, uv + vec2(x, y) * u_TexelSize).rgb;
            vec3 sYCoCg = RGBToYCoCg(s);
            m1 += sYCoCg;
            m2 += sYCoCg * sYCoCg;
            minC = min(minC, sYCoCg);
            maxC = max(maxC, sYCoCg);
        }
    }
    vec3 mean = m1 / N;
    vec3 variance = max(vec3(0.0), m2 / N - mean * mean);
    vec3 stddev = sqrt(variance);
    // Variance clip: tighter than min/max, avoids excessive ghosting while
    // keeping thin-feature coverage. 1.25 is a common tuning.
    vec3 aabbMin = max(minC, mean - 1.25 * stddev);
    vec3 aabbMax = min(maxC, mean + 1.25 * stddev);

    vec3 historyYCoCg = RGBToYCoCg(historyColor);
    vec3 clampedYCoCg = clamp(historyYCoCg, aabbMin, aabbMax);
    vec3 clampedHistory = YCoCgToRGB(clampedYCoCg);

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
    float motionWeight = clamp((length(velocityPixels) - 1.0) * 0.25, 0.0, 1.0);
    float effectiveFeedback = mix(u_Feedback, 0.5, motionWeight);

    vec3 resolved = mix(currentColor, clampedHistory, effectiveFeedback);

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
