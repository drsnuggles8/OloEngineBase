#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyFragment.glsl"
#endif
void main()
{
    // Depth is written automatically by the rasterizer

#ifdef OLO_VSM_FAMILY
    vsmFamilyWriteDepth();
#endif
}
