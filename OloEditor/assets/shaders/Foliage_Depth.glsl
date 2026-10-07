// =============================================================================
// Foliage_Depth.glsl - Shadow depth pass for instanced foliage
//
// Matches Foliage_Instance.glsl's geometry stream and, crucially, its
// PLACEMENT: every decision about where a plant's vertices go and which of the
// layer's two shapes owns a pixel comes from include/FoliageInstanceGeometry.glsl,
// the same functions the beauty and G-Buffer stages call (issue #1233, fourth
// criterion). A shadow cast by a quad while the lit plant is an authored pine
// passes every CPU test and reads downstream as a completely different bug.
//
// The shadow camera carries the light VP; FoliageParams carries the same wind
// snapshot and animation clock used by colour. FoliageWind.glsl owns deformation.
// Octahedral cards use Foliage_Impostor_Depth.glsl, with the colour atlas cutout.
// =============================================================================

#type vertex
#version 460 core
#include "include/FoliageDepthVertexStage.glsl"

#type fragment
#version 460 core
#include "include/FoliageDepthFragmentStage.glsl"
