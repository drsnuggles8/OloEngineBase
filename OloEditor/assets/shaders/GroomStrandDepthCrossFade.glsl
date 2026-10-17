// =============================================================================
// GroomStrandDepthCrossFade.glsl — GroomStrandDepth.glsl for a coat that is
// cross-fading between two LOD tiers (#1612). The same stages with the
// cross-fade mask compiled in: ShadowRenderPass binds it for a fading caster
// only, so a steady caster never pays for a discard it does not use.
// =============================================================================

#type vertex
#version 460 core

#define OLO_GROOM_CROSSFADE_MASK 1
#include "include/GroomStrandDepthVertex.glsl"

#type fragment
#version 460 core

#define OLO_GROOM_CROSSFADE_MASK 1
#include "include/GroomStrandDepthFragment.glsl"
