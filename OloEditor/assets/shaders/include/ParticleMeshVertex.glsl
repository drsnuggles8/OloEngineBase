#ifndef PARTICLE_MESH_VERTEX_GLSL
#define PARTICLE_MESH_VERTEX_GLSL

// The mesh-particle VERTEX stage, shared by Particle_Mesh.glsl and Particle_Mesh_OIT.glsl (#1417).
// Include straight after the stage's #version line.

#ifdef OLO_VULKAN
// Engine V1 stream: position, normal and UV occupy eight float lanes.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
// Per-vertex (from mesh)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
#endif

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
	mat4 _camera_pad_view;
	mat4 _camera_pad_proj;
	vec4 _camera_pad_position;
	mat4 u_PrevViewProjection;
};

layout(std140, binding = 3) uniform MeshInstanceData
{
	mat4 u_Model;        // 64 bytes
	vec4 u_Color;        // 16 bytes
	ivec4 u_IDs;         // 16 bytes (x = EntityID, yzw = unused)
	mat4 u_PrevModel;    // 64 bytes — previous-frame model matrix for motion vectors
};

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) out VertexOutput Output;
layout(location = 2) out flat int v_EntityID;
layout(location = 3) out vec4 v_ClipPosCurr;
layout(location = 4) out vec4 v_ClipPosPrev;

void main()
{
#ifdef OLO_PULLED_VERTEX
	int vertBase = gl_VertexIndex * 8;
	vec3 a_Position = vec3(b_Vertices.v[vertBase], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
	vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
	vec4 worldPos = u_Model     * vec4(a_Position, 1.0);
	vec4 worldPosPrev = u_PrevModel * vec4(a_Position, 1.0);
	vec4 clipCurr = u_ViewProjection     * worldPos;
	vec4 clipPrev = u_PrevViewProjection * worldPosPrev;
	gl_Position = clipCurr;
	v_ClipPosCurr = clipCurr;
	v_ClipPosPrev = clipPrev;
	Output.Color = u_Color;
	Output.TexCoord = a_TexCoord;
	v_EntityID = u_IDs.x;
}

#endif
