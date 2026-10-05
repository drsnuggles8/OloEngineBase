#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyFragment.glsl"
#endif
void main()
{
    // Depth-only pass — no color output needed

#ifdef OLO_VSM_FAMILY
    vsmFamilyWriteDepth();
#endif
}
