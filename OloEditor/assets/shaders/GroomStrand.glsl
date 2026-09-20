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
// FIBRE SCATTERING (#1247) IS OPT-IN, AND ITS ABSENCE IS #1246's PICTURE.
// With u_GroomFibreModes.x == 0 this shader is exactly the visibility pass it
// was: a neutral albedo modulated by a geometric root-to-tip ramp, so every
// coverage measurement #1246 made is still a measurement of coverage alone and
// every capture it committed still means what it meant. With the fibre
// material present the same fragment is lit by the scene's lights and its
// environment through the BCSDF in include/GroomFibreCommon.glsl.
//
// A STRAND HAS NO SURFACE NORMAL, so none of this is the usual surface
// shading. The model takes the strand TANGENT and the two directions, and
// depends on their azimuth DIFFERENCE about that tangent — which is what lets
// it run on a screen-facing ribbon, a shape with no meaningful binormal to
// give it. v_ViewNormal below is still written for SSAO, and is still not what
// the lighting uses.
// --------------------------

#type vertex
#version 450 core

#include "include/GroomStrandCommon.glsl"

#ifdef OLO_VULKAN
// ADR 0011 §5: the Vulkan backend declares no vertex input state at all, so
// every attribute is pulled from the engine-wide vertex SSBO by index. The
// float stride below must match GroomStrandVertex in
// OloEngine/Groom/GroomStrandMesh.h — 16 floats, 64 bytes. It was 12 until
// #1249 added the previous-frame centreline; GroomStrandMeshTest pins the C++
// size and this stride is the other half of that contract.
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
layout(location = 5) in float a_SegmentId;   // uintBitsToFloat of the stochastic-hash segment identity
layout(location = 6) in float a_Tint;       // packed coat tint, see oloGroomUnpackTint
layout(location = 7) in vec3 a_PrevPosition; // this centreline point as it was LAST frame, object space
layout(location = 8) in float a_Pad1;
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
	// Fibre scattering (#1247). The DERIVED GroomFibreParams, mirrored lane for
	// lane from UBOStructures::GroomStrandParamsUBO — see that struct for why
	// they are derived on the CPU rather than here.
	vec4 u_GroomFibreSigmaEta; // rgb = sigma_a, w = eta
	vec4 u_GroomFibreLobe;     // x = V[0], y = azimuthal scale, z = intensity, w unused
	vec4 u_GroomFibreSinAlpha; // xyz = sin(2^k alpha)
	vec4 u_GroomFibreCosAlpha; // xyz = cos(2^k alpha)
	ivec4 u_GroomFibreModes;   // x = lit, y = h-quadrature order, z = debug mode, w unused
	// Coat self-shadowing (#1248). Mirrored lane for lane from
	// UBOStructures::GroomStrandParamsUBO. These go up INACTIVE
	// (u_GroomCoatModes.x == 0) and only a draw with a built, bound volume
	// turns them on, so every way the bake can fail leaves this shader on the
	// unshadowed branch by construction.
	mat4 u_GroomCoatWorldToObject; // RIGID world -> groom object space
	vec4 u_GroomCoatBoundsMin;     // xyz = volume min (object space), w = kappa
	vec4 u_GroomCoatInvExtent;     // xyz = 1/(max-min), w = march step in world metres
	ivec4 u_GroomCoatModes;        // x = effective CoatShadowMode, y = samples the scene shadow,
	                               // z = the object box below is valid for the receiver offset (#1323)
};

layout(location = 0) out vec2 v_Coords;
layout(location = 1) out float v_Alpha;
layout(location = 2) flat out uint v_SegmentId;
layout(location = 3) out vec4 v_ClipCurr;
layout(location = 4) out vec4 v_ClipPrev;
layout(location = 5) out vec3 v_ViewNormal;
// Fibre scattering (#1247). All three are in the RENDER-RELATIVE world space
// u_ViewProjection and the light UBO already live in (issue #429), so nothing
// downstream has to shift anything back.
layout(location = 6) out vec3 v_WorldPos;
layout(location = 7) out vec3 v_WorldTangent;
// Surface -> eye, UNNORMALISED so it interpolates correctly across the ribbon.
layout(location = 8) out vec3 v_WorldView;
// The per-strand coat tint (#1251), already unpacked. FLAT: it is constant over
// the whole ribbon, and interpolating the packed lane instead would be
// arithmetic on a bit pattern.
layout(location = 9) flat out vec3 v_CoatTint;

