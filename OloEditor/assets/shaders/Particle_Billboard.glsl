//--------------------------
// - OloEngine -
// Particle Billboard Shader
// Instanced rendering with GPU-side billboarding
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
layout(location = 6) in int a_EntityID;
// xyz = previous-frame world position; w = previous-frame size.
layout(location = 7) in vec4 a_PrevPosition;
layout(location = 8) in float a_PrevRotation;   // previous-frame rotation (radians)
layout(location = 9) in float a_Pad0;            // padding for 16-byte instance alignment

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
	mat4 _camera_pad_view;     // View matrix (unused here, present for layout match)
	mat4 _camera_pad_proj;     // Projection matrix (same)
	vec4 _camera_pad_position; // vec3 camera pos + pad
	// Previous-frame VP combines with a_PrevPosition to emit accurate
	// per-particle motion vectors to scene FB RT3 (used by TAA).
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
	vec3 position = a_PositionSize.xyz;
	float size = a_PositionSize.w;
	float rotation = a_VelocityRotation.w;
	vec3 velocity = a_VelocityRotation.xyz;
	float stretchFactor = a_StretchFactor;

	vec3 right;
	vec3 up;

	if (stretchFactor > 0.001)
	{
		// Stretched billboard: align along velocity direction
		float speed = length(velocity);
		vec3 stretchDir = (speed > 0.001) ? velocity / speed : u_CameraUp;

		// Project stretch direction onto billboard plane
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
	}

	// Construct world position from unit quad corner offset. For the previous
	// frame we reconstruct a prev-frame quad basis from a_PrevRotation and
	// a_PrevPosition.w (prev size) so rotating / scaling particles emit the
	// correct motion vector into scene FB RT3 (TAA reprojects them cleanly).
	vec3 worldPos = position + a_QuadPos.x * right + a_QuadPos.y * up;

	vec3 prevCenter = a_PrevPosition.xyz;
	float prevSize = a_PrevPosition.w;
	float prevRotation = a_PrevRotation;
	vec3 prevRight;
	vec3 prevUp;
	if (stretchFactor > 0.001)
	{
		// Stretched path: reuse the current stretch basis but rescale to prev
		// size to approximate the quad dimensions at t-1 (velocity history is
		// not tracked, so axis direction is shared).
		float sizeRatio = (size > 0.0001) ? (prevSize / size) : 1.0;
		prevRight = right * sizeRatio;
		prevUp = up * sizeRatio;
	}
	else
	{
		float prevHalf = prevSize * 0.5;
		prevRight = u_CameraRight * prevHalf;
		prevUp = u_CameraUp * prevHalf;
		if (abs(prevRotation) > 0.001)
		{
			float cosR = cos(prevRotation);
			float sinR = sin(prevRotation);
			vec3 newRight = prevRight * cosR + prevUp * sinR;
			vec3 newUp = -prevRight * sinR + prevUp * cosR;
			prevRight = newRight;
			prevUp = newUp;
		}
	}
	vec3 prevWorldPos = prevCenter + a_QuadPos.x * prevRight + a_QuadPos.y * prevUp;
	vec4 clipCurr = u_ViewProjection     * vec4(worldPos, 1.0);
	vec4 clipPrev = u_PrevViewProjection * vec4(prevWorldPos, 1.0);
	gl_Position = clipCurr;
	v_ClipPosCurr = clipCurr;
	v_ClipPosPrev = clipPrev;

	// Interpolate UV within the sub-rect
	vec2 uv01 = a_QuadPos + vec2(0.5); // Convert [-0.5, 0.5] to [0, 1]
	Output.TexCoord = mix(a_UVRect.xy, a_UVRect.zw, uv01);
	Output.Color = a_Color;
	v_EntityID = a_EntityID;
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity — camera + per-particle motion (pos, rotation, size)
// reprojected through u_PrevViewProjection.
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
