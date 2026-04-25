//--------------------------
// - OloEngine -
// Particle Billboard Shader — Weighted-Blended OIT variant (Phase 6).
//
// Shares the vertex stage with Particle_Billboard.glsl verbatim.
// Fragment stage emits accum + revealage for WB-OIT composite in
// OIT_Resolve.glsl, with no entity-ID / view-normal outputs (those
// are only written by the opaque+classic transparent path).
// --------------------------
#type vertex
#version 450 core

// Per-vertex (unit quad)
layout(location = 0) in vec2 a_QuadPos;

// Per-instance
layout(location = 1) in vec4 a_PositionSize;       // xyz = world position, w = size
layout(location = 2) in vec4 a_Color;               // rgba
layout(location = 3) in vec4 a_UVRect;              // minU, minV, maxU, maxV
layout(location = 4) in vec4 a_VelocityRotation;    // xyz = velocity, w = rotation (radians)
layout(location = 5) in float a_StretchFactor;      // 0 = billboard, >0 = stretched
layout(location = 6) in int a_EntityID;             // unused in OIT variant

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
	float ViewZ;
};

layout(location = 0) out VertexOutput Output;

void main()
{
	vec3 position = a_PositionSize.xyz;
	float size = a_PositionSize.w;
	float rotation = a_VelocityRotation.w;
	vec3 velocity = a_VelocityRotation.xyz;
	float stretchFactor = a_StretchFactor;

	vec3 right;
	vec3 up;

	if (stretchFactor > 0.001)
	{
		float speed = length(velocity);
		vec3 stretchDir = (speed > 0.001) ? velocity / speed : u_CameraUp;

		vec3 forward = normalize(cross(u_CameraRight, u_CameraUp));
		vec3 projStretch = stretchDir - dot(stretchDir, forward) * forward;
		float projLen = length(projStretch);
		projStretch = (projLen < 0.001) ? u_CameraUp : projStretch / projLen;

		float halfWidth = size * 0.5;
		float halfLength = halfWidth + speed * stretchFactor * 0.5;

		up = projStretch * halfLength;
		right = normalize(cross(projStretch, forward)) * halfWidth;
	}
	else
	{
		float halfSize = size * 0.5;
		right = u_CameraRight * halfSize;
		up = u_CameraUp * halfSize;

		if (abs(rotation) > 0.001)
		{
			float cosR = cos(rotation);
			float sinR = sin(rotation);
			vec3 newRight = right * cosR + up * sinR;
			vec3 newUp = -right * sinR + up * cosR;
			right = newRight;
			up = newUp;
		}
	}

	vec3 worldPos = position + a_QuadPos.x * right + a_QuadPos.y * up;
	vec4 clipPos = u_ViewProjection * vec4(worldPos, 1.0);
	gl_Position = clipPos;

	vec2 uv01 = a_QuadPos + vec2(0.5);
	Output.TexCoord = mix(a_UVRect.xy, a_UVRect.zw, uv01);
	Output.Color = a_Color;
	// Linear view-space depth approximation via clip-space w (works for
	// perspective cameras; orthographic would need view matrix, but
	// particles are typically rendered under perspective).
	Output.ViewZ = max(clipPos.w, 1e-3);
}

#type fragment
#version 450 core

// WB-OIT outputs. OITBuffer attachments:
//   0 : RGBA16F accum (sum of Ci*ai*wi, sum of ai*wi)
//   1 : RG16F revealage (R = product factor for (1 - ai))
layout(location = 0) out vec4 o_Accum;
layout(location = 1) out vec4 o_Revealage;

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
	float ViewZ;
};

layout(location = 0) in VertexOutput Input;

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

#include "include/OITCommon.glsl"

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

	float weight = ComputeOITWeight(texColor.a, Input.ViewZ);
	OITPack(texColor.rgb, texColor.a, weight, o_Accum, o_Revealage);
}
