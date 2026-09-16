#type vertex
#version 460 core
#define OLO_FOLIAGE_SHADOW 1

// Nothing in this fragment stage reads the instance index — declare no
// varying for it (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning). The deferred sibling omits this define.
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageImpostorVertexStage.glsl"

#type fragment
#version 460 core
layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 2) in float v_MeshCoverage;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 7) in float v_Radius;
#include "include/FoliageParams.glsl"
// Match the colour pass's atlas view, regardless of which light casts this map.
#define u_CameraPosition u_MeshViewPos.xyz
#include "include/FoliageImpostorSampling.glsl"
void main() { ImpostorSample surface = SampleImpostorCard(); }
