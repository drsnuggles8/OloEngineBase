// Virtual Shadow Map depth raster — static mesh casters into LOCAL light layers
// (issue #703).
//
// The local twin of VSM_Depth.glsl. One indirect draw per caster batch covers
// every layer, with the layer travelling in the compacted instance record — the
// same trick, for the same reason (one draw per (batch, layer) would be hundreds
// of draws in a lamp-lit scene).
//
// The one thing this vertex stage does that the directional one does not is the
// SUB-RECT SCALE. A layer is mipped, the viewport is always the mip-0
// resolution, and a layer being redrawn at mip m has to land in a
// (LOCAL_RES >> m)² corner of it. See include/VirtualShadowLocalRasterStage.glsl
// for what that buys and what it does not.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): V1 engine-vertex pull. Binding 57 is the
// engine-wide vertex-pull binding; the stream is the engine `Vertex` (32 B), so
// the stride is 8 floats even though this pass only needs the position.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

#include "include/VirtualShadowResources.glsl"
#include "include/VirtualShadowDrawList.glsl"

layout(location = 0) flat out uint v_VSMLocalLayer;
layout(location = 1) flat out uint v_VSMLocalMip;

void main()
{
#ifdef OLO_PULLED_VERTEX
	int vertBase = gl_VertexIndex * 8;
	vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
	// u_VSMPassParams.x is this batch's run base, and gl_InstanceIndex rather
	// than a base-instance offset for the same reason as the directional raster:
	// gl_BaseInstance is unavailable on one of the engine's two compile routes
	// (glsl-shaders.md §5c-bis).
	VSMDrawInstance instance = b_DrawInstances[u_VSMPassParams.x + uint(gl_InstanceIndex)];
	uint layer = min(instance.LocalLayer, uint(VSM_MAX_LOCAL_LAYERS - 1));
	uint rasterMip = min(b_LocalRasterMip[layer], uint(VSM_LOCAL_MIP_COUNT - 1));

	v_VSMLocalLayer = layer;
	v_VSMLocalMip = rasterMip;

	vec4 clipPos = b_LocalLights[layer].ViewProjectionRaster * instance.Transform * vec4(a_Position, 1.0);

	// Map the face's NDC xy from [-1,1] into [-1, -1 + 2s], which puts it in the
	// lower-left (GL) / upper-left (Vulkan) (LOCAL_RES >> mip)² corner of the
	// mip-0 viewport. Written in CLIP space, before the divide, so it stays
	// perspective-correct: it is an affine map on (x, w), not on x/w.
	float s = 1.0 / float(1u << rasterMip);
	clipPos.xy = (clipPos.xy + clipPos.w) * s - clipPos.w;
	gl_Position = clipPos;
}

#type fragment
#version 460 core

#include "include/VirtualShadowLocalRasterStage.glsl"
