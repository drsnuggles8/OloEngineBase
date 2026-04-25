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

// Previous-frame particle snapshot (one entry per particle slot, indexed by
// particle slot — NOT by aliveIndex). Written by Particle_Simulate.comp
// before integration and by Particle_Emit.comp on spawn.
struct PrevParticleData
{
	vec4 Position;      // xyz = prev world position, w unused
	vec4 RotationSize;  // x = prev rotation (radians), y = prev size, zw unused
};

layout(std430, binding = 14) readonly buffer PrevPositionBuffer
{
	PrevParticleData prevData[];
};

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
	mat4 _camera_pad_view;
	mat4 _camera_pad_proj;
	vec4 _camera_pad_position;
	mat4 u_PrevViewProjection;
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
layout(location = 3) out vec4 v_ClipPosCurr;
layout(location = 4) out vec4 v_ClipPosPrev;

void main()
{
	// Look up which particle this instance refers to (via compacted alive index)
	uint particleIdx = aliveIndices[gl_InstanceIndex];
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

	// Construct world position from unit quad corner offset. For the prev
	// frame we reconstruct a full quad basis from the snapshotted prev
	// rotation/size so rotating / scaling particles emit correct motion
	// vectors into scene FB RT3 (TAA reprojects them cleanly).
	vec3 worldPos = position + a_QuadPos.x * right + a_QuadPos.y * up;

	PrevParticleData prev = prevData[particleIdx];
	vec3 prevCenter = prev.Position.xyz;
	float prevRotation = prev.RotationSize.x;
	float prevSize = prev.RotationSize.y;

	float prevHalf = prevSize * 0.5;
	vec3 prevRight = u_CameraRight * prevHalf;
	vec3 prevUp = u_CameraUp * prevHalf;
	if (abs(prevRotation) > 0.001)
	{
		float cosR = cos(prevRotation);
		float sinR = sin(prevRotation);
		vec3 newRight = prevRight * cosR + prevUp * sinR;
		vec3 newUp = -prevRight * sinR + prevUp * cosR;
		prevRight = newRight;
		prevUp = newUp;
	}
	vec3 prevWorldPos = prevCenter + a_QuadPos.x * prevRight + a_QuadPos.y * prevUp;
	vec4 clipCurr = u_ViewProjection     * vec4(worldPos, 1.0);
	vec4 clipPrev = u_PrevViewProjection * vec4(prevWorldPos, 1.0);
	gl_Position = clipCurr;
	v_ClipPosCurr = clipCurr;
	v_ClipPosPrev = clipPrev;

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
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity \u2014 camera + per-particle motion (prev positions
// live in binding 14, written by Particle_Simulate.comp pre-integration).
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
	o_ViewNormal = vec2(-2.0);

	vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
	vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
	o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
