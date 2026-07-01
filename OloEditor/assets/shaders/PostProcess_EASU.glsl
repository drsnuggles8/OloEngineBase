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
// FSR1 EASU — Edge-Adaptive Spatial Upsampling (#480, completes the #432 epic).
// Port of AMD FidelityFX FSR 1.0 EASU (MIT / permissive). EASU is the spatial
// UPSCALE stage: it reconstructs a display-resolution image from a scene that
// was rendered BELOW display resolution, using a directional (edge-adaptive)
// 12-tap Lanczos-like kernel that follows local gradients so edges stay crisp
// instead of bilinear-blurry.
//
// Placement (OloEngine, issue #480 / glsl-shaders.md §9): EASU runs EARLY, on
// the HDR scene colour, BEFORE display-res post (Bloom/DOF/ToneMap) so that
// downstream post runs at full display resolution. The paired RCAS sharpen runs
// LATE (post-tonemap) in UpscalerRenderPass.
//
// HDR note: stock FSR1 expects a perceptual (post-tonemap) input; its edge
// analysis and Lanczos weighting misbehave on unbounded HDR-linear values
// (bright highlights dominate the direction estimate and the negative lobe can
// ring hard). Because OloEngine runs EASU pre-tonemap, every tap is wrapped in a
// reversible per-channel tonemap  t = c / (1 + c)  (inverse  c = t / (1 - t)):
// the kernel runs entirely in the bounded [0,1) proxy and the result is expanded
// back to HDR at the end. This is the standard HDR-resolve trick and keeps EASU
// well-behaved on highlights. The CPU mirror EASUMathTest replicates it exactly.
//
// The source lives in the corner of a full-size target (DRS renders the reduced
// region into [0, bounds]); tap UVs are clamped to u_EASU_Bounds so EASU never
// samples uninitialised texels.
// =============================================================================

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture; // Reduced-res HDR scene colour

// EASU constants (UBO binding 45). InputSizeAndTexel.xy = rendered region size
// in pixels (output UV maps into this); .zw = source texel size (1/physW,
// 1/physH). Bounds.xy = DRS RenderScaleBounds (clamp taps to the rendered region).
layout(std140, binding = 45) uniform EASUParams
{
    vec4 u_EASU_InputSizeAndTexel;
    vec4 u_EASU_Bounds;
};

#define u_RenderSize (u_EASU_InputSizeAndTexel.xy)
#define u_SrcTexel (u_EASU_InputSizeAndTexel.zw)
#define u_Bounds (u_EASU_Bounds.xy)

// Guarded reciprocals (replace FSR's bit-hack approximations; exact + finite and
// easy to mirror on the CPU). Flat regions feed a 0 numerator alongside the 0
// denominator, so the guard floor never changes a real result.
float Rcp(float x)
{
    return 1.0 / max(x, 1e-6);
}
float Rsqrt(float x)
{
    return inversesqrt(max(x, 1e-6));
}

// Reversible per-channel tonemap proxy: HDR -> bounded [0,1) and back.
vec3 ToProxy(vec3 c)
{
    return c / (vec3(1.0) + max(c, vec3(0.0)));
}
vec3 FromProxy(vec3 t)
{
    return t / max(vec3(1.0) - t, vec3(1e-6));
}

// FSR1 luma proxy used for the edge-direction analysis (operates on the
// tonemapped proxy colour).
float Luma(vec3 c)
{
    return c.g + 0.5 * (c.r + c.b);
}

// Sample one source tap (clamped to the DRS-rendered region) as the tonemapped
// proxy colour.
vec3 SampleTap(vec2 fp, vec2 off)
{
    vec2 uv = (fp + off) * u_SrcTexel;
    uv = min(uv, u_Bounds);
    return ToProxy(texture(u_Texture, uv).rgb);
}

// Accumulate one directional Lanczos tap (FSR1 FsrEasuTap, RGB).
void EasuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
             float lob, float clp, vec3 c)
{
    vec2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    // (25/16 * (2/5 x^2 - 1)^2 - (25/16 - 1)) * (lob x^2 - 1)^2
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

// Accumulate the '+' gradient at one of the four bilinear sub-positions
// (FSR1 FsrEasuSet, scalar luma). lA..lE are the cross samples around the tap.
void EasuSet(inout vec2 dir, inout float len, vec2 pp,
             float w, float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = Rcp(lenX);
    float dirX = lD - lB;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = Rcp(lenY);
    float dirY = lE - lA;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;

    dir += vec2(dirX, dirY) * w;
    len += (lenX + lenY) * w;
}

void main()
{
    // Output position in source-pixel space (output UV maps into the rendered
    // region of u_RenderSize pixels). 'F' is the top-left of the 2x2 around the
    // resolve position; pp is the fractional offset within it.
    vec2 pp = v_TexCoord * u_RenderSize - vec2(0.5);
    vec2 fp = floor(pp);
    pp -= fp;

    // 12-tap neighbourhood (proxy colour):
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = SampleTap(fp, vec2(0.5, -0.5));
    vec3 c = SampleTap(fp, vec2(1.5, -0.5));
    vec3 e = SampleTap(fp, vec2(-0.5, 0.5));
    vec3 f = SampleTap(fp, vec2(0.5, 0.5));
    vec3 g = SampleTap(fp, vec2(1.5, 0.5));
    vec3 h = SampleTap(fp, vec2(2.5, 0.5));
    vec3 i = SampleTap(fp, vec2(-0.5, 1.5));
    vec3 j = SampleTap(fp, vec2(0.5, 1.5));
    vec3 k = SampleTap(fp, vec2(1.5, 1.5));
    vec3 l = SampleTap(fp, vec2(2.5, 1.5));
    vec3 n = SampleTap(fp, vec2(0.5, 2.5));
    vec3 o = SampleTap(fp, vec2(1.5, 2.5));

    // Per-tap luma for the direction/length analysis.
    float bL = Luma(b), cL = Luma(c);
    float eL = Luma(e), fL = Luma(f), gL = Luma(g), hL = Luma(h);
    float iL = Luma(i), jL = Luma(j), kL = Luma(k), lL = Luma(l);
    float nL = Luma(n), oL = Luma(o);

    // Accumulate direction and length over the four bilinear sub-positions.
    vec2 dir = vec2(0.0);
    float len = 0.0;
    EasuSet(dir, len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    EasuSet(dir, len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);

    // Normalize direction, clean up near-zero (flat) regions.
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = Rsqrt(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);

    // {0..2} -> {0..1}, shaped.
    len = len * 0.5;
    len *= len;

    // Stretch kernel {1 axis-aligned, sqrt(2) diagonal}.
    float stretch = (dir.x * dir.x + dir.y * dir.y) * Rcp(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = Rcp(lob);

    // Accumulate the 12 taps with the directional kernel.
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    EasuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    EasuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    EasuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    EasuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    EasuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    EasuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    EasuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    EasuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    EasuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    EasuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    EasuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    EasuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    // Normalize and de-ring: clamp to the local 2x2 min/max (HDR-safe — done in
    // the proxy range, NOT a [0,1] saturate, so highlights survive).
    vec3 proxy = aC / max(aW, 1e-6);
    vec3 mn = min(min(f, g), min(j, k));
    vec3 mx = max(max(f, g), max(j, k));
    proxy = clamp(proxy, mn, mx);

    // Expand back to HDR linear.
    o_Color = vec4(FromProxy(proxy), 1.0);
}
