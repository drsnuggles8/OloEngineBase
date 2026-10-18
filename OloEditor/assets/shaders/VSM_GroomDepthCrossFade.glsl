// =============================================================================
// VSM_GroomDepthCrossFade.glsl — VSM_GroomDepth.glsl for a coat that is
// cross-fading between two LOD tiers (#1612): the same stages with the mask
// compiled in, bound by ShadowRenderPass for a fading caster only.
// =============================================================================

#type vertex
#version 460 core

#define OLO_GROOM_CROSSFADE_MASK 1
#include "include/VSM_GroomDepthVertex.glsl"

#type fragment
#version 460 core

#define OLO_GROOM_CROSSFADE_MASK 1
#include "include/VSM_GroomDepthFragment.glsl"