void main()
{
#ifdef OLO_PULLED_VERTEX
	int base = gl_VertexIndex * 16;
	vec3 a_Position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
	vec3 a_Other = vec3(b_Vertices.v[base + 3], b_Vertices.v[base + 4], b_Vertices.v[base + 5]);
	float a_Side = b_Vertices.v[base + 6];
	float a_Radius = b_Vertices.v[base + 7];
	vec2 a_Coords = vec2(b_Vertices.v[base + 8], b_Vertices.v[base + 9]);
	float a_SegmentId = b_Vertices.v[base + 10];
	float a_Tint = b_Vertices.v[base + 11];
	vec3 a_PrevPosition = vec3(b_Vertices.v[base + 12], b_Vertices.v[base + 13], b_Vertices.v[base + 14]);
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
		v_WorldPos = vec3(0.0);
		v_WorldTangent = vec3(1.0, 0.0, 0.0);
		v_WorldView = vec3(0.0, 0.0, 1.0);
		v_CoatTint = vec3(1.0);
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
	//
	// a_PrevPosition, NOT a_Position (issue #1249). A groom bound to a body
	// deforms PER STRAND, so last frame's position of this point is not
	// recoverable from u_GroomPrevModel however the matrix is composed — the
	// body's pose moved, the groom's transform did not. An unbound groom writes
	// a_PrevPosition == a_Position, so this line reduces exactly to what it was
	// before the binding existed.
	v_ClipCurr = clipCurr;
	v_ClipPrev = u_PrevViewProjection * (u_GroomPrevModel * vec4(a_PrevPosition, 1.0));

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

	// The WORLD strand tangent — the fibre model's one geometric input, and a
	// different thing from v_ViewNormal above, which exists for SSAO and is a
	// fabrication (a curve has no normal). Same degenerate guard: two
	// coincident control points are legal in a cooked groom, and normalising a
	// zero vector would put a NaN into every lobe.
	vec3 segmentWorld = mat3(u_GroomModel) * (a_Other - a_Position);
	v_WorldTangent = length(segmentWorld) > 1e-8 ? normalize(segmentWorld) : vec3(1.0, 0.0, 0.0);

	v_WorldPos = worldCurr.xyz;
	// The eye, recovered from the VIEW MATRIX rather than read from the camera
	// block's position lane. The view matrix is rigid, so its inverse
	// translation is exact — and, decisively, it is in whatever space u_View is
	// in. u_ViewProjection is uploaded RENDER-RELATIVE (issue #429), so this
	// eye is render-relative too, automatically and without this shader having
	// to know the origin or trust a second lane to have been shifted the same
	// way.
	vec3 eyeWorld = -(transpose(mat3(u_View)) * u_View[3].xyz);
	v_WorldView = eyeWorld - worldCurr.xyz;
	v_CoatTint = oloGroomUnpackTint(a_Tint);
}

#type fragment
#version 450 core

#include "include/GroomStrandCommon.glsl"
#include "include/GroomFibreCommon.glsl"
#include "include/GroomCoatShadowCommon.glsl"

#include "include/BindlessHeap.glsl"

// PBRCommon supplies LightData, MAX_LIGHTS and the light-type constants the
// block below is declared in terms of. It is included for the DECLARATIONS,
// not for its BRDF: a fibre is not a surface and none of the surface shading
// in there applies to a strand.
#include "include/PBRCommon.glsl"

