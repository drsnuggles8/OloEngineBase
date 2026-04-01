// ===================================================
// Slug font rendering algorithm — GLSL 4.50 port.
// Based on reference code by Eric Lengyel (MIT License).
// ===================================================

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in vec4 a_BandTransform;
layout(location = 4) in ivec4 a_GlyphData;
layout(location = 5) in int a_EntityID;

layout(std140, binding = 0) uniform Camera
{
	mat4 u_ViewProjection;
};

layout(location = 0) out vec4 v_Color;
layout(location = 1) out vec2 v_TexCoord;
layout(location = 2) out flat vec4 v_BandTransform;
layout(location = 3) out flat ivec4 v_GlyphData;
layout(location = 4) out flat int v_EntityID;

void main()
{
	v_Color = a_Color;
	v_TexCoord = a_TexCoord;
	v_BandTransform = a_BandTransform;
	v_GlyphData = a_GlyphData;
	v_EntityID = a_EntityID;

	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

layout(location = 0) in vec4 v_Color;
layout(location = 1) in vec2 v_TexCoord;
layout(location = 2) in flat vec4 v_BandTransform;
layout(location = 3) in flat ivec4 v_GlyphData;
layout(location = 4) in flat int v_EntityID;

layout(binding = 0) uniform sampler2D u_CurveTexture;   // RGBA16F curve control points
layout(binding = 1) uniform usampler2D u_BandTexture;   // RG16UI band headers + curve lists

const int kLogBandTextureWidth = 12;

// Calculate root eligibility code from the signs of 3 control point y-coordinates.
uint CalcRootCode(float y1, float y2, float y3)
{
	uint i1 = floatBitsToUint(y1) >> 31u;
	uint i2 = floatBitsToUint(y2) >> 30u;
	uint i3 = floatBitsToUint(y3) >> 29u;

	uint shift = (i2 & 2u) | (i1 & ~2u);
	shift = (i3 & 4u) | (shift & ~4u);

	return ((0x2E74u >> shift) & 0x0101u);
}

// Solve for x-coordinates where quadratic Bézier crosses y = 0.
vec2 SolveHorizPoly(vec4 p12, vec2 p3)
{
	vec2 a = p12.xy - p12.zw * 2.0 + p3;
	vec2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.y;
	float rb = 0.5 / b.y;

	float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
	float t1 = (b.y - d) * ra;
	float t2 = (b.y + d) * ra;

	if (abs(a.y) < 1.0 / 65536.0)
	{
		t1 = p12.y * rb;
		t2 = t1;
	}

	return vec2(
		(a.x * t1 - b.x * 2.0) * t1 + p12.x,
		(a.x * t2 - b.x * 2.0) * t2 + p12.x
	);
}

// Solve for y-coordinates where quadratic Bézier crosses x = 0.
vec2 SolveVertPoly(vec4 p12, vec2 p3)
{
	vec2 a = p12.xy - p12.zw * 2.0 + p3;
	vec2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.x;
	float rb = 0.5 / b.x;

	float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
	float t1 = (b.x - d) * ra;
	float t2 = (b.x + d) * ra;

	if (abs(a.x) < 1.0 / 65536.0)
	{
		t1 = p12.x * rb;
		t2 = t1;
	}

	return vec2(
		(a.y * t1 - b.y * 2.0) * t1 + p12.y,
		(a.y * t2 - b.y * 2.0) * t2 + p12.y
	);
}

// Calculate band texture location with wrapping.
ivec2 CalcBandLoc(ivec2 glyphLoc, uint offset)
{
	ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
	bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
	bandLoc.x &= (1 << kLogBandTextureWidth) - 1;
	return bandLoc;
}

// Combine horizontal and vertical coverage.
float CalcCoverage(float xcov, float ycov, float xwgt, float ywgt)
{
	float coverage = max(
		abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
		min(abs(xcov), abs(ycov))
	);
	return clamp(coverage, 0.0, 1.0);
}

// Main Slug rendering function.
float SlugRender(vec2 renderCoord, vec4 bandTransform, ivec4 glyphData)
{
	vec2 emsPerPixel = fwidth(renderCoord);
	vec2 pixelsPerEm = 1.0 / emsPerPixel;

	ivec2 bandMax = glyphData.zw;
	bandMax.y &= 0x00FF;

	ivec2 bandIndex = clamp(
		ivec2(renderCoord * bandTransform.xy + bandTransform.zw),
		ivec2(0, 0), bandMax
	);
	ivec2 glyphLoc = glyphData.xy;

	// --- Horizontal bands (xcov) ---
	float xcov = 0.0;
	float xwgt = 0.0;

	uvec2 hbandData = texelFetch(u_BandTexture, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
	ivec2 hbandLoc = CalcBandLoc(glyphLoc, hbandData.y);

	for (int ci = 0; ci < int(hbandData.x); ci++)
	{
		ivec2 curveLoc = ivec2(texelFetch(u_BandTexture, ivec2(hbandLoc.x + ci, hbandLoc.y), 0).xy);

		vec4 p12 = texelFetch(u_CurveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
		vec2 p3 = texelFetch(u_CurveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

		if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5)
			break;

		uint code = CalcRootCode(p12.y, p12.w, p3.y);
		if (code != 0u)
		{
			vec2 r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;

			if ((code & 1u) != 0u)
			{
				xcov += clamp(r.x + 0.5, 0.0, 1.0);
				xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
			}

			if (code > 1u)
			{
				xcov -= clamp(r.y + 0.5, 0.0, 1.0);
				xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
			}
		}
	}

	// --- Vertical bands (ycov) ---
	float ycov = 0.0;
	float ywgt = 0.0;

	uvec2 vbandData = texelFetch(u_BandTexture, ivec2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0).xy;
	ivec2 vbandLoc = CalcBandLoc(glyphLoc, vbandData.y);

	for (int ci = 0; ci < int(vbandData.x); ci++)
	{
		ivec2 curveLoc = ivec2(texelFetch(u_BandTexture, ivec2(vbandLoc.x + ci, vbandLoc.y), 0).xy);

		vec4 p12 = texelFetch(u_CurveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
		vec2 p3 = texelFetch(u_CurveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

		if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5)
			break;

		uint code = CalcRootCode(p12.x, p12.z, p3.x);
		if (code != 0u)
		{
			vec2 r = SolveVertPoly(p12, p3) * pixelsPerEm.y;

			if ((code & 1u) != 0u)
			{
				ycov -= clamp(r.x + 0.5, 0.0, 1.0);
				ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
			}

			if (code > 1u)
			{
				ycov += clamp(r.y + 0.5, 0.0, 1.0);
				ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
			}
		}
	}

	return CalcCoverage(xcov, ycov, xwgt, ywgt);
}

void main()
{
	float coverage = SlugRender(v_TexCoord, v_BandTransform, v_GlyphData);

	if (coverage < 1.0 / 256.0)
		discard;

	o_Color = v_Color * coverage;
	o_EntityID = v_EntityID;
	o_ViewNormal = vec2(-2.0);
}
