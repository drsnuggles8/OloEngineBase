#ifndef PARTICLE_TRAIL_VERTEX_GLSL
#define PARTICLE_TRAIL_VERTEX_GLSL

// The particle-trail VERTEX stage, shared by Particle_Trail.glsl and Particle_Trail_OIT.glsl (#1417).
// Include straight after the stage's #version line.

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V7 trail pull — the 40-byte TrailVertex
// {pos3, color4, uv2, int EntityID} on the engine-wide binding 57 (10 float
// lanes; EntityID is an int lane, floatBitsToInt). Non-instanced.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in int a_EntityID;
#endif

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
	mat4 _camera_pad_view;
	mat4 _camera_pad_proj;
	vec4 _camera_pad_position;
	mat4 u_PrevViewProjection;
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
	int vertBase = gl_VertexIndex * 10;
	vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
	vec4 a_Color = vec4(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4],
	                    b_Vertices.v[vertBase + 5], b_Vertices.v[vertBase + 6]);
	vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 7], b_Vertices.v[vertBase + 8]);
	int a_EntityID = floatBitsToInt(b_Vertices.v[vertBase + 9]);
#endif
	vec4 clipCurr = u_ViewProjection     * vec4(a_Position, 1.0);
	vec4 clipPrev = u_PrevViewProjection * vec4(a_Position, 1.0);
	gl_Position = clipCurr;
	v_ClipPosCurr = clipCurr;
	v_ClipPosPrev = clipPrev;
	Output.Color = a_Color;
	Output.TexCoord = a_TexCoord;
	v_EntityID = a_EntityID;
}

#endif
