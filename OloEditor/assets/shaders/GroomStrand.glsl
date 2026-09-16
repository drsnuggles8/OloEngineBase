//--------------------------
// - OloEngine -
// Groom strand visibility (issue #1246)
//
// Expands a cooked groom's curve segments into screen-facing ribbons with a
// one-pixel minimum width and a compensating alpha, then resolves that alpha
// either with a hard cutoff (OpaqueRibbon) or with a hashed stochastic test
// (StochasticAlpha). Which of the two runs is decided on the CPU by
// SelectGroomComposition and arrives as u_GroomMode; the shader never selects.
//
// NEUTRAL LIGHTING ON PURPOSE. #1246's scope is visibility, and #1247 owns
// fibre scattering. The only modulation here is a geometric root-to-tip ramp,
// which is a property of the curve rather than of any light — so a capture
// from this pass shows silhouette, overlap and coverage and nothing else. A
// lighting term added here would make every coverage measurement a measurement
// of the lighting too.
// --------------------------

#type vertex
#version 450 core

#include "include/GroomStrandCommon.glsl"

#ifdef OLO_VULKAN
// ADR 0011 §5: the Vulkan backend declares no vertex input state at all, so
// every attribute is pulled from the engine-wide vertex SSBO by index. The
// float stride below must match GroomStrandVertex in
// OloEngine/Groom/GroomStrandBuffers.h — 12 floats, 48 bytes.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;   // this vertex's centreline point, object space
layout(location = 1) in vec3 a_Other;      // this point plus the segment delta (P1-P0), object space
layout(location = 2) in float a_Side;      // -1 or +1: which edge of the ribbon
layout(location = 3) in float a_Radius;    // object-space RADIUS here (the cooked width is a diameter)
layout(location = 4) in vec2 a_Coords;     // x = root-to-tip parameter, y = across-ribbon in [-1, 1]
layout(location = 5) in float a_SegmentId; // uintBitsToFloat of the stochastic-hash segment identity
layout(location = 6) in float a_Pad0;
#endif

layout(std140, binding = 0) uniform CameraMatrices {
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 _groomCameraPadPosition;
	float _groomCameraPad0;
	mat4 u_PrevViewProjection;
};

// ONE block, on the shared PASS-LOCAL slot, declared IDENTICALLY in both
// stages.
//
// Two things are load-bearing here and both were learned the hard way:
//
//   * IDENTICAL MEMBER NAMES ACROSS STAGES. A uniform block of the same name
//     in two stages of one program must match field for field. Renaming the
//     fields one stage does not read to the underscore-prefixed "deliberately
//     unused" form fails the LINK with "struct fields mismatch between shaders
//     for uniform" — and compiling the stages separately with glslc cannot see
//     it, because it is a cross-stage interface rule that only linking checks.
//
//   * IT IS NOT UBO_MODEL. A UniformBuffer claims its binding point at
//     CONSTRUCTION and nothing rebinds it afterwards, so sharing binding 3 with
//     the engine's per-draw model UBO means whichever was constructed last owns
//     the slot: the strand pass read the scene's matrices, and every later pass
//     would have read the strand pass's. UBO_USER_0 is the slot whose contract
//     is that its occupant rebinds and refills it, which is exactly this.
layout(std140, binding = 7) uniform GroomStrandParams {
	mat4 u_GroomModel;
	mat4 u_GroomPrevModel;
	vec4 u_GroomColor;       // rgb = neutral albedo, a unused
	ivec4 u_GroomIDs;        // x = EntityID, yzw unused
	vec4 u_GroomViewport;    // xy = width/height in pixels, zw unused
	vec4 u_GroomRampWidth;   // x = ramp floor, y = width scale, z = object scale, w = alpha cutoff
	ivec4 u_GroomModeFrame;  // x = composition mode, y = frame index, z = stochastic seed, w unused
};

layout(location = 0) out vec2 v_Coords;
layout(location = 1) out float v_Alpha;
layout(location = 2) flat out uint v_SegmentId;
layout(location = 3) out vec4 v_ClipCurr;
layout(location = 4) out vec4 v_ClipPrev;
layout(location = 5) out vec3 v_ViewNormal;

