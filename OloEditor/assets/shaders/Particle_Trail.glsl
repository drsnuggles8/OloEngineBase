//--------------------------
// - OloEngine -
// Particle Trail Shader
// Batched trail quad rendering with soft particle support
// --------------------------
#type vertex
#version 450 core

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

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity — camera motion only.
layout(location = 3) out vec2 o_Velocity;

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) in VertexOutput Input;
layout(location = 2) in flat int v_EntityID;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). Both slots move
// together because ParticleBatchRenderer::BindParticleTextures stages both in
// one call — converting one and leaving the other would leave the unconverted
// sampler unbound once this program builds as the bindless variant (§5c).
#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#define u_DepthTexture OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_Texture;
layout(binding = 1) uniform sampler2D u_DepthTexture;
#endif

layout(std140, binding = 2) uniform ParticleParams
{
	vec3 u_CameraRight;
	vec3 u_CameraUp;
	int u_HasTexture;
	int u_SoftParticlesEnabled;
	float u_SoftParticleDistance;
	float u_NearClip;
	float u_FarClip;
	vec2 u_ViewportSize;
};

float LinearizeDepth(float depth)
{
	float ndc = depth * 2.0 - 1.0;
	return (2.0 * u_NearClip * u_FarClip) / (u_FarClip + u_NearClip - ndc * (u_FarClip - u_NearClip));
}

void main()
{
	vec4 texColor = Input.Color;
	if (u_HasTexture != 0)
	{
		texColor *= texture(u_Texture, Input.TexCoord);
	}

	if (texColor.a < 0.001)
		discard;

	// Soft particle depth fade
	if (u_SoftParticlesEnabled != 0)
	{
		vec2 screenUV = gl_FragCoord.xy / u_ViewportSize;
		float sceneDepth = texture(u_DepthTexture, screenUV).r;
		float linearScene = LinearizeDepth(sceneDepth);
		float linearFrag = LinearizeDepth(gl_FragCoord.z);
		float depthDiff = linearScene - linearFrag;
		float fade = clamp(depthDiff / u_SoftParticleDistance, 0.0, 1.0);
		texColor.a *= fade;
	}

	if (texColor.a < 0.001)
		discard;

	o_Color = texColor;
	o_EntityID = v_EntityID;
	o_ViewNormal = vec2(-2.0);

	vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
	vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
	o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
