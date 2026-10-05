#ifndef OLO_VIRTUAL_SHADOW_FAMILY_VERTEX_GLSL
#define OLO_VIRTUAL_SHADOW_FAMILY_VERTEX_GLSL
#include "VirtualShadowResources.glsl"
layout(std430, binding = 70) readonly buffer VSMFamilyHPB { uint b_HPB[]; };
#include "VirtualShadowPageFootprint.glsl"

// ShadowRenderPass's per-view block, distinct from VSM's compute scratch.
layout(std140, binding = 7) uniform VSMFamilyView
{
    ivec4 u_VSMFamilyView; // domain (0 clips, 1 local), level/layer
    vec4 u_VSMFamilyBoundsMin;
    vec4 u_VSMFamilyBoundsMax;
};
layout(location = 12) flat out uvec3 v_VSMFamilyTarget;

vec4 vsmFamilyPosition(vec4 clipPosition)
{
    uint domain = uint(u_VSMFamilyView.x);
    uint target = uint(u_VSMFamilyView.y);
    uint mip = 0u;
    if (domain == 0u)
    {
        ivec2 lo = ivec2(0);
        ivec2 hi = ivec2(VSM_PAGE_TABLE_MASK);
        if (u_VSMFamilyView.z == 0)
        {
            vec3 center = (u_VSMFamilyBoundsMin.xyz + u_VSMFamilyBoundsMax.xyz) * 0.5;
            float radius = length(u_VSMFamilyBoundsMax.xyz - center);
            vsmSphereToPageRect(u_VSMClips[target].ViewProjection, center, radius, lo, hi);
        }
        if (!vsmFootprintHasDirtyPage(int(target), u_VSMClips[target].PageOffset, lo, hi))
        {
            v_VSMFamilyTarget = uvec3(domain, target, mip);
            return vec4(0.0, 0.0, -2.0, 1.0);
        }
    }
    if (domain != 0u)
    {
        mip = b_LocalRasterMip[target];
        // No dirty page in this layer: never rasterise it at an invalid mip.
        if (mip >= uint(VSM_LOCAL_MIP_COUNT))
        {
            v_VSMFamilyTarget = uvec3(domain, target, 0u);
            return vec4(0.0, 0.0, -2.0, 1.0);
        }
        float scale = 1.0 / float(1u << mip);
        clipPosition.xy = (clipPosition.xy + clipPosition.w) * scale - clipPosition.w;
    }
    v_VSMFamilyTarget = uvec3(domain, target, mip);
    return clipPosition;
}
#endif
