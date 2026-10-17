// The vertex stage of GroomStrandDepth.glsl and GroomStrandDepthCrossFade.glsl;
// see GroomStrandDepth.glsl for the caster family it draws.
// The LOD cross-fade's mask (#1612) is compiled in only where
// OLO_GROOM_CROSSFADE_MASK is defined -- the *CrossFade.glsl variant, bound for a
// fading caster alone -- so every other caster keeps an empty (or discard-free)
// fragment stage and the driver's early depth test.


#include "GroomStrandCommon.glsl"
#include "GroomShadowWidening.glsl"
#include "GroomQuat.glsl"
#include "GroomStrandDeform.glsl"

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
layout(location = 8) in float a_CoverageGrowth; // unused here: the caster draws at the shadow's own width
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
// The FRAGMENT stage declares no block: what it needs (#1612's cross-fade
// mask) arrives as flat varyings, so there is still no cross-stage block to
// keep in step — the trap GroomStrand.glsl's header describes does not arise
// here.
layout(std140, binding = 7) uniform GroomShadowParams
{
	mat4 u_GroomShadowModel;  // render-relative model matrix
	vec4 u_GroomShadowWidth;  // x = width scale, y = object scale, z = target resolution in texels, w = min width in texels
	ivec4 u_GroomShadowModes; // x = VSM clip level (the VSM route only), y = frame index (#1612), zw unused
	// GPU strand deformation (#1427), mirrored from GroomStrandParams:
	// x = mode, y = simulated, z = roots, w = guide slots.
	ivec4 u_GroomShadowDeformModes;
	// x = root base, y = slot base, z = displacement base, w = displacements.
	ivec4 u_GroomShadowDeformBases;
	// The LOD cross-fade mask (#1612), GroomStrandParams' lane: x = t, y = role.
	vec4 u_GroomShadowCrossFade;
};

#ifdef OLO_GROOM_CROSSFADE_MASK
// A fading coat's two tiers cast through the masks they are drawn with (#1612).
layout(location = 0) flat out vec4 v_CrossFade;
layout(location = 1) flat out uint v_CrossFadeFrame;
#endif

void main()
{
#ifdef OLO_PULLED_VERTEX
	int base = gl_VertexIndex * 16;
	vec3 a_Position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
	vec3 a_Other = vec3(b_Vertices.v[base + 3], b_Vertices.v[base + 4], b_Vertices.v[base + 5]);
	float a_Side = b_Vertices.v[base + 6];
	float a_Radius = b_Vertices.v[base + 7];
	vec2 a_Coords = vec2(b_Vertices.v[base + 8], b_Vertices.v[base + 9]);
	vec3 a_PrevPosition = vec3(b_Vertices.v[base + 12], b_Vertices.v[base + 13], b_Vertices.v[base + 14]);
#endif

	// Final stream (mode 0): the attributes are the object-space points.
	vec3 position = a_Position;
	vec3 other = a_Other;
	if (u_GroomShadowDeformModes.x == 1)
	{
		// A BOUND coat (#1427). The stream holds this corner's endpoint and the
		// segment's OTHER endpoint in the root's bind frame, and the lanes that
		// are last frame's centreline on a final stream hold x = root slot,
		// y = the other endpoint's parameter, z = 1 at P1. `other` is rebuilt as
		// this point plus the segment's P1 - P0, exactly as GroomStrand.glsl
		// does, so the widening sees the same segment the camera does.
		OloGroomDeformLayout deform = oloGroomDeformLayout(u_GroomShadowDeformModes, u_GroomShadowDeformBases);
		uint root = uint(a_PrevPosition.x + 0.5);
		bool atP1 = a_PrevPosition.z > 0.5;
		vec3 self = oloGroomDeformPoint(deform, root, a_Position, a_Coords.x, false);
		vec3 otherEnd = oloGroomDeformPoint(deform, root, a_Other, a_PrevPosition.y, false);
		vec3 segmentDelta = atP1 ? (self - otherEnd) : (otherEnd - self);
		position = self;
		other = self + segmentDelta;
	}

	gl_Position = oloGroomShadowRibbonPosition(u_ViewProjection, u_GroomShadowModel, position, other,
	                                           a_Radius, a_Side, u_GroomShadowWidth.x, u_GroomShadowWidth.y,
	                                           u_GroomShadowWidth.z, u_GroomShadowWidth.w);
#ifdef OLO_GROOM_CROSSFADE_MASK
	v_CrossFade = u_GroomShadowCrossFade;
	v_CrossFadeFrame = uint(u_GroomShadowModes.y);
#endif
}
