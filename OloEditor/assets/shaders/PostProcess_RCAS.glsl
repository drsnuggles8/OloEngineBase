#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

// =============================================================================
// FSR1 RCAS — Robust Contrast-Adaptive Sharpening (#480). Port of AMD FidelityFX
// FSR 1.0 RCAS (MIT / permissive). RCAS is the sharpening half of FSR1: a
// contrast-limited sharpen designed to run AFTER EASU upscaling, restoring the
// high-frequency detail EASU's reconstruction softens. It is the FSR1-paired
// replacement for the standalone CAS kernel on the upscale path.
//
// Like CAS it runs post-tonemap on the LDR ([0,1] display-range) image — its
// contrast limiter and the peak-range constant assume that range. It shares the
// UpscalerRenderPass scaffold and the binding-44 UBO with CAS: x = sharpness in
// [0,1] (1 = max sharp), y = 1/texW, z = 1/texH, w = pad. UpscalerRenderPass
// selects RCAS over CAS whenever FSR1 upscaling is active.
//
// The math is mirrored on the CPU by RCASMathTest and the frame is checked by
// EASUVisualEvidenceTest (math pins the formula, the visual test proves the
// frame looks right).
// =============================================================================

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture; // Tonemapped (LDR) scene colour

layout(std140, binding = 44) uniform CASParams
{
    vec4 u_CAS_Params;
};
#define u_Sharpness (u_CAS_Params.x)
#define u_TexelSize (u_CAS_Params.yz)

// Upper bound on the sharpening lobe (FSR1 FSR_RCAS_LIMIT = 0.25 - 1/16).
const float kRcasLimit = 0.25 - (1.0 / 16.0);

float Max3(float a, float b, float c)
{
    return max(a, max(b, c));
}
float Min3(float a, float b, float c)
{
    return min(a, min(b, c));
}
// Luma in [0,1] for an LDR [0,1] colour (white -> 1.0). RCAS's peak-range
// constant assumes the signal ceiling is 1.0, so the luma MUST be normalised to
// [0,1] — FSR's "luma times 2" (g + 0.5(r+b)) would make the limiter refuse to
// sharpen anything brighter than mid-grey. This is the green-weighted half of it.
float Luma(vec3 c)
{
    return 0.5 * c.g + 0.25 * (c.r + c.b);
}

void main()
{
    vec2 uv = v_TexCoord;
    vec2 ts = u_TexelSize;

    // Minimal 3x3 cross neighbourhood:
    //    b
    //  d e f
    //    h
    vec3 b = texture(u_Texture, uv + vec2(0.0, -ts.y)).rgb;
    vec3 d = texture(u_Texture, uv + vec2(-ts.x, 0.0)).rgb;
    vec3 e = texture(u_Texture, uv).rgb;
    vec3 f = texture(u_Texture, uv + vec2(ts.x, 0.0)).rgb;
    vec3 h = texture(u_Texture, uv + vec2(0.0, ts.y)).rgb;

    float bL = Luma(b), dL = Luma(d), eL = Luma(e), fL = Luma(f), hL = Luma(h);

    // Min/max of the ring (luma).
    float mn = min(Min3(bL, dL, fL), hL);
    float mx = max(Max3(bL, dL, fL), hL);

    // Contrast limiter (peak range constant (1, -4)). The hitMin denominator
    // 4*mx is always >= 0 (magnitude guard suffices); the hitMax denominator
    // 4*mn-4 is SIGNED (negative below mid-grey, positive above), so it needs a
    // sign-preserving guard — clamping it negative would invert the sharpen on
    // every region brighter than mid-grey.
    float dMax = 4.0 * mn - 4.0;
    dMax = (dMax >= 0.0) ? max(dMax, 1e-6) : min(dMax, -1e-6);
    float hitMin = min(mn, eL) / max(4.0 * mx, 1e-6);
    float hitMax = (1.0 - max(mx, eL)) / dMax;
    float lobeL = max(-hitMin, hitMax);
    // UI sharpness in [0,1] (1 = max) -> FSR attenuation stops in [2,0] (0 = max).
    float stops = mix(2.0, 0.0, clamp(u_Sharpness, 0.0, 1.0));
    float lobe = max(-kRcasLimit, min(lobeL, 0.0)) * exp2(-stops);

    // Noise removal: back off the sharpen in flat/noisy regions.
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float range = Max3(Max3(bL, dL, eL), fL, hL) - Min3(Min3(bL, dL, eL), fL, hL);
    nz = clamp(abs(nz) / max(range, 1e-6), 0.0, 1.0);
    nz = 1.0 - 0.5 * nz;
    lobe *= nz;

    // Resolve: (lobe*(b+d+h+f) + e) / (4*lobe + 1), per channel.
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 outColor = (lobe * (b + d + h + f) + e) * rcpL;

    o_Color = vec4(clamp(outColor, 0.0, 1.0), 1.0);
}
