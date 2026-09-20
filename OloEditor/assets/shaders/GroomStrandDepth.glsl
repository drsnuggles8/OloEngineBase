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
// =============================================================================

#type vertex
#version 460 core

#include "include/GroomStrandCommon.glsl"
#include "include/GroomShadowWidening.glsl"

#ifdef OLO_VULKAN
// ADR 0011 §5: the Vulkan backend declares no vertex input state at all, so
// every attribute is pulled from the engine-wide vertex SSBO by index. The
// float stride must match GroomStrandVertex in
// OloEngine/Groom/GroomStrandMesh.h — 16 floats, 64 bytes — and is the same
// stride GroomStrand.glsl pulls with.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;     // this vertex's centreline point, object space
layout(location = 1) in vec3 a_Other;        // this point plus the segment delta (P1-P0), object space
layout(location = 2) in float a_Side;        // -1 or +1: which edge of the ribbon
layout(location = 3) in float a_Radius;      // object-space RADIUS (the cooked width is a diameter)
layout(location = 4) in vec2 a_Coords;
layout(location = 5) in float a_SegmentId;
layout(location = 6) in float a_Tint;
layout(location = 7) in vec3 a_PrevPosition;
layout(location = 8) in float a_Pad1;
#endif

// The shadow camera UBO ShadowRenderPass fills per item: u_ViewProjection is
// THIS cascade's or atlas entry's light VP, already render-relative. Declared
// exactly as ShadowDepth.glsl declares it — a std140 block may stop short of
// the writer's trailing members, and stopping here keeps the two depth shaders
// reading the same prefix.
layout(std140, binding = 0) uniform CameraMatrices
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition;
	float _padding0;
};

// UBO_USER_0 (7), the shared PASS-LOCAL slot, filled per item by
// ShadowRenderPass. C++ twin: ShaderBindingLayout::GroomShadowParamsUBO.
//
// The FRAGMENT stage declares nothing at all, so there is no cross-stage block
// to keep in step — the trap GroomStrand.glsl's header describes does not
// arise here.
layout(std140, binding = 7) uniform GroomShadowParams
{
	mat4 u_GroomShadowModel;  // render-relative model matrix
	vec4 u_GroomShadowWidth;  // x = width scale, y = object scale, z = target resolution in texels, w = min width in texels
	ivec4 u_GroomShadowModes; // x = VSM clip level (the VSM route only), yzw unused
};

void main()
{
#ifdef OLO_PULLED_VERTEX
	int base = gl_VertexIndex * 16;
	vec3 a_Position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
	vec3 a_Other = vec3(b_Vertices.v[base + 3], b_Vertices.v[base + 4], b_Vertices.v[base + 5]);
	float a_Side = b_Vertices.v[base + 6];
	float a_Radius = b_Vertices.v[base + 7];
#endif

	gl_Position = oloGroomShadowRibbonPosition(u_ViewProjection, u_GroomShadowModel, a_Position, a_Other,
	                                           a_Radius, a_Side, u_GroomShadowWidth.x, u_GroomShadowWidth.y,
	                                           u_GroomShadowWidth.z, u_GroomShadowWidth.w);
}

#type fragment
#version 460 core

void main()
{
	// Depth is written automatically by the rasterizer.
}
