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
// block GroomStrandDepth.glsl reads — the VIEW lanes (Modes.x and .z) are the
// only fields this route uses that the cascade route does not.
layout(std140, binding = 7) uniform GroomShadowParams
{
	mat4 u_GroomShadowModel;
	vec4 u_GroomShadowWidth;
	// x = clip level, or local-light layer when z == 1; y = frame index (#1612);
	// z = domain: 0 a directional clip level, 1 a local-light layer (#1592).
	ivec4 u_GroomShadowModes;
	ivec4 u_GroomShadowDeformModes; // #1427: x = mode, y = simulated, z = roots, w = guide slots
	ivec4 u_GroomShadowDeformBases; // x = root base, y = slot base, z = displacement base, w = displacements
	vec4 u_GroomShadowCrossFade;    // #1612: the LOD cross-fade mask, x = t, y = role
};

// The page table this draw writes: x = domain (0 clip level, 1 local layer),
// y = the level or layer, z = the layer's raster mip (0 for a clip level).
layout(location = 0) flat out uvec3 v_VSMTarget;
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

#ifdef OLO_GROOM_CROSSFADE_MASK
	v_CrossFade = u_GroomShadowCrossFade;
	v_CrossFadeFrame = uint(u_GroomShadowModes.y);
#endif

	// ViewProjectionRaster, not ViewProjection: the raster flavour carries the
	// backend's clip-space convention, and the page resolve downstream assumes
	// it (see VirtualShadowRasterStage.glsl's single backend fork). The width
	// floor is measured in the texels this raster writes, which is what its
	// viewport is sized in.
	//
	// The view index is CLAMPED, not trusted, in both domains: it indexes a
	// fixed-size array, and a corrupt uniform would otherwise read past it --
	// undefined behaviour in a raster that writes into a shared physical pool.
	if (u_GroomShadowModes.z == 1)
	{
		// A LOCAL-LIGHT LAYER (#1592): a lamp's cube face or spot, the way
		// VSM_DepthLocal.glsl draws a mesh into it. The layer is rasterised at
		// the finest mip with a dirty page, in that mip's corner of the mip-0
		// viewport, so the width floor is a texel of THAT mip.
		uint layer = uint(clamp(u_GroomShadowModes.x, 0, VSM_MAX_LOCAL_LAYERS - 1));
		uint rasterMip = b_LocalRasterMip[layer];
		v_VSMTarget = uvec3(1u, layer, min(rasterMip, uint(VSM_LOCAL_MIP_COUNT - 1)));
		if (rasterMip >= uint(VSM_LOCAL_MIP_COUNT))
		{
			// No dirty page in this layer: every page is cached, so nothing is
			// drawn -- and never at an out-of-range mip.
			gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
			return;
		}
		// THE WIDTH FLOOR IS THE COARSER OF TWO TEXELS. The CPU cast this view a
		// share of the coat sized for the texels its receivers read
		// (u_GroomShadowWidth.z, LocalLayerReceiverResolution), and a share is a
		// promise about world coverage: so many strands, each that wide. Floored
		// at a finer raster mip instead, the same share drew strands too thin to
		// cover what it was cut for, and the shadow went sparse. Floored at the
		// receivers' texel alone, a coarser raster mip could miss texel centres.
		// The coarser of the two keeps both.
		float rasterTexels = float(VSM_LOCAL_VIRTUAL_RESOLUTION >> rasterMip);
		float floorTexels = min(rasterTexels, max(u_GroomShadowWidth.z, 1.0));
		vec4 clipPosition = oloGroomShadowRibbonPosition(b_LocalLights[layer].ViewProjectionRaster, u_GroomShadowModel,
		                                                 position, other, a_Radius, a_Side, u_GroomShadowWidth.x,
		                                                 u_GroomShadowWidth.y, floorTexels, u_GroomShadowWidth.w);
		gl_Position = vsmLocalRasterSubRect(clipPosition, rasterMip);
		return;
	}

	int clipLevel = clamp(u_GroomShadowModes.x, 0, VSM_CLIP_LEVELS - 1);
	v_VSMTarget = uvec3(0u, uint(clipLevel), 0u);
	gl_Position = oloGroomShadowRibbonPosition(u_VSMClips[clipLevel].ViewProjectionRaster, u_GroomShadowModel,
	                                           position, other, a_Radius, a_Side, u_GroomShadowWidth.x,
	                                           u_GroomShadowWidth.y, float(VSM_VIRTUAL_RESOLUTION),
	                                           u_GroomShadowWidth.w);
}