// Multi-Light UBO (binding 5) — THE FULL BLOCK, matching PBR_MultiLight.glsl
// and Foliage_Instance.glsl. Declared in full rather than truncated to the
// header plus Light[0]: a truncated view reads the first light correctly and
// makes the other 255 unreachable, which is the shape of bug #1234 had to fix
// in the foliage shader.
//
// Nothing binds this buffer for us and nothing has to. A UniformBuffer claims
// its binding point at construction and Renderer3D::UploadMultiLightUBO
// refills it once a frame, on GL through glBindBufferBase and on Vulkan
// through the bind-state mirror VulkanBindingState keeps for exactly this
// reason. So the strand pass reads the same lights the lit scene did.
layout(std140, binding = 5) uniform MultiLightBuffer {
	int u_LightCount;
	int u_MaxLights;
	int u_ShadowCasterCount;
	int u_DirectionalLightCount;
	LightData u_Lights[MAX_LIGHTS];
};

// ── The scene's shadow term (issue #1323) ──────────────────────────────
//
// THE OTHER DIRECTION. #1248 gave the coat its own internal occlusion — a
// density volume holding the groom's own strands and nothing else — and said
// out loud that a body casting onto its own coat is the shadow map's job. This
// block is that job: the same four shadow inputs, the same two techniques and
// the same three functions the lit surface shaders use, so a coat in shade goes
// dark for exactly the reason the body beside it does.
//
// DECLARED IDENTICALLY TO PBR_MultiLight.glsl, down to the sampler kinds. The
// four units carry a specific sampler state and a specific typed null, and
// every site that stages a shadow-map offset has to agree about both or
// whichever pass ran last silently wins (issue #691). GroomRenderPass binds
// them through CommandDispatch::BindSceneShadowTextures for the same reason —
// one publisher, not a second copy.
#ifdef OLO_BINDLESS
#define u_ShadowMapCSM OLO_HEAP_TEX_2D_ARRAY_SHADOW(8)
#define u_ShadowAtlas OLO_HEAP_TEX_2D_ARRAY_SHADOW(13)
#define u_ShadowMapCSMRaw OLO_HEAP_TEX_2D_ARRAY(33)
#define u_ShadowAtlasRaw OLO_HEAP_TEX_2D_ARRAY(34)
#else
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;  // TEX_SHADOW (CSM)
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;  // TEX_SHADOW_ATLAS
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;    // TEX_SHADOW_CSM_RAW
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;     // TEX_SHADOW_ATLAS_RAW
#endif

layout(std140, binding = 6) uniform ShadowData {
	mat4 u_DirectionalLightSpaceMatrices[4];
	vec4 u_CascadePlaneDistances;
	vec4 u_ShadowParams;  // x = bias, y = normalBias, z = softness, w = maxShadowDistance
	mat4 u_AtlasEntryMatrices[48];
	vec4 u_AtlasEntryScaleOffset[48];
	int u_DirectionalShadowEnabled;
	int u_AtlasEntryCount;
	int u_ShadowMapResolution;
	int u_AtlasResolution;
	int u_CascadeDebugEnabled;
	int u_SoftShadowMode;
	float u_AtlasDepthBias;
	int _shadowPad2;
};

// The Virtual Shadow Map's consumer half. The engine has TWO directional
// techniques and a caster family reaches a technique only if somebody wired it
// there — which is as true of the RECEIVING side as of the casting side, and
// nothing detects either gap. Including this is the receiving half of that
// wiring; ShadowRenderPass::RenderGroomVirtualShadowLevels is the casting half.
#include "include/VirtualShadowSampling.glsl"

// The camera block, declared IDENTICALLY to the vertex stage's — same name,
// same fields, same order. A uniform block of one name must match across the
// stages of a program or the LINK fails with "struct fields mismatch", and
// compiling the stages separately with glslc cannot see it. u_View is what the
// cascade selection below needs.
layout(std140, binding = 0) uniform CameraMatrices {
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 _groomCameraPadPosition;
	float _groomCameraPad0;
	mat4 u_PrevViewProjection;
};

#ifdef OLO_BINDLESS
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10) // TEX_USER_0
#else
layout(binding = 10) uniform samplerCube u_IrradianceMap; // TEX_USER_0
#endif