void main()
{
#ifdef OLO_PULLED_VERTEX
	int base = gl_VertexIndex * 12;
	vec3 a_Position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
	vec3 a_Other = vec3(b_Vertices.v[base + 3], b_Vertices.v[base + 4], b_Vertices.v[base + 5]);
	float a_Side = b_Vertices.v[base + 6];
	float a_Radius = b_Vertices.v[base + 7];
	vec2 a_Coords = vec2(b_Vertices.v[base + 8], b_Vertices.v[base + 9]);
	float a_SegmentId = b_Vertices.v[base + 10];
#endif

	vec4 worldCurr = u_GroomModel * vec4(a_Position, 1.0);
	vec4 worldOther = u_GroomModel * vec4(a_Other, 1.0);
	vec4 clipCurr = u_ViewProjection * worldCurr;
	vec4 clipOther = u_ViewProjection * worldOther;

	// A vertex at or behind the eye has no screen position, so the whole
	// ribbon is collapsed rather than projected through a near-zero w — which
	// would throw a strand across the frame. The CPU model drops the same
	// segments (ProjectionStats::SegmentsDroppedBehindCamera), so the two stay
	// comparable at the frame edges where this bites.
	if (clipCurr.w <= 1e-6 || clipOther.w <= 1e-6)
	{
		gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped away
		v_Coords = vec2(0.0);
		v_Alpha = 0.0;
		v_SegmentId = 0u;
		v_ClipCurr = vec4(0.0, 0.0, 0.0, 1.0);
		v_ClipPrev = vec4(0.0, 0.0, 0.0, 1.0);
		v_ViewNormal = vec3(0.0, 0.0, 1.0);
		return;
	}

	vec2 viewport = max(u_GroomViewport.xy, vec2(1.0));
	vec2 screenCurr = (clipCurr.xy / clipCurr.w * 0.5 + 0.5) * viewport;
	vec2 screenOther = (clipOther.xy / clipOther.w * 0.5 + 0.5) * viewport;

	vec2 delta = screenOther - screenCurr;
	float deltaLength = length(delta);
	// A segment whose two ends land on the same pixel still has a direction to
	// widen along; +X is as good as any and keeps the quad non-degenerate.
	vec2 tangent = deltaLength > 1e-5 ? delta / deltaLength : vec2(1.0, 0.0);
	vec2 normal = vec2(-tangent.y, tangent.x);

	float radiusWorld = a_Radius * u_GroomRampWidth.y * u_GroomRampWidth.z;
	float halfWidthPixels = radiusWorld * oloGroomPixelsPerUnitAtUnitW(u_Projection, viewport.y) / clipCurr.w;
	float rasterHalfWidth = oloGroomRasterHalfWidth(halfWidthPixels);

	vec2 offsetPixels = normal * (rasterHalfWidth * a_Side);
	// Back to clip space through the same mapping the screen position came
	// from, so the widening is exact rather than approximately a pixel.
	vec2 offsetNdc = offsetPixels / viewport * 2.0;

	gl_Position = clipCurr;
	gl_Position.xy += offsetNdc * clipCurr.w;

	v_Coords = a_Coords;
	v_Alpha = oloGroomWidenedAlpha(halfWidthPixels);
	v_SegmentId = floatBitsToUint(a_SegmentId);

	// Velocity is per-vertex in CLIP space and interpolated, exactly as the
	// opaque geometry shaders do it. The widening offset is deliberately NOT
	// applied to these: a strand's motion is its centreline's motion, and
	// carrying a width-dependent offset into the velocity would make a
	// resolution change read as movement.
	v_ClipCurr = clipCurr;
	v_ClipPrev = u_PrevViewProjection * (u_GroomPrevModel * vec4(a_Position, 1.0));

	// A curve has no surface normal. The ribbon's is the best available
	// answer for an SSAO consumer: perpendicular to the strand and facing the
	// eye. Written rather than left undefined, because attachment 2 is SSAO's
	// input and an unwritten MRT output is garbage, not zero.
	vec3 segmentView = mat3(u_View) * (mat3(u_GroomModel) * (a_Other - a_Position));
	// A zero-length segment (two coincident control points) is legal in a
	// cooked groom, so the degenerate case picks an axis instead of
	// normalising a zero vector into NaNs that would poison SSAO.
	vec3 viewTangent = length(segmentView) > 1e-8 ? normalize(segmentView) : vec3(1.0, 0.0, 0.0);
	vec3 toEye = vec3(0.0, 0.0, 1.0);
	vec3 bitangent = cross(viewTangent, toEye);
	v_ViewNormal = length(bitangent) > 1e-8 ? normalize(cross(bitangent, viewTangent)) : toEye;
}

#type fragment
#version 450 core

#include "include/GroomStrandCommon.glsl"

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
layout(location = 3) out vec2 o_Velocity;
layout(location = 4) out vec4 o_SkinDiffuse;

