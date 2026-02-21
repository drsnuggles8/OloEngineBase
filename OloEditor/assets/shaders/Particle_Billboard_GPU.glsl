//--------------------------
// - OloEngine -
// GPU Particle Billboard Shader
// Reads particle data from SSBO (GPU-driven rendering via indirect draw)
// --------------------------
#type vertex
#version 450 core

// Per-vertex (unit quad)
layout(location = 0) in vec2 a_QuadPos;

// Per-particle data in SSBO
struct GPUParticle
{
	vec4 PositionLifetime;
	vec4 VelocityMaxLifetime;
	vec4 Color;
	vec4 InitialColor;
	vec4 InitialVelocitySize;
	vec4 Misc;  // x = initial size, y = rotation, z = alive, w = entityID
};

layout(std430, binding = 0) readonly buffer ParticleBuffer
{
	GPUParticle particles[];
};

layout(std430, binding = 1) readonly buffer AliveIndexBuffer
{
	uint aliveIndices[];
};

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

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

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) out VertexOutput Output;
layout(location = 2) out flat int v_EntityID;

void main()
{
	// Look up which particle this instance refers to (via compacted alive index)
	uint particleIdx = aliveIndices[gl_InstanceID];
	GPUParticle p = particles[particleIdx];

	vec3 position = p.PositionLifetime.xyz;
	float size = p.InitialVelocitySize.w;
	float rotation = p.Misc.y;
	vec3 velocity = p.VelocityMaxLifetime.xyz;
	float stretchFactor = 0.0; // GPU path: billboard only for Phase 1

	vec3 right;
	vec3 up;

	// Standard billboard
	float halfSize = size * 0.5;
	right = u_CameraRight * halfSize;
	up = u_CameraUp * halfSize;

	// Apply rotation around billboard normal
	if (abs(rotation) > 0.001)
	{
		float cosR = cos(rotation);
		float sinR = sin(rotation);
		vec3 newRight = right * cosR + up * sinR;
		vec3 newUp = -right * sinR + up * cosR;
		right = newRight;
		up = newUp;
	}

	// Construct world position from unit quad corner offset
	vec3 worldPos = position + a_QuadPos.x * right + a_QuadPos.y * up;
	gl_Position = u_ViewProjection * vec4(worldPos, 1.0);

	// Full [0,1] UV from the quad pos
	vec2 uv01 = a_QuadPos + vec2(0.5);
	Output.TexCoord = uv01;
	Output.Color = p.Color;
	v_EntityID = int(p.Misc.w);
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
