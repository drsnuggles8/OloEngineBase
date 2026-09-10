#ifndef OLO_VIRTUAL_SHADOW_PAGE_FOOTPRINT_GLSL
#define OLO_VIRTUAL_SHADOW_PAGE_FOOTPRINT_GLSL

// =============================================================================
// VirtualShadowPageFootprint.glsl — "does anything under this footprint need
// redrawing?", the one test that turns a shadow cull into a page-driven one.
//
// TWO culls ask it, and they must ask it identically or one of them draws into
// pages the other already considers finished:
//   * VSM_CullCasters.comp — one (mesh caster, clip level) pair per invocation
//     (issue #702);
//   * VirtualClusterCull.comp in its VSM mode — one (virtual-geometry cluster,
//     clip level) pair (issue #1149).
//
// Zero means no page under the footprint is being redrawn this frame, so the
// caster has nothing to contribute: the level already holds valid texels for it.
// This is where a static scene's shadow cost collapses to nothing.
//
// The footprint is translated into WRAPPED page space before the lookup, because
// that is the space the pyramid was reduced in. A footprint that straddles the
// wrap seam widens to the full axis — conservative, and rare enough (it needs a
// caster spanning most of a clip level) not to be worth splitting into two rects.
//
// REQUIRES the includer to have declared `b_HPB` (SSBO_VSM_HPB, binding 70) and
// to have included VirtualShadowCommon.glsl for the constants and vsmHPBIndex.
// `pageOffset` is passed rather than read from u_VSMClips so the virtual-geometry
// cull can use this without pulling in the whole VSM globals block.
// =============================================================================

bool vsmFootprintHasDirtyPage(int clipLevel, ivec2 pageOffset, ivec2 pageMin, ivec2 pageMax)
{
    ivec2 wrappedMin = pageMin + pageOffset;
    ivec2 wrappedMax = pageMax + pageOffset;

    // Straddling the seam: widen to the whole axis rather than test two rects.
    if ((wrappedMin.x >> VSM_PAGE_TABLE_RES_LOG2) != (wrappedMax.x >> VSM_PAGE_TABLE_RES_LOG2))
    {
        wrappedMin.x = 0;
        wrappedMax.x = VSM_PAGE_TABLE_MASK;
    }
    else
    {
        wrappedMin.x &= VSM_PAGE_TABLE_MASK;
        wrappedMax.x &= VSM_PAGE_TABLE_MASK;
    }
    if ((wrappedMin.y >> VSM_PAGE_TABLE_RES_LOG2) != (wrappedMax.y >> VSM_PAGE_TABLE_RES_LOG2))
    {
        wrappedMin.y = 0;
        wrappedMax.y = VSM_PAGE_TABLE_MASK;
    }
    else
    {
        wrappedMin.y &= VSM_PAGE_TABLE_MASK;
        wrappedMax.y &= VSM_PAGE_TABLE_MASK;
    }

    // The coarsest mip at which the footprint spans at most 2x2 texels: one
    // lookup instead of a walk over every page the caster covers.
    int mip = 0;
    while (mip < VSM_HPB_MIP_COUNT - 1 &&
           (((wrappedMax.x >> mip) - (wrappedMin.x >> mip) > 1) ||
            ((wrappedMax.y >> mip) - (wrappedMin.y >> mip) > 1)))
    {
        ++mip;
    }

    ivec2 lo = wrappedMin >> mip;
    ivec2 hi = wrappedMax >> mip;
    for (int y = lo.y; y <= hi.y; ++y)
    {
        for (int x = lo.x; x <= hi.x; ++x)
        {
            if (b_HPB[vsmHPBIndex(clipLevel, mip, ivec2(x, y))] != 0u)
                return true;
        }
    }
    return false;
}

// The page rectangle a world-space sphere covers in an ORTHOGRAPHIC clip level,
// clamped to the level. `viewProjection` is the MATH flavour of that level's VP
// (never the rasterizer one — this projects and then interprets the result).
//
// Ortho-only, and that is what makes it exact rather than conservative: the map
// is linear, so a sphere's NDC extent is its radius times the per-axis scale of
// the VP, which is the length of the matrix's x / y ROW. A perspective VP would
// need the silhouette cone instead.
void vsmSphereToPageRect(mat4 viewProjection, vec3 worldCenter, float worldRadius,
                         out ivec2 outPageMin, out ivec2 outPageMax)
{
    vec4 clipPos = viewProjection * vec4(worldCenter, 1.0);
    vec2 ndcCenter = clipPos.xy / clipPos.w;

    float ndcRadiusX = worldRadius * length(vec3(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0]));
    float ndcRadiusY = worldRadius * length(vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));

    vec2 uvMin = clamp((ndcCenter - vec2(ndcRadiusX, ndcRadiusY)) * 0.5 + 0.5, vec2(0.0), vec2(0.99999));
    vec2 uvMax = clamp((ndcCenter + vec2(ndcRadiusX, ndcRadiusY)) * 0.5 + 0.5, vec2(0.0), vec2(0.99999));

    outPageMin = ivec2(floor(uvMin * float(VSM_PAGE_TABLE_RESOLUTION)));
    outPageMax = ivec2(floor(uvMax * float(VSM_PAGE_TABLE_RESOLUTION)));
}

#endif // OLO_VIRTUAL_SHADOW_PAGE_FOOTPRINT_GLSL
