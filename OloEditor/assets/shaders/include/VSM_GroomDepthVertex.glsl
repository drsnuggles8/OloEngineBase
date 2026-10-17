// The vertex stage of VSM_GroomDepth.glsl and VSM_GroomDepthCrossFade.glsl.
// The LOD cross-fade's mask (#1612) is compiled in only where
// OLO_GROOM_CROSSFADE_MASK is defined -- the *CrossFade.glsl variant, bound for a
// fading caster alone -- so every other caster keeps an empty (or discard-free)
// fragment stage and the driver's early depth test.


#include "GroomStrandCommon.glsl"
#include "GroomShadowWidening.glsl"
#include "GroomQuat.glsl"
#include "GroomStrandDeform.glsl"
#include "VirtualShadowResources.glsl"

#ifdef OLO_VULKAN
// ADR 0011 §5: the Vulkan backend declares no vertex input state, so every
// attribute is pulled by index. 16 floats, matching GroomStrandVertex.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Other;
layout(location = 2) in float a_Side;
layout(location = 3) in float a_Radius;
layout(location = 4) in vec2 a_Coords;
layout(location = 5) in float a_SegmentId;
layout(location = 6) in float a_Tint;
layout(location = 7) in vec3 a_PrevPosition;
layout(location = 8) in float a_CoverageGrowth; // unused here: the caster draws at the shadow's own width
#endif

// UBO_USER_0 (7). C++ twin: ShaderBindingLayout::GroomShadowParamsUBO, the same
// block GroomStrandDepth.glsl reads — the CLIP LEVEL lane is the only field
// this route uses that the cascade route does not.
layout(std140, binding = 7) uniform GroomShadowParams
{
	mat4 u_GroomShadowModel;
	vec4 u_GroomShadowWidth;
	ivec4 u_GroomShadowModes; // x = clip level, y = frame index (#1612)
	ivec4 u_GroomShadowDeformModes; // #1427: x = mode, y = simulated, z = roots, w = guide slots
	ivec4 u_GroomShadowDeformBases; // x = root base, y = slot base, z = displacement base, w = displacements
	vec4 u_GroomShadowCrossFade;    // #1612: the LOD cross-fade mask, x = t, y = role
};

layout(location = 0) flat out uint v_VSMClipLevel;
#ifdef OLO_GROOM_CROSSFADE_MASK
// A fading coat's two tiers cast through the masks they are drawn with (#1612).
layout(location = 1) flat out vec4 v_CrossFade;
layout(location = 2) flat out uint v_CrossFadeFrame;
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

	vec3 position = a_Position;
	vec3 other = a_Other;
	if (u_GroomShadowDeformModes.x == 1)
	{
		// See GroomStrandDepth.glsl: the rest stream's corner and the segment's
		// other endpoint, both in the root's bind frame, deformed and rebuilt as
		// this point plus the segment's P1 - P0.
		OloGroomDeformLayout deform = oloGroomDeformLayout(u_GroomShadowDeformModes, u_GroomShadowDeformBases);
		uint root = uint(a_PrevPosition.x + 0.5);
		bool atP1 = a_PrevPosition.z > 0.5;
		vec3 self = oloGroomDeformPoint(deform, root, a_Position, a_Coords.x, false);
		vec3 otherEnd = oloGroomDeformPoint(deform, root, a_Other, a_PrevPosition.y, false);
		vec3 segmentDelta = atP1 ? (self - otherEnd) : (otherEnd - self);
		position = self;
		other = self + segmentDelta;
	}

	// CLAMPED, not trusted. The level indexes a fixed-size array in the globals
	// block, and a corrupt uniform would otherwise read past it — undefined
	// behaviour in a raster that writes into a shared physical pool.
	int clipLevel = clamp(u_GroomShadowModes.x, 0, VSM_CLIP_LEVELS - 1);
	v_VSMClipLevel = uint(clipLevel);
#ifdef OLO_GROOM_CROSSFADE_MASK
	v_CrossFade = u_GroomShadowCrossFade;
	v_CrossFadeFrame = uint(u_GroomShadowModes.y);
#endif

	// ViewProjectionRaster, not ViewProjection: the raster flavour carries the
	// backend's clip-space convention, and the page resolve downstream assumes
	// it (see VirtualShadowRasterStage.glsl's single backend fork). The width
	// floor is measured in VIRTUAL texels, which is what this raster's viewport
	// is sized in.
	gl_Position = oloGroomShadowRibbonPosition(u_VSMClips[clipLevel].ViewProjectionRaster, u_GroomShadowModel,
	                                           position, other, a_Radius, a_Side, u_GroomShadowWidth.x,
	                                           u_GroomShadowWidth.y, float(VSM_VIRTUAL_RESOLUTION),
	                                           u_GroomShadowWidth.w);
}