layout(location = 0) in vec2 v_Coords;
layout(location = 1) in float v_Alpha;
layout(location = 2) flat in uint v_SegmentId;
layout(location = 3) in vec4 v_ClipCurr;
layout(location = 4) in vec4 v_ClipPrev;
layout(location = 5) in vec3 v_ViewNormal;

// ONE block, on the shared PASS-LOCAL slot, declared IDENTICALLY in both
// stages.
//
// Two things are load-bearing here and both were learned the hard way:
//
//   * IDENTICAL MEMBER NAMES ACROSS STAGES. A uniform block of the same name
//     in two stages of one program must match field for field. Renaming the
//     fields one stage does not read to the underscore-prefixed "deliberately
//     unused" form fails the LINK with "struct fields mismatch between shaders
//     for uniform" — and compiling the stages separately with glslc cannot see
//     it, because it is a cross-stage interface rule that only linking checks.
//
//   * IT IS NOT UBO_MODEL. A UniformBuffer claims its binding point at
//     CONSTRUCTION and nothing rebinds it afterwards, so sharing binding 3 with
//     the engine's per-draw model UBO means whichever was constructed last owns
//     the slot: the strand pass read the scene's matrices, and every later pass
//     would have read the strand pass's. UBO_USER_0 is the slot whose contract
//     is that its occupant rebinds and refills it, which is exactly this.
layout(std140, binding = 7) uniform GroomStrandParams {
	mat4 u_GroomModel;
	mat4 u_GroomPrevModel;
	vec4 u_GroomColor;       // rgb = neutral albedo, a unused
	ivec4 u_GroomIDs;        // x = EntityID, yzw unused
	vec4 u_GroomViewport;    // xy = width/height in pixels, zw unused
	vec4 u_GroomRampWidth;   // x = ramp floor, y = width scale, z = object scale, w = alpha cutoff
	ivec4 u_GroomModeFrame;  // x = composition mode, y = frame index, z = stochastic seed, w unused
};

vec2 octEncode(vec3 n)
{
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
	return n.xy * 0.5 + 0.5;
}

void main()
{
	float alpha = clamp(v_Alpha, 0.0, 1.0);

	if (u_GroomModeFrame.x == OLO_GROOM_MODE_STOCHASTIC_ALPHA)
	{
		// The fragment survives when the hash falls under its coverage, so the
		// expectation of the surviving set IS the coverage. gl_FragCoord is in
		// pixels with a half-pixel offset, so the floor gives an integer pixel
		// index.
		//
		// That index is NOT guaranteed to be the row the CPU coverage model calls
		// the same pixel: gl_FragCoord.y is bottom-up on OpenGL and top-down on
		// Vulkan, while GroomCoverage::ProjectGroom flips Y to match the PNG
		// convention. Deliberately not reconciled — what the estimator needs is a
		// value decorrelated per pixel, per frame and per segment, and the row
		// convention changes WHICH pixel draws which sample, not the distribution
		// or any statistic measured from it. What must agree exactly is the hash
		// FUNCTION, and GroomStrandGpuParityTest pins that against this very
		// include, texel for texel.
		uint px = uint(floor(gl_FragCoord.x));
		uint py = uint(floor(gl_FragCoord.y));
		float threshold = oloGroomStochasticHash(px, py, uint(u_GroomModeFrame.y), v_SegmentId,
		                                         uint(u_GroomModeFrame.z));
		if (threshold >= alpha)
		{
			discard;
		}
	}
	else
	{
		if (alpha < u_GroomRampWidth.w)
		{
			discard;
		}
	}

	// GEOMETRIC ramp only — see the file header. v_Coords.x is the root-to-tip
	// parameter, so a groom imported tip-first reads inverted here exactly as
	// it does in the debug preview.
	float ramp = mix(u_GroomRampWidth.x, 1.0, clamp(v_Coords.x, 0.0, 1.0));

	o_Color = vec4(u_GroomColor.rgb * ramp, 1.0);
	o_EntityID = u_GroomIDs.x;
	o_ViewNormal = octEncode(normalize(v_ViewNormal));

	vec2 ndcCurr = v_ClipCurr.xy / max(v_ClipCurr.w, 1e-6);
	vec2 ndcPrev = v_ClipPrev.xy / max(v_ClipPrev.w, 1e-6);
	o_Velocity = (ndcCurr - ndcPrev) * 0.5;

	// "No skin diffusion here." Attachment 4 is undefined unless written, and
	// an unwritten one is blurred into scene colour by SkinDiffusion.glsl.
	o_SkinDiffuse = vec4(0.0);
}
