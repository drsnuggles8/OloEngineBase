// Virtual Shadow Map depth raster — static mesh casters (issue #702).
//
// One indirect draw per caster batch covers every clip level: the compacted
// instance record carries its own clip level, so the vertex stage picks the
// projection per instance instead of the pass picking it per draw.
//
// The fragment stage writes nothing to the framebuffer — see
// include/VirtualShadowRasterStage.glsl for why the depth lands in an R32UI image
// through imageAtomicMin rather than in a depth attachment.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. Binding 57 is the
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

layout(location = 0) flat out uint v_VSMClipLevel;

void main()
{
#ifdef OLO_PULLED_VERTEX
	int vertBase = gl_VertexIndex * 8;
	vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
	// u_VSMPassParams.x is this batch's run base. gl_InstanceIndex is used rather
	// than a base-instance offset deliberately: gl_BaseInstance is unavailable on
	// one of the engine's two compile routes (glsl-shaders.md §5c-bis), so the
	// batch offset travels in a uniform that both routes can read.
	VSMDrawInstance instance = b_DrawInstances[u_VSMPassParams.x + uint(gl_InstanceIndex)];
	v_VSMClipLevel = instance.ClipLevel;
	gl_Position = u_VSMClips[instance.ClipLevel].ViewProjectionRaster * instance.Transform * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

#include "include/VirtualShadowRasterStage.glsl"
