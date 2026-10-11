#ifndef OLO_FOLIAGE_GROUP_CULL_KERNEL_GLSL
#define OLO_FOLIAGE_GROUP_CULL_KERNEL_GLSL

// The body of the foliage PATCH cull, shared by FoliageGroupCull.comp (every
// view) and FoliageGroupCullVsm.comp (a virtual-shadow-map clip level, which
// defines OLO_FOLIAGE_GROUP_CULL_VSM first). See FoliageGroupCull.comp for
// what the kernel does and why its bounds are conservative.

#include "FoliageCullCommon.glsl"
#include "GPUReadbackStats.glsl"

#ifdef OLO_FOLIAGE_GROUP_CULL_VSM
#include "VirtualShadowCommon.glsl"
layout(std430, binding = 70) readonly buffer VSMFamilyHPB
{
    uint b_HPB[];
};
#include "VirtualShadowPageFootprint.glsl"

// Does any page under this group's footprint in the clip level get redrawn
// this frame? s_VsmLocalToClip is the level's MATH-flavour projection composed
// with the terrain transform (terrain-local -> clip), so the group box needs
// no transform of its own. The rectangle is VSM_InvalidatePages.comp's.
bool oloFoliageGroupTouchesDirtyPage(vec3 lo, vec3 hi)
{
    vec2 ndcMin = vec2(1e30);
    vec2 ndcMax = vec2(-1e30);
    for (int corner = 0; corner < 8; ++corner)
    {
        vec3 p = vec3((corner & 1) != 0 ? hi.x : lo.x,
                      (corner & 2) != 0 ? hi.y : lo.y,
                      (corner & 4) != 0 ? hi.z : lo.z);
        vec4 clipPos = s_VsmLocalToClip * vec4(p, 1.0);
        vec2 ndc = clipPos.xy / clipPos.w;
        ndcMin = min(ndcMin, ndc);
        ndcMax = max(ndcMax, ndc);
    }
    if (any(greaterThan(ndcMin, vec2(1.0))) || any(lessThan(ndcMax, vec2(-1.0))))
        return false;
    vec2 uvMin = clamp(ndcMin * 0.5 + 0.5, vec2(0.0), vec2(0.99999));
    vec2 uvMax = clamp(ndcMax * 0.5 + 0.5, vec2(0.0), vec2(0.99999));
    ivec2 pageMin = ivec2(floor(uvMin * float(VSM_PAGE_TABLE_RESOLUTION)));
    ivec2 pageMax = ivec2(floor(uvMax * float(VSM_PAGE_TABLE_RESOLUTION)));
    return vsmFootprintHasDirtyPage(s_VsmLevel.x - 1, s_VsmLevel.yz, pageMin, pageMax);
}
#endif

void main()
{
    uint group = gl_GlobalInvocationID.x;
    if (group >= s_GroupCount)
        return;

    if (s_EmitStats != 0u)
        oloStatAdd(OLO_STAT_FoliageCullGroupsTested, 1u);

    vec3 lo;
    vec3 hi;
    oloFoliageGroupBounds(group, lo, hi);

    bool visible = oloFoliageBoxVisible(lo, hi) &&
                   oloFoliageBoxDistance(lo, hi) <= s_DistanceOrigin.w;
#ifdef OLO_FOLIAGE_GROUP_CULL_VSM
    // s_VsmLevel.x is the clip level + 1; 0 means this cull is not for a clip
    // level and the page test does not apply.
    if (visible && s_VsmLevel.x > 0)
        visible = oloFoliageGroupTouchesDirtyPage(lo, hi);
#endif

    s_Tail[group] = visible ? 1u : 0u;
    if (visible)
    {
        atomicAdd(s_GroupsVisible, 1u);
        if (s_EmitStats != 0u)
            oloStatAdd(OLO_STAT_FoliageCullGroupsVisible, 1u);
    }
}

#endif // OLO_FOLIAGE_GROUP_CULL_KERNEL_GLSL
