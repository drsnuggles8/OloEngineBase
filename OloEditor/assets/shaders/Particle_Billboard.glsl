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

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

uniform vec3 u_CameraRight;
uniform vec3 u_CameraUp;

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) out VertexOutput Output;
layout(location = 2) out flat int v_EntityID;

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

	// Construct world position from unit quad corner offset
	vec3 worldPos = position + a_QuadPos.x * right + a_QuadPos.y * up;
	gl_Position = u_ViewProjection * vec4(worldPos, 1.0);

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

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) in VertexOutput Input;
layout(location = 2) in flat int v_EntityID;

uniform sampler2D u_Texture;
uniform int u_HasTexture;

void main()
{
	vec4 texColor = Input.Color;
	if (u_HasTexture != 0)
	{
		texColor *= texture(u_Texture, Input.TexCoord);
	}

	if (texColor.a < 0.001)
		discard;

	o_Color = texColor;
	o_EntityID = v_EntityID;
}
