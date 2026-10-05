#ifndef OLO_VIRTUAL_SHADOW_FAMILY_FRAGMENT_GLSL
#define OLO_VIRTUAL_SHADOW_FAMILY_FRAGMENT_GLSL
#include "VirtualShadowRasterWrite.glsl"
#include "VirtualShadowLocalRasterWrite.glsl"
layout(location = 12) flat in uvec3 v_VSMFamilyTarget;
void vsmFamilyWriteDepth()
{
    if (v_VSMFamilyTarget.x == 0u)
        vsmWriteRasterDepth(v_VSMFamilyTarget.y);
    else
        vsmWriteLocalRasterDepth(v_VSMFamilyTarget.y, v_VSMFamilyTarget.z);
}
#endif
