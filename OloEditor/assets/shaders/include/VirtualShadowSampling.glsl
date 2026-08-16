#ifndef OLO_VIRTUAL_SHADOW_SAMPLING_GLSL
#define OLO_VIRTUAL_SHADOW_SAMPLING_GLSL

// =============================================================================
// VirtualShadowSampling.glsl — the consumer half of the VSM contract (issue #702)
//
// The single entry point every lit shader calls. It shares
// VirtualShadowCommon.glsl's clip-level heuristic with the page-marking kernel,
// which is load-bearing: if the marker requested level L and the sampler asks for
// level L+1, the sample lands on an unallocated page and the surface silently
// reads fully lit. Neither side reimplements the choice.
//
// Two properties worth knowing before changing anything here:
//
//   * NO SEAM AT A CLIP-LEVEL BOUNDARY. Every clip level shares one world-space
//     light direction and one fixed light-space depth range; only the XY extent
//     doubles. So a world position's stored depth is the same number at every
//     level it appears in, and a fragment that switches level between frames (or
//     between neighbouring pixels) compares against the same value. This is why
//     the depth range in VirtualShadowMapSettings is camera-INDEPENDENT.
//
//   * A MISSING PAGE DEGRADES, IT DOES NOT PUNCH A HOLE. Page marking runs one
//     frame behind (it needs the depth buffer, which does not exist when the
//     shadow pass runs), so a newly disoccluded surface has no fine page yet. The
//     sampler walks to coarser levels — which are large and almost always
//     resident — so the visible worst case is one frame of blurrier shadow. That
//     fallback is the anti-pop-in mechanism, not an error path.
// =============================================================================

#define VSM_PAGE_TABLE_READONLY 1
#include "VirtualShadowResources.glsl"

// TEX_VSM_PHYSICAL. usampler2D, not sampler2D: the pool stores raw float bits so
// the raster can resolve visibility with imageAtomicMin, and a float sampler
// would reinterpret — and filter — the bit pattern.
layout(binding = 65) uniform usampler2D u_VSMPhysicalPool;

// One clip level's visibility, PCF-filtered. Returns [0,1], or -1.0 when no tap
// found a resident page (the caller's cue to try a coarser level).
//
// Every tap resolves its OWN page. A tap that crosses a page boundary lands in a
// different, possibly non-adjacent region of the physical pool, so resolving the
// page once for the kernel centre and offsetting inside it would read a
// neighbouring page's texels and draw a hard line along every page edge.
float vsmSampleLevel(vec3 worldPosRelative, int clipLevel, float depthBias)
{
    vec2 centreUV;
    float receiverDepth;
    if (!vsmProjectIntoClip(worldPosRelative, clipLevel, centreUV, receiverDepth))
        return -1.0;

    float texelUV = 1.0 / float(VSM_VIRTUAL_RESOLUTION);
    float filterUV = texelUV * max(VSM_SOFTNESS, 0.25);

    float sum = 0.0;
    int taps = 0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 tapUV = centreUV + vec2(float(x), float(y)) * filterUV;
            if (any(lessThan(tapUV, vec2(0.0))) || any(greaterThanEqual(tapUV, vec2(1.0))))
                continue;

            uint entry = b_PageTable[vsmPageIndex(clipLevel, vsmUVToWrappedPage(tapUV, clipLevel))];
            if (!vsmPageIsAllocated(entry))
                continue;

            float stored = vsmDecodeDepth(texelFetch(u_VSMPhysicalPool, vsmPhysicalTexel(tapUV, entry), 0).r);
            sum += (receiverDepth - depthBias > stored) ? 0.0 : 1.0;
            ++taps;
        }
    }

    return (taps == 0) ? -1.0 : (sum / float(taps));
}

// Directional-light visibility at a render-relative world position. 1 = lit.
// `normalRelative` is the shading normal, used for the normal offset.
float vsmShadowFactor(vec3 worldPosRelative, vec3 normalRelative)
{
    if (VSM_ENABLED == 0)
        return 1.0;

    float distance = length(worldPosRelative - u_VSMCameraPosition.xyz);
    if (distance > VSM_MAX_SHADOW_DISTANCE)
        return 1.0;

    int baseLevel = vsmClipLevelForDistance(distance, VSM_CLIP0_HALF_EXTENT, VSM_CLIP_SELECTION_BIAS);

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        int level = min(baseLevel + attempt, VSM_CLIP_LEVELS - 1);

        // Normal offset scaled by THIS level's texel size — the standard cure for
        // the acne a constant depth bias cannot reach on grazing surfaces, and it
        // has to follow the level because a level-15 texel is 4 orders of
        // magnitude larger than a level-0 one.
        vec3 offsetPos = worldPosRelative +
                         normalRelative * (VSM_NORMAL_BIAS + u_VSMClips[level].TexelWorldSize * 1.5);

        float visibility = vsmSampleLevel(offsetPos, level, VSM_DEPTH_BIAS);
        if (visibility >= 0.0)
            return visibility;

        if (level == VSM_CLIP_LEVELS - 1)
            break;
    }

    // Nothing resident at any level. Unshadowed is the right failure: a hole in
    // the page table must not read as a black patch on the ground.
    return 1.0;
}

// Debug visualisation, gated by VirtualShadowMapSettings::DebugMode.
//   1 — clip-level tint (the concentric rings should be centred on the camera)
//   2 — page address within the level (a checkerboard; a discontinuity that does
//       NOT lie on a page edge means the wraparound maths is wrong)
//   3 — residency (green resident, red requested-but-unbacked)
vec3 vsmDebugTint(vec3 worldPosRelative)
{
    const vec3 levelColors[8] = vec3[8](
        vec3(1.0, 0.2, 0.2), vec3(1.0, 0.6, 0.2), vec3(1.0, 1.0, 0.2), vec3(0.2, 1.0, 0.2),
        vec3(0.2, 1.0, 1.0), vec3(0.2, 0.4, 1.0), vec3(0.7, 0.3, 1.0), vec3(1.0, 0.3, 0.8));

    float distance = length(worldPosRelative - u_VSMCameraPosition.xyz);
    int level = vsmClipLevelForDistance(distance, VSM_CLIP0_HALF_EXTENT, VSM_CLIP_SELECTION_BIAS);

    vec2 uv;
    float depth;
    if (!vsmProjectIntoClip(worldPosRelative, level, uv, depth))
        return vec3(0.0);

    ivec2 wrapped = vsmUVToWrappedPage(uv, level);
    uint entry = b_PageTable[vsmPageIndex(level, wrapped)];

    if (VSM_DEBUG_MODE == 2)
    {
        float checker = float((wrapped.x + wrapped.y) & 1);
        return mix(vec3(0.25), vec3(0.75), checker) * levelColors[level & 7];
    }
    if (VSM_DEBUG_MODE == 3)
    {
        if (vsmPageIsAllocated(entry))
            return vec3(0.1, 0.8, 0.1);
        return vec3(0.9, 0.1, 0.1);
    }
    return levelColors[level & 7];
}

#endif // OLO_VIRTUAL_SHADOW_SAMPLING_GLSL
