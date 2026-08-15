// Shadow Depth Shader
// Used for directional and spot light shadow map generation.
// Renders geometry from the light's perspective; depth is written automatically.

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): V1 engine-vertex pull. Binding 57 is the
// engine-wide vertex-pull binding; the stream is the engine `Vertex` (32 B),
// so the stride is 8 floats even though this pass only needs the position.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

layout(std140, binding = 0) uniform CameraMatrices
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition;
	float _padding0;
};

// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

void main()
{
#ifdef OLO_PULLED_VERTEX
	int vertBase = gl_VertexIndex * 8;
	vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
	OLO_INSTANCE_FORWARD();
	gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

void main()
{
	// Depth is written automatically by the rasterizer
}