// The coat-shadow volume (#1248), TEX_GROOM_COAT_VOLUME. xyz = the voxel's mean
// fibre direction times its coherence, w = fibre areal density in 1/metre.
//
// ALWAYS DECLARED AND ALWAYS BOUND, even when no coat is shadowing: the pass
// binds a 1x1x1 zero volume otherwise, the way VolumetricFogPass binds a
// placeholder for its density volume. A dangling sampler is undefined
// behaviour, not a zero read, so the ROUTING (u_GroomCoatModes.x) decides
// whether the volume is sampled — never the binding.
#ifdef OLO_BINDLESS
#define u_GroomCoatVolume OLO_HEAP_TEX_3D(75) // TEX_GROOM_COAT_VOLUME
#else
layout(binding = 75) uniform sampler3D u_GroomCoatVolume; // TEX_GROOM_COAT_VOLUME
#endif

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
layout(location = 6) in vec3 v_WorldPos;
layout(location = 7) in vec3 v_WorldTangent;
layout(location = 8) in vec3 v_WorldView;
layout(location = 9) flat in vec3 v_CoatTint;

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
	// Fibre scattering (#1247). The DERIVED GroomFibreParams, mirrored lane for
	// lane from UBOStructures::GroomStrandParamsUBO — see that struct for why
	// they are derived on the CPU rather than here.
	vec4 u_GroomFibreSigmaEta; // rgb = sigma_a, w = eta
	vec4 u_GroomFibreLobe;     // x = V[0], y = azimuthal scale, z = intensity, w unused
	vec4 u_GroomFibreSinAlpha; // xyz = sin(2^k alpha)
	vec4 u_GroomFibreCosAlpha; // xyz = cos(2^k alpha)
	ivec4 u_GroomFibreModes;   // x = lit, y = h-quadrature order, z = debug mode, w unused
	// Coat self-shadowing (#1248). Mirrored lane for lane from
	// UBOStructures::GroomStrandParamsUBO. These go up INACTIVE
	// (u_GroomCoatModes.x == 0) and only a draw with a built, bound volume
	// turns them on, so every way the bake can fail leaves this shader on the
	// unshadowed branch by construction.
	mat4 u_GroomCoatWorldToObject; // RIGID world -> groom object space
	vec4 u_GroomCoatBoundsMin;     // xyz = volume min (object space), w = kappa
	vec4 u_GroomCoatInvExtent;     // xyz = 1/(max-min), w = march step in world metres
	ivec4 u_GroomCoatModes;        // x = effective CoatShadowMode, y = samples the scene shadow,
	                               // z = the object box below is valid for the receiver offset (#1323)
};

vec2 octEncode(vec3 n)
{
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
	return n.xy * 0.5 + 0.5;
}

OloGroomFibre oloGroomFibreFromUniforms()
{
	OloGroomFibre fibre;
	fibre.SigmaA = u_GroomFibreSigmaEta.rgb;
	fibre.Eta = u_GroomFibreSigmaEta.w;
	fibre.V0 = u_GroomFibreLobe.x;
	fibre.S = u_GroomFibreLobe.y;
	fibre.Intensity = u_GroomFibreLobe.z;
	fibre.Sin2kAlpha = u_GroomFibreSinAlpha.xyz;
	fibre.Cos2kAlpha = u_GroomFibreCosAlpha.xyz;
	fibre.HSamples = u_GroomFibreModes.y;
	return fibre;
}

void oloGroomAccumulate(inout OloGroomFibreLobes total, OloGroomFibreLobes add, vec3 weight)
{
	total.R += add.R * weight;
	total.TT += add.TT * weight;
	total.TRT += add.TRT * weight;
	total.Residual += add.Residual * weight;
}

