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
// Contrast Adaptive Sharpening (CAS) — first slice of the FSR1 spatial upscaler
// (#432). Port of AMD's FidelityFX CAS (public-domain) "sharpen-only" path.
//
// CAS is an adaptive unsharp mask: it samples a 3x3 neighborhood, measures the
// local min/max, and derives a per-channel sharpening amount that is STRONG in
// low-contrast regions and BACKS OFF on already-high-contrast edges, so the
// result reads crisp without the ringing/haloing of a fixed-strength sharpen.
//
// This runs AFTER tone mapping, on the LDR ([0,1] display-range) image: the
// `2.0 - mx` contrast-headroom term and the final saturate() both assume that
// range, so running CAS pre-tonemap (in unbounded HDR) would zero out
// sharpening on any pixel brighter than mid-grey. See UpscalerRenderPass /
// RenderPipeline for the post-tonemap placement.
//
// The math is mirrored on the CPU by CASMathTest and the rendered frame is
// checked by CASVisualEvidenceTest (CLAUDE.md: math pins the formula, the
// visual test proves the frame looks right).
// =============================================================================

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture; // Tonemapped (LDR) scene color

// CAS parameters (UBO binding 44). x = sharpness in [0,1] (0 = subtle, 1 =
// maximum), y = 1/textureWidth, z = 1/textureHeight, w = pad.
layout(std140, binding = 44) uniform CASParams
{
    vec4 u_CAS_Params;
};
#define u_Sharpness (u_CAS_Params.x)
#define u_TexelSize (u_CAS_Params.yz)

vec3 SampleColor(vec2 uv)
{
    return texture(u_Texture, uv).rgb;
}

void main()
{
    vec2 uv = v_TexCoord;
    vec2 ts = u_TexelSize;

    // 3x3 neighborhood around the center pixel e:
    //   a b c
    //   d e f
    //   g h i
    vec3 a = SampleColor(uv + vec2(-ts.x, -ts.y));
    vec3 b = SampleColor(uv + vec2(0.0, -ts.y));
    vec3 c = SampleColor(uv + vec2(ts.x, -ts.y));
    vec3 d = SampleColor(uv + vec2(-ts.x, 0.0));
    vec3 e = SampleColor(uv);
    vec3 f = SampleColor(uv + vec2(ts.x, 0.0));
    vec3 g = SampleColor(uv + vec2(-ts.x, ts.y));
    vec3 h = SampleColor(uv + vec2(0.0, ts.y));
    vec3 i = SampleColor(uv + vec2(ts.x, ts.y));

    // Soft min/max: the plus (b d e f h) first, then fold in the diagonals
    // (a c g i). The two passes are summed (a factor of 2 pulled out of the
    // final weighting), matching the FidelityFX reference.
    vec3 mnRGB = min(min(min(d, e), min(f, b)), h);
    vec3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
    mnRGB += mnRGB2;

    vec3 mxRGB = max(max(max(d, e), max(f, b)), h);
    vec3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
    mxRGB += mxRGB2;

    // Contrast-adaptive amplitude: smooth distance to the signal limits
    // (0 below, 2.0 == 2*white above) divided by the smooth max. Low local
    // contrast -> amp near 1 (sharpen hard); near the [0,1] limits -> amp near
    // 0 (leave alone, no clipping/ringing). The reciprocal is guarded so a
    // pure-black neighborhood (mx == 0) yields amp == 0 rather than a NaN.
    vec3 rcpMRGB = mxRGB / max(mxRGB * mxRGB, vec3(1e-6));
    vec3 ampRGB = clamp(min(mnRGB, vec3(2.0) - mxRGB) * rcpMRGB, 0.0, 1.0);
    ampRGB = sqrt(ampRGB);

    // Sharpness coefficient: lerp the CAS peak from -1/8 (subtle) to -1/5
    // (maximum) as sharpness goes 0 -> 1.
    float peak = -1.0 / mix(8.0, 5.0, clamp(u_Sharpness, 0.0, 1.0));
    vec3 wRGB = ampRGB * peak; // <= 0

    // Filter shape (cross weights w, center 1):
    //   0 w 0
    //   w 1 w
    //   0 w 0
    // out = (e + w*(b+d+f+h)) / (1 + 4w). With w < 0 this subtracts the
    // neighbor average and renormalizes -> an edge-preserving unsharp mask.
    vec3 rcpWeightRGB = vec3(1.0) / (vec3(1.0) + 4.0 * wRGB);
    vec3 outColor = (b * wRGB + d * wRGB + f * wRGB + h * wRGB + e) * rcpWeightRGB;

    o_Color = vec4(clamp(outColor, 0.0, 1.0), 1.0);
}
