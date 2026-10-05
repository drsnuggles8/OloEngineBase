#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyFragment.glsl"
#endif
layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 2) in float v_MeshCoverage;
layout(location = 3) in vec2 v_LodSeedFade; // (instance draw, thinning fade) — issue #1237
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 7) in float v_Radius;
#include "FoliageParams.glsl"
// Match the colour pass's atlas view, regardless of which light casts this map.
#define u_CameraPosition u_MeshViewPos.xyz
#include "FoliageImpostorSampling.glsl"
void main() { ImpostorSample surface = SampleImpostorCard();
#ifdef OLO_VSM_FAMILY
    vsmFamilyWriteDepth();
#endif
}