// The lit fibre response at this fragment.
//
// COAT SELF-SHADOWING (#1248) MULTIPLIES THE INCOMING RADIANCE, and nothing
// else. Each light's radiance is attenuated by exp(-tau * (1 - exp(-kappa))),
// where tau is the expected number of fibre crossings between this fragment and
// that light, marched through the coat-shadow volume. That is the MEAN of the
// per-ray transmittances over the footprint, not the transmittance of the mean
// crossing count — see oloGroomCoatTransmittance and issue #1360. The BCSDF
// below is untouched.
//
// THAT IS WHERE THE DOUBLE-COUNT BOUNDARY LIVES. #1247's per-fibre
// attenuations already absorb light INSIDE one fibre, so the coat term must be
// geometric and colourless or the pigment is applied twice — which is the trap
// the issue's scope note names. tau sees no colour: it is fibre length density
// times diameter times the sine of the angle to the fibre, and nothing else.
//
// WHAT IS STILL NOT HERE: occlusion by the rest of the SCENE. A body casting
// onto its own coat is the shadow map's job, not this volume's, and the volume
// deliberately contains the groom's own strands and nothing else. With the
// coat term active the geometric root-to-tip ramp is bypassed — see main() —
// because that ramp was the crude stand-in for exactly this.
// How much of `light` reaches this strand THROUGH THE SCENE. Issue #1323.
//
// THE RECEIVER IS THE COAT'S LIGHT-EXIT POINT, NOT THE FRAGMENT, and that one
// substitution is the whole double-count fix. Once grooms are shadow CASTERS
// their own strands are in the cascade map, so a strand sampling that map at
// its own position is occluded by its own coat TWICE: once by the density
// volume in oloGroomShadeFibre and once by the map. Moving the sample to where
// the light LEAVES the coat leaves the map answering only the part the volume
// did not — is the coat's lit surface itself in shadow.
//
// IT COSTS NOTHING WHEN THERE IS NO VOLUME, and that is not a happy accident:
// oloGroomCoatLightExitDistance returns 0 for an inactive coat mode, so the
// receiver is the fragment. Which is the CORRECT answer there — with no volume
// there is no second count to remove, and the coat's own strands in the map are
// the only occlusion it gets. The offset therefore applies exactly when the
// double count exists.
//
// A STRAND HAS NO SURFACE NORMAL. The engine's receiver bias is a world-metre
// offset along the shading normal, and a ribbon's `v_ViewNormal` is written for
// SSAO rather than for shading — it faces the camera, not the surface. So the
// bias direction passed below is L: an offset TOWARDS the light, which is the
// direction the exit offset already moves in and the only direction on a fibre
// that means anything for occlusion.
float oloGroomSceneShadow(LightData light, int lightType, vec3 L)
{
	if (u_GroomCoatModes.y == 0)
	{
		return 1.0;
	}

	// .z, not .x: the offset is gated on this groom being a CASTER, not on it
	// having a density volume. The map's occlusion of the coat by its own
	// strands exists the moment the coat is in the map, and leaving it in
	// turns the coat black (44.98 -> 0.22 mean luma, measured).
	float exitDistance = oloGroomCoatLightExitDistance(u_GroomCoatWorldToObject, u_GroomCoatBoundsMin.xyz,
	                                                   u_GroomCoatInvExtent.xyz, v_WorldPos, L,
	                                                   u_GroomCoatModes.z);
	vec3 shadowPos = v_WorldPos + L * exitDistance;

	if (lightType == DIRECTIONAL_LIGHT)
	{
		if (u_DirectionalShadowEnabled == 0)
		{
			return 1.0;
		}
		// VSM owns the directional light when active (#702) — the CSM cascades
		// are not rendered at all in that case, so this is an either/or rather
		// than a blend. Reading it at runtime rather than through a shader
		// variant is what the lit shaders do, for the same reason.
		if (VSM_ENABLED != 0)
		{
			return vsmShadowFactor(shadowPos, L);
		}
		vec4 viewSpacePos = u_View * vec4(shadowPos, 1.0);
		return calculateCascadedShadowFactorCSM(u_ShadowMapCSM, u_ShadowMapCSMRaw, shadowPos, L, viewSpacePos.z,
		                                        u_DirectionalLightSpaceMatrices, u_CascadePlaneDistances,
		                                        u_ShadowParams, u_ShadowMapResolution, u_SoftShadowMode);
	}

	if (lightType == SPOT_LIGHT)
	{
		int atlasEntry = int(light.direction.w);
		float localShadow;
		if (vsmLocalShadow(shadowPos, L, atlasEntry, false, localShadow))
		{
			return localShadow;
		}
		if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
		{
			return calculateAtlasEntryShadow(shadowPos, u_AtlasEntryMatrices[atlasEntry],
			                                 u_AtlasEntryScaleOffset[atlasEntry], u_ShadowAtlas, u_ShadowAtlasRaw,
			                                 u_AtlasDepthBias, u_AtlasResolution, u_SoftShadowMode,
			                                 u_ShadowParams.z);
		}
		return 1.0;
	}

	if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
	{
		// direction.w carries the BASE atlas entry of the 6 face tiles. A sphere
		// area light shadows from its centre, the same representative point the
		// lighting above treats it as.
		int baseEntry = int(light.direction.w);
		float localShadow;
		if (vsmLocalShadow(shadowPos, L, baseEntry, true, localShadow))
		{
			return localShadow;
		}
		if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
		{
			int entry = baseEntry + atlasCubeFace(shadowPos - light.position.xyz);
			return calculateAtlasEntryShadow(shadowPos, u_AtlasEntryMatrices[entry],
			                                 u_AtlasEntryScaleOffset[entry], u_ShadowAtlas, u_ShadowAtlasRaw,
			                                 u_AtlasDepthBias, u_AtlasResolution,
			                                 0, // PCF only on cube faces, matching the surface path
			                                 u_ShadowParams.z);
		}
		return 1.0;
	}

	return 1.0;
}

