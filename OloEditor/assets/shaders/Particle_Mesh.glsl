//--------------------------
// - OloEngine -
// Particle Mesh Shader
// Per-draw-call mesh particle rendering
// Single instance data via UBO (binding 3)
// --------------------------
#type vertex
#version 450 core

// Per-vertex (from mesh)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

layout(std140, binding = 3) uniform MeshInstanceData
{
	mat4 u_Model;      // 64 bytes
	vec4 u_Color;      // 16 bytes
	ivec4 u_IDs;       // 16 bytes (x = EntityID, yzw = unused)
};

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) out VertexOutput Output;
layout(location = 2) out flat int v_EntityID;

void main()
{
	vec4 worldPos = u_Model * vec4(a_Position, 1.0);
	gl_Position = u_ViewProjection * worldPos;
	Output.Color = u_Color;
	Output.TexCoord = a_TexCoord;
	v_EntityID = u_IDs.x;
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) in VertexOutput Input;
layout(location = 2) in flat int v_EntityID;

layout(binding = 0) uniform sampler2D u_Texture;
layout(binding = 1) uniform sampler2D u_DepthTexture;

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
}
