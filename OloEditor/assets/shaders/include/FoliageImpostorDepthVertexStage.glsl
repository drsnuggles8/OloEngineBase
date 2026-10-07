#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyVertex.glsl"
#endif
#define OLO_FOLIAGE_SHADOW 1

// Nothing in this fragment stage reads the instance index — declare no
// varying for it (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning). The deferred sibling omits this define.
#define OLO_INSTANCE_NO_FORWARD 1
#include "FoliageImpostorVertexStage.glsl"