vec3 oloGroomShadeFibre()
{
	OloGroomFibre fibre = oloGroomFibreFromUniforms();

	vec3 T = normalize(v_WorldTangent);
	vec3 V = normalize(v_WorldView);

	OloGroomFibreLobes total;
	total.R = vec3(0.0);
	total.TT = vec3(0.0);
	total.TRT = vec3(0.0);
	total.Residual = vec3(0.0);

	int lightCount = min(u_LightCount, MAX_LIGHTS);
	for (int i = 0; i < MAX_LIGHTS; ++i)
	{
		if (i >= lightCount)
		{
			break;
		}

		LightData light = u_Lights[i];
		int lightType = int(light.position.w);

		vec3 L;
		float attenuation = 1.0;
		if (lightType == DIRECTIONAL_LIGHT)
		{
			// The stored direction points the way the light TRAVELS, so the
			// direction towards the light is its negation — the same reading
			// every other lit shader in the engine takes.
			L = normalize(-light.direction.xyz);
		}
		else if (lightType == POINT_LIGHT || lightType == SPOT_LIGHT || lightType == SPHERE_AREA_LIGHT)
		{
			// SPHERE_AREA_LIGHT IS TREATED AS PUNCTUAL AT ITS CENTRE, and that
			// is a stated approximation rather than an oversight. Scene.cpp does
			// pack area lights into this buffer with the type tag 3 and their
			// radius in SpotParams.z, and PBRCommon's oloLightSample REFUSES
			// that type outright — it has no single L, because the surface
			// evaluator picks a representative point on the sphere that depends
			// on the shading normal. A fibre has no shading normal to pick one
			// with, so there is no representative point to compute: the honest
			// options are the centre or nothing.
			//
			// The centre is the radius -> 0 limit, so the coat stays lit and its
			// highlight is narrower than the body's beside it, by roughly the
			// angle the emitter subtends. Skipping instead would leave the coat
			// black under a light the body clearly responds to, which is the
			// worse of the two wrong answers. A real area-light fibre lobe is a
			// cone-times-sphere integral and belongs with #1248's transport
			// work, not here.
			vec3 toLight = light.position.xyz - v_WorldPos;
			float distance = length(toLight);
			if (distance < 1e-6)
			{
				continue;
			}
			L = toLight / distance;
			attenuation = calculateAttenuation(light.position.xyz, v_WorldPos, light.attenuationParams);
			if (lightType == SPOT_LIGHT)
			{
				attenuation *= calculateSpotIntensity(L, light.direction.xyz, light.spotParams);
			}
		}
		else
		{
			// An unknown type tag. Skipping is the loud answer: shading it as
			// something it is not would be a coat lit by a light nobody can
			// find in the scene.
			continue;
		}

		if (attenuation <= 0.0)
		{
			continue;
		}

		// How much of this coat is between the fragment and this light. Zero
		// crossings (or an inactive mode) gives transmittance 1, so a coat with
		// no volume built renders exactly as it did before this existed.
		float coatTau = oloGroomCoatOpticalDepth(u_GroomCoatVolume, u_GroomCoatWorldToObject,
		                                         u_GroomCoatBoundsMin.xyz, u_GroomCoatInvExtent.xyz,
		                                         v_WorldPos, L, u_GroomCoatInvExtent.w, u_GroomCoatModes.x);
		float coatShadow = oloGroomCoatTransmittance(coatTau, u_GroomCoatBoundsMin.w);

		// The SCENE's occlusion (#1323), multiplying the same incoming radiance
		// the coat's own term does. The two are disjoint by construction: the
		// volume holds this groom's strands and nothing else, and the map is
		// sampled at the coat's light-exit point so this groom's own strands are
		// behind the sample rather than in front of it. That is the three-way
		// split groom-coat-self-shadowing.md rule 1 states, with its third term
		// finally present.
		float sceneShadow = oloGroomSceneShadow(light, lightType, L);

		vec3 radiance = light.color.rgb * light.color.w * attenuation * coatShadow * sceneShadow;

		// THE FIBRE'S PROJECTED WIDTH, not a surface N.L. A strand lit along
		// its own length intercepts almost no light per unit length, and this
		// is the factor that says so. It is outside the BCSDF because the
		// BCSDF is normalised over the sphere without it — see
		// GroomFibreScattering.h.
		float cosWeight = oloGroomFibreCosineWeight(T, L);
		if (cosWeight <= 0.0)
		{
			continue;
		}

		oloGroomAccumulate(total, oloGroomFibreEvaluateDirections(fibre, T, V, L), radiance * cosWeight);
	}

	// THE ENVIRONMENT, through the SAME material parameters and the same
	// attenuations the loop above used — criterion 4's consistency as an
	// identity rather than a promise. The irradiance map is a cosine-lobe blur
	// of the environment, so dividing by pi recovers an average radiance, which
	// is what the uniform-environment approximation wants.
	//
	// Sampled along the fibre's EYE-FACING NORMAL, the direction in the plane
	// perpendicular to the strand that points at the viewer. With no
	// environment bound the slot answers black and this term vanishes, which is
	// the correct unlit-environment answer rather than a silent constant.
	float sinThetaO = clamp(dot(T, V), -1.0, 1.0);
	vec3 perpV = V - (T * sinThetaO);
	if (length(perpV) > 1e-6)
	{
		vec3 envDir = normalize(perpV);
		vec3 averageRadiance = texture(u_IrradianceMap, envDir).rgb * (1.0 / OLO_GROOM_FIBRE_PI);

		// The environment is occluded by the coat too, and along the SAME
		// direction it is sampled from — so this is the one extra march that is
		// consistent with the term it attenuates rather than an invented
		// ambient-occlusion factor. A strand buried in the coat sees the sky
		// through the coat; one on the surface sees it directly.
		float envTau = oloGroomCoatOpticalDepth(u_GroomCoatVolume, u_GroomCoatWorldToObject,
		                                        u_GroomCoatBoundsMin.xyz, u_GroomCoatInvExtent.xyz,
		                                        v_WorldPos, envDir, u_GroomCoatInvExtent.w, u_GroomCoatModes.x);
		averageRadiance *= oloGroomCoatTransmittance(envTau, u_GroomCoatBoundsMin.w);

		oloGroomAccumulate(total, oloGroomFibreAmbientResponse(fibre, sinThetaO), averageRadiance);
	}

	// Intensity multiplies AFTER the lobes are separated, so a debug capture of
	// one lobe is exposed the same way the full frame is.
	return oloGroomFibreSelect(total, u_GroomFibreModes.z) * fibre.Intensity;
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
	//
	// AND IT IS BYPASSED ONCE COAT SHADOWING IS ACTIVE (#1248). The ramp exists
	// because "a strand is darker near the root because it is deeper in the
	// coat" — which is precisely what the density volume now measures, per
	// fragment and per light, instead of assuming. Keeping both would darken
	// the roots twice, and the issue's scope note forbids exactly that kind of
	// double count. A coat with no volume keeps the ramp, so every capture
	// #1246 and #1247 committed still means what it meant.
	float ramp = mix(u_GroomRampWidth.x, 1.0, clamp(v_Coords.x, 0.0, 1.0));
	// BOTH conditions, not just the coat mode. The coat term is applied inside
	// oloGroomShadeFibre(), which only runs on a LIT groom — so bypassing the
	// ramp on an unlit one would remove the only depth cue it has and put
	// nothing in its place, leaving a flat coat that is darker nowhere.
	if (u_GroomFibreModes.x != 0 && u_GroomCoatModes.x != OLO_GROOM_COAT_MODE_NONE)
	{
		ramp = 1.0;
	}

	vec3 colour;
	if (u_GroomFibreModes.x != 0)
	{
		if (u_GroomFibreModes.z == OLO_GROOM_FIBRE_DEBUG_TANGENT)
		{
			// The tangent frame, remapped to [0, 1]. The one input every lobe
			// shares, so a coat that shades wrong everywhere is checked here
			// first — and it is a diagnostic, so it deliberately ignores the
			// ramp and the intensity.
			colour = (normalize(v_WorldTangent) * 0.5) + 0.5;
		}
		else
		{
			// The ramp still applies: it is a property of the CURVE (a strand
			// is darker near the root because it is deeper in the coat), not of
			// the lighting, and dropping it when the material arrives would
			// make the two paths differ by more than the lighting.
			colour = oloGroomShadeFibre() * ramp;
		}
	}
	else
	{
		colour = u_GroomColor.rgb * ramp;
	}

	// THE COAT TINT (#1251), applied LAST and to the shaded result.
	//
	// It multiplies the exit radiance rather than modulating the fibre's
	// sigma_a, and that is an APPROXIMATION, stated here rather than left to be
	// discovered: a real pigment variation would change the absorption inside
	// each fibre and therefore the hue of the TT lobe differently from the R
	// lobe. Doing it properly means a per-strand sigma_a, which means editing
	// include/GroomFibreCommon.glsl — #1247's model, which #1255 is validating
	// against independent references at the time of writing. The approximation
	// is what a per-strand albedo variation buys at zero risk to that work; the
	// exact version belongs with whoever next opens the BCSDF.
	//
	// The TANGENT diagnostic is deliberately left untinted: it is a picture of a
	// geometric input, and colouring it by the coat would make a tint look like
	// a broken tangent frame.
	if (u_GroomFibreModes.z != OLO_GROOM_FIBRE_DEBUG_TANGENT || u_GroomFibreModes.x == 0)
	{
		colour *= v_CoatTint;
	}

	o_Color = vec4(colour, 1.0);
	o_EntityID = u_GroomIDs.x;
	o_ViewNormal = octEncode(normalize(v_ViewNormal));

	vec2 ndcCurr = v_ClipCurr.xy / max(v_ClipCurr.w, 1e-6);
	vec2 ndcPrev = v_ClipPrev.xy / max(v_ClipPrev.w, 1e-6);
	o_Velocity = (ndcCurr - ndcPrev) * 0.5;

	// "No skin diffusion here." Attachment 4 is undefined unless written, and
	// an unwritten one is blurred into scene colour by SkinDiffusion.glsl.
	o_SkinDiffuse = vec4(0.0);
}
