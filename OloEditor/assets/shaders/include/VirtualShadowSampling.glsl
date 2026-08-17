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

// The internals of ONE sampling decision, for the debug views below. It walks
// exactly the levels vsmShadowFactor walks and applies the same normal offset, so
// what it reports is what the shading actually saw — a probe that re-derived the
// level or skipped the offset would answer a question nobody asked.
#define VSM_PROBE_OK          0 // reached the depth comparison
#define VSM_PROBE_BEYOND_MAX  1 // distance > VSM_MAX_SHADOW_DISTANCE — bailed before sampling
#define VSM_PROBE_NO_PAGE     2 // no resident page at any of the three levels

struct VSMProbe
{
    int Reason;     // one of VSM_PROBE_*
    int Level;      // the level a resident page was found at, or -1
    float Receiver; // receiver depth there
    float Stored;   // depth the raster left in that page's texel (1.0 = far sentinel)
};

VSMProbe vsmProbe(vec3 worldPosRelative, vec3 normalRelative)
{
    VSMProbe probe;
    probe.Reason = VSM_PROBE_NO_PAGE;
    probe.Level = -1;
    probe.Receiver = 0.0;
    probe.Stored = 1.0;

    float dist = length(worldPosRelative - u_VSMCameraPosition.xyz);
    // Replicated from vsmShadowFactor deliberately: an early-out the probe does
    // not model is an early-out the probe cannot report, and this one silently
    // turns the whole system off.
    if (dist > VSM_MAX_SHADOW_DISTANCE)
    {
        probe.Reason = VSM_PROBE_BEYOND_MAX;
        return probe;
    }

    int baseLevel = vsmClipLevelForDistance(dist, VSM_CLIP0_HALF_EXTENT, VSM_CLIP_SELECTION_BIAS);

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        int level = min(baseLevel + attempt, VSM_CLIP_LEVELS - 1);
        vec3 offsetPos = worldPosRelative +
                         normalRelative * (VSM_NORMAL_BIAS + u_VSMClips[level].TexelWorldSize * 1.5);

        vec2 uv;
        float depth;
        if (vsmProjectIntoClip(offsetPos, level, uv, depth))
        {
            uint entry = b_PageTable[vsmPageIndex(level, vsmUVToWrappedPage(uv, level))];
            if (vsmPageIsAllocated(entry))
            {
                probe.Reason = VSM_PROBE_OK;
                probe.Level = level;
                probe.Receiver = depth;
                probe.Stored = vsmDecodeDepth(texelFetch(u_VSMPhysicalPool, vsmPhysicalTexel(uv, entry), 0).r);
                break;
            }
        }
        if (level == VSM_CLIP_LEVELS - 1)
            break;
    }
    return probe;
}

// Both depths sit within a hair of 0.5 — the ortho range is +-DepthRange metres
// (4096 by default) so a whole scene occupies ~0.1% of it. Raw, the ramps below
// would be a flat grey field; this expands roughly +-40 m around the render
// origin into the full [0,1] display range.
float vsmDebugDepthRamp(float depth)
{
    return clamp((depth - 0.5) * 100.0 + 0.5, 0.0, 1.0);
}

// Debug visualisation, gated by VirtualShadowMapSettings::DebugMode.
//   1 — clip-level tint (the concentric rings should be centred on the camera)
//   2 — page address within the level (a checkerboard; a discontinuity that does
//       NOT lie on a page edge means the wraparound maths is wrong)
//   3 — residency (green resident, red requested-but-unbacked)
//   4 — THE SHADOW DECISION, split three ways: green where the stored depth is
//       nearer than the receiver (this pixel SHOULD be shadowed), grey where it
//       is not, red where no resident page was found at any level. A frame whose
//       green region matches CSM's shadow but which still renders lit means the
//       data is right and vsmSampleLevel's comparison is wrong; an all-grey frame
//       means the raster never wrote the pages the receiver reads.
//   5 — stored depth ramp; 6 — receiver depth ramp. Same mapping, so the two are
//       directly comparable: where 5 is brighter than 6 the map holds nothing
//       nearer than the surface, which is "lit" by definition.
//   7 — the shadow factor the lit path actually receives, as greyscale. The one
//       view that is NOT a reimplementation: 4-6 walk their own copy of the level
//       search, so a frame where 4 shows a shadow and 7 does not is the sampler
//       and the probe disagreeing, and a frame where 7 shows a shadow the RENDER
//       does not have means the term is computed and then dropped by the caller.
//       Both of those happened while this system was being brought up.
vec3 vsmDebugTint(vec3 worldPosRelative, vec3 normalRelative)
{
    if (VSM_DEBUG_MODE == 7)
        return vec3(vsmShadowFactor(worldPosRelative, normalRelative));

    if (VSM_DEBUG_MODE >= 4)
    {
        VSMProbe probe = vsmProbe(worldPosRelative, normalRelative);
        if (probe.Reason == VSM_PROBE_BEYOND_MAX)
            return vec3(0.9, 0.1, 0.9); // MAGENTA — the max-distance gate ate it
        if (probe.Reason == VSM_PROBE_NO_PAGE)
            return vec3(0.9, 0.1, 0.1); // RED — nothing resident at any level
        if (VSM_DEBUG_MODE == 5)
            return vec3(vsmDebugDepthRamp(probe.Stored));
        if (VSM_DEBUG_MODE == 6)
            return vec3(vsmDebugDepthRamp(probe.Receiver));
        // Mode 4 answers all three questions in one frame: GREEN where the map
        // says shadowed, otherwise the stored-depth ramp — so pure white grey is
        // "the raster wrote nothing here" and mid-grey is "it wrote something,
        // just not nearer than the surface".
        if (probe.Receiver - VSM_DEPTH_BIAS > probe.Stored)
            return vec3(0.1, 0.9, 0.1);
        return vec3(vsmDebugDepthRamp(probe.Stored));
    }

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
