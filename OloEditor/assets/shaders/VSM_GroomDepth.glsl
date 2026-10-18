// =============================================================================
// VSM_GroomDepth.glsl — a groom rasterised into the Virtual Shadow Map's pages.
// Issue #1323.
//
// WHY IT EXISTS AT ALL, rather than the CSM depth shader serving both: the VSM
// has no depth attachment. Its fragment stage resolves a page table and does an
// imageAtomicMin into the physical pool (include/VirtualShadowRasterStage.glsl),
// so the two techniques differ in their FRAGMENT stage and in nothing else.
// The vertex maths is shared verbatim through include/GroomShadowWidening.glsl,
// because two copies of it would let the two techniques disagree about where a
// coat's shadow is with nothing detecting the drift —
// virtual-geometry-into-a-second-shadow-technique.md's whole subject.
//
// ONE DRAW PER VIEW, from the CPU, rather than the compacted per-instance
// record VSM_Depth.glsl reads. A groom caster is a handful of draws, not a
// GPU-driven cluster stream, so it takes the ExternalCasterRenderer seam #1149
// opened: the view rides in this shader's own params block and the pass issues
// one draw per view the coat's bounds reach -- a clip level of the sun, or
// (#1592) a local-light layer, a lamp's cube face or spot, drawn at the mip
// VSM_DepthLocal.glsl would draw a mesh there.
//
// A BOUND COAT IS DEFORMED HERE, exactly as in GroomStrandDepth.glsl (#1427,
// re-landed with #1523): the same include/GroomStrandDeform.glsl branch on the
// same lanes, or a rest stream would be rasterised as final geometry.
// =============================================================================

// The stages live in include/VSM_GroomDepth{Vertex,Fragment}.glsl, shared with
// VSM_GroomDepthCrossFade.glsl, the variant a fading coat casts through (#1612).

#type vertex
#version 460 core

#include "include/VSM_GroomDepthVertex.glsl"

#type fragment
#version 460 core

#include "include/VSM_GroomDepthFragment.glsl"
