// =============================================================================
// GroomStrandDepth.glsl — a groom rasterised FROM THE LIGHT. Issue #1323.
//
// The sixth caster family of ShadowRenderPass, covering both targets that pass
// drives: the CSM cascades and the local-light shadow atlas. The Virtual Shadow
// Map route is a separate shader (VSM_GroomDepth.glsl) only because its
// fragment stage resolves a page table instead of writing a depth attachment;
// the vertex maths below is the same in both, and both of them exist because
// virtual-geometry-into-a-second-shadow-technique.md is explicit that a caster
// family reaches a technique only if somebody wired it there.
//
// THE ONE-TEXEL WIDTH FLOOR IS THE WHOLE MECHANISM, and it is the same argument
// groom-strand-visibility.md rule 2 makes for the main pass, at a resolution
// three orders of magnitude coarser. A 70 um hair against a cascade texel of a
// few centimetres projects to ~1e-3 of a texel: rasterised honestly it crosses
// a texel CENTRE essentially never, so an animal's whole coat casts nothing at
// all — which is exactly the state this issue found. Widened to one texel it
// casts the silhouette the coat actually has.
//
// THE COMPENSATING ALPHA HAS NO COUNTERPART HERE, AND THAT IS DELIBERATE. The
// main pass turns the widening back into coverage by weighting the fragment's
// alpha and resolving it stochastically; a depth-only target has no alpha to
// weight, and a hashed discard would be a stochastic technique with NOTHING to
// converge it — which is the fallback groom-strand-visibility.md rule 6 refuses
// outright, for the same reason, one pass over. So a widened strand casts an
// OPAQUE shadow, and the consequence is stated rather than hidden: a coat too
// sparse to fill a shadow texel is over-occluded, by at most the ratio between
// one texel and its true width. A dense coat — the case grooms exist for — has
// many strands per texel and is opaque there in reality too.
//
// A STRAND HAS NO SURFACE NORMAL, so there is no normal-offset bias to apply on
// either side of this. The receiver's half of that is in GroomStrand.glsl,
// which offsets to the coat's light-exit point instead.
//
// A BOUND COAT IS DEFORMED HERE TOO (#1427, re-landed with #1523). Its stream
// is a REST stream in each root's bind frame, so drawn as final geometry it
// would cast a heap of strands piled at the origin. The mode-1 branch below is
// GroomStrand.glsl's, current frame only, through the same
// include/GroomStrandDeform.glsl and the same lanes, so the shadow is cast by
// the coat the camera sees.
// =============================================================================

// The stages live in include/GroomStrandDepth{Vertex,Fragment}.glsl, shared with
// GroomStrandDepthCrossFade.glsl, the variant a fading coat casts through (#1612).

#type vertex
#version 460 core

#include "include/GroomStrandDepthVertex.glsl"

#type fragment
#version 460 core

#include "include/GroomStrandDepthFragment.glsl"
