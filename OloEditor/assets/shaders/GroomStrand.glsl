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
#include "include/GroomQuat.glsl"
#include "include/GroomStrandDeform.glsl"

// The depth prepass (#1533 E1) draws every coat twice with this program and
// shades the second draw at depth EQUAL, so the two draws must put each vertex
// at the same depth to the bit. One program with the same inputs already does
// on every driver seen; this makes it the language's promise, not the driver's.
invariant gl_Position;

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

// The shared camera block (include/CameraCommon.glsl), identical in every
// stage of every program that includes this — GL links a program only if
// its stages agree on the block — and carrying the forward screen-space AO
// lane (issue #1452).
#include "include/CameraCommon.glsl"

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
	ivec4 u_GroomModeFrame;  // x = composition mode, y = frame index, z = stochastic seed, w = depth prepass
	// Fibre scattering (#1247). The DERIVED GroomFibreParams, mirrored lane for
	// lane from UBOStructures::GroomStrandParamsUBO — see that struct for why
	// they are derived on the CPU rather than here.
	vec4 u_GroomFibreSigmaEta; // rgb = sigma_a, w = eta
	vec4 u_GroomFibreLobe;     // x = V[0], y = azimuthal scale, z = intensity, w = IBL intensity
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
	// x = effective CoatShadowMode; y = receives the scene shadow (#1323);
	// z = the object box above is valid for the scene-shadow receiver offset
	// (set for a CASTER, volume or not); w = the volume is the coat AT REST and is
	// marched from v_CoatRestPos along the direction v_CoatRestFrame turns back
	// (#1533; only a GPU-deformed draw sets it).
	ivec4 u_GroomCoatModes;
	// GPU strand deformation (#1427). Mirrored lane for lane from
	// UBOStructures::GroomStrandParamsUBO; include/GroomStrandDeform.glsl reads
	// them. Mode 0 (the default every unbound groom draws with) means the
	// stream is final and neither lane is read.
	ivec4 u_GroomDeformModes;      // x = mode, y = simulated, z = roots, w = guide slots
	ivec4 u_GroomDeformBases;      // x = root base, y = slot base, z = displacement base, w = displacements

	// Dual scattering (#1533): what the coat's other fibres pass on. The .w
	// density factors are ZERO unless this draw has a built, bound coat
	// volume, so a coat without one shades exactly as #1247/#1248 did.
	vec4 u_GroomFibreForwardScatter; // rgb = a_f, w = d_f
	vec4 u_GroomFibreBackScatter;    // rgb = A_b, w = d_b
	vec4 u_GroomFibreBackLobe;       // x = shift, y = width (radians of theta_h), zw unused
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
// The coat tint (#1251), UNPACKED HERE, per vertex. Not flat since #1533: the
// strand runs from its root tint to its tip tint, the P0 and P1 corners carry
// the two ends of each segment, and the rasterizer interpolates the COLOUR along
// the ribbon. Only the unpacked value may interpolate — the packed lane is a bit
// pattern, and arithmetic on it would be noise.
layout(location = 9) out vec3 v_CoatTint;
// A coat BAKED AT REST (#1533): this point where it rests, in groom object
// space, and the turn its root has made since the bind, as the quaternion that
// takes a direction on the posed coat back to the rest coat. On any other draw
// the posed point and the identity, which the fragment stage never reads.
layout(location = 10) out vec3 v_CoatRestPos;
layout(location = 11) flat out vec4 v_CoatRestFrame;
// The ribbon's half-width in world metres: the radius of the tube it stands
// for, which the coat-shadow march starts outside of (see oloGroomCoatTau).
layout(location = 12) out float v_TubeRadius;

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

	// The three object-space points everything below is built from. On a
	// final stream (mode 0: every unbound groom, and the CPU-deformed
	// reference path) they are the attributes, unchanged.
	vec3 position = a_Position;
	vec3 other = a_Other;
	vec3 prevPosition = a_PrevPosition;
	vec3 coatRestPos = a_Position;
	vec4 coatRestFrame = vec4(0.0, 0.0, 0.0, 1.0);
	if (u_GroomDeformModes.x == 1)
	{
		// A BOUND coat, deformed here (#1427). The stream holds this corner's
		// endpoint and the segment's other endpoint in the root's bind frame,
		// and the lanes that are last frame's centreline on a final stream
		// hold x = root slot, y = the other endpoint's parameter, z = 1 at P1.
		// See GroomStrandVertex in GroomStrandMesh.h.
		//
		// Both endpoints are deformed, so `other` is re-derived exactly the
		// way BuildGroomStrandMesh writes it: this point plus the segment's
		// P1 - P0, the same for all four corners, so the quad is not a bowtie.
		OloGroomDeformLayout deform = oloGroomDeformLayout(u_GroomDeformModes, u_GroomDeformBases);
		uint root = uint(a_PrevPosition.x + 0.5);
		float tSelf = a_Coords.x;
		float tOther = a_PrevPosition.y;
		bool atP1 = a_PrevPosition.z > 0.5;
		vec3 self = oloGroomDeformPoint(deform, root, a_Position, tSelf, false);
		vec3 otherEnd = oloGroomDeformPoint(deform, root, a_Other, tOther, false);
		vec3 segmentDelta = atP1 ? (self - otherEnd) : (otherEnd - self);
		position = self;
		other = self + segmentDelta;
		// Last frame's centreline point comes from last frame's root transform
		// and last frame's guide displacements — never from this frame's
		// position — so the velocity is the strand's own motion.
		//
		// THE DEPTH PREPASS (u_GroomModeFrame.w, #1533 E1) needs only the
		// position: it writes depth and nothing else, so it skips last frame's
		// deformation and the rest lookup below. Neither feeds gl_Position, so
		// the two draws still meet at depth EQUAL.
		bool depthOnly = u_GroomModeFrame.w != 0;
		prevPosition = depthOnly ? position : oloGroomDeformPoint(deform, root, a_Position, tSelf, true);

		// Where this point RESTS, and the turn since the bind (#1533): a coat
		// baked at rest is looked up there, in the direction the root has
		// turned back. Local to the root, so rigid with it -- exact for all the
		// body carries, and the simulation's displacement is not in it: a
		// swinging lock is shadowed by the neighbours it was groomed among.
		if (!depthOnly && u_GroomCoatModes.w != 0)
		{
			vec3 bindOrigin;
			vec4 bindRotation;
			oloGroomDeformBindFrame(deform, root, bindOrigin, bindRotation);
			coatRestPos = bindOrigin + oloGroomQuatRotate(bindRotation, a_Position);
			coatRestFrame =
			    oloGroomQuatMul(bindRotation, oloGroomQuatConjugate(oloGroomDeformRootRotation(deform, root)));
		}
	}

	vec4 worldCurr = u_GroomModel * vec4(position, 1.0);
	vec4 worldOther = u_GroomModel * vec4(other, 1.0);
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
		v_CoatRestPos = vec3(0.0);
		v_CoatRestFrame = vec4(0.0, 0.0, 0.0, 1.0);
		v_TubeRadius = 0.0;
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

	// THE FIBRE'S FRONT SURFACE, NOT ITS AXIS (#1428). The ribbon stands for a
	// cylinder, and what the camera sees of a cylinder is its near side, one
	// radius closer than the axis. Only the DEPTH moves -- the screen position
	// and the velocity stay the axis's. A card's radius is its cluster's
	// covered half-width, centimetres: drawn at its axis, the body hid the
	// card's fringe wherever the skin curves towards the camera, and the long
	// coat's card tier lost 9% of its coverage over the animal to it. A
	// strand's radius is a fraction of a millimetre, which is still enough to
	// lift its root out of a depth tie with the skin it grows from (the strand
	// tier gained 2% there).
	//
	// The eye, recovered from the VIEW MATRIX rather than read from the camera
	// block's position lane. The view matrix is rigid, so its inverse
	// translation is exact — and, decisively, it is in whatever space u_View is
	// in. u_ViewProjection is uploaded RENDER-RELATIVE (issue #429), so this
	// eye is render-relative too, automatically and without this shader having
	// to know the origin or trust a second lane to have been shifted the same
	// way.
	vec3 eyeWorld = -(transpose(mat3(u_View)) * u_View[3].xyz);
	vec3 axisToEye = eyeWorld - worldCurr.xyz;
	float eyeDistance = length(axisToEye);
	if (eyeDistance > 2.0 * radiusWorld)
	{
		vec4 clipFront = u_ViewProjection * vec4(worldCurr.xyz + (axisToEye * (radiusWorld / eyeDistance)), 1.0);
		// Only while the front surface is still in front of the near plane: a
		// fibre whose axis is just inside it keeps the axis's depth rather
		// than being clipped away. u_ViewProjection is the rasterizer flavour,
		// so NDC depth starts at 0 on Vulkan and at -1 on OpenGL.
#ifdef OLO_VULKAN
		const float nearDepth = 0.0;
#else
		const float nearDepth = -1.0;
#endif
		float frontDepth = clipFront.w > 1e-6 ? clipFront.z / clipFront.w : nearDepth;
		if (frontDepth > nearDepth)
		{
			gl_Position.z = frontDepth * gl_Position.w;
		}
	}

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
	v_ClipPrev = u_PrevViewProjection * (u_GroomPrevModel * vec4(prevPosition, 1.0));

	// A curve has no surface normal. The ribbon's is the best available
	// answer for an SSAO consumer: perpendicular to the strand and facing the
	// eye. Written rather than left undefined, because attachment 2 is SSAO's
	// input and an unwritten MRT output is garbage, not zero.
	vec3 segmentView = mat3(u_View) * (mat3(u_GroomModel) * (other - position));
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
	vec3 segmentWorld = mat3(u_GroomModel) * (other - position);
	v_WorldTangent = length(segmentWorld) > 1e-8 ? normalize(segmentWorld) : vec3(1.0, 0.0, 0.0);

	v_WorldPos = worldCurr.xyz;
	v_WorldView = axisToEye;
	v_CoatTint = oloGroomUnpackTint(a_Tint);
	v_CoatRestPos = coatRestPos;
	v_CoatRestFrame = coatRestFrame;
	v_TubeRadius = radiusWorld;
}

#type fragment
#version 450 core

#include "include/GroomStrandCommon.glsl"
#include "include/GroomFibreCommon.glsl"
#include "include/GroomCoatShadowCommon.glsl"
#include "include/GroomQuat.glsl"

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

// ── The scene's shadows, received (#1323, re-landed by #1523) ───────────────
//
// THE OTHER DIRECTION from #1248. The density volume below gives the coat its
// own internal occlusion — the groom's strands and nothing else — and a body
// casting onto its own coat is the shadow map's job. This block is that job:
// the same four shadow inputs, the same two directional techniques and the same
// helper functions the lit surface shaders use, so a coat in shade goes dark
// for exactly the reason the body beside it does.
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
	float u_AtlasDepthBiasTexels;
	int _shadowPad2;
};

// The Virtual Shadow Map's consumer half. The engine has TWO directional
// techniques and a caster family reaches a technique only if somebody wired it
// there — which is as true of the RECEIVING side as of the casting side, and
// nothing detects either gap. Including this is the receiving half of that
// wiring; ShadowRenderPass::RenderGroomVirtualShadowLevels is the casting half.
#include "include/VirtualShadowSampling.glsl"

#ifdef OLO_BINDLESS
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10) // TEX_USER_0
#else
layout(binding = 10) uniform samplerCube u_IrradianceMap; // TEX_USER_0
#endif

// The camera block the vertex stage declares; GL links a program only if its
// stages agree on the block.
//
// No screen-space AO here (issue #1452): a strand is not in the forward
// depth-normal prepass, so the AO buffer holds the occlusion of the surface
// BEHIND it at every one of its pixels. The Deferred path's forward-lit groom
// takes none either.
#include "include/CameraCommon.glsl"

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
layout(location = 3) out vec4 o_Velocity;
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
// Interpolated root-to-tip (#1533); the vertex stage declares it the same way.
layout(location = 9) in vec3 v_CoatTint;
layout(location = 10) in vec3 v_CoatRestPos;
layout(location = 11) flat in vec4 v_CoatRestFrame;
layout(location = 12) in float v_TubeRadius;

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
	ivec4 u_GroomModeFrame;  // x = composition mode, y = frame index, z = stochastic seed, w = depth prepass
	// Fibre scattering (#1247). The DERIVED GroomFibreParams, mirrored lane for
	// lane from UBOStructures::GroomStrandParamsUBO — see that struct for why
	// they are derived on the CPU rather than here.
	vec4 u_GroomFibreSigmaEta; // rgb = sigma_a, w = eta
	vec4 u_GroomFibreLobe;     // x = V[0], y = azimuthal scale, z = intensity, w = IBL intensity
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
	// x = effective CoatShadowMode; y = receives the scene shadow (#1323);
	// z = the object box above is valid for the scene-shadow receiver offset
	// (set for a CASTER, volume or not); w = the volume is the coat AT REST and is
	// marched from v_CoatRestPos along the direction v_CoatRestFrame turns back
	// (#1533; only a GPU-deformed draw sets it).
	ivec4 u_GroomCoatModes;
	// GPU strand deformation (#1427). Mirrored lane for lane from
	// UBOStructures::GroomStrandParamsUBO; include/GroomStrandDeform.glsl reads
	// them. Mode 0 (the default every unbound groom draws with) means the
	// stream is final and neither lane is read.
	ivec4 u_GroomDeformModes;      // x = mode, y = simulated, z = roots, w = guide slots
	ivec4 u_GroomDeformBases;      // x = root base, y = slot base, z = displacement base, w = displacements

	// Dual scattering (#1533): what the coat's other fibres pass on. The .w
	// density factors are ZERO unless this draw has a built, bound coat
	// volume, so a coat without one shades exactly as #1247/#1248 did.
	vec4 u_GroomFibreForwardScatter; // rgb = a_f, w = d_f
	vec4 u_GroomFibreBackScatter;    // rgb = A_b, w = d_b
	vec4 u_GroomFibreBackLobe;       // x = shift, y = width (radians of theta_h), zw unused
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

// How much of `light` reaches this strand THROUGH THE SCENE. Issue #1323,
// re-landed by #1523.
//
// WHERE THIS FRAGMENT IS IN ITS COAT. A volume baked from the drawn pose is
// marched from the fragment's own point. One baked AT REST (#1533,
// u_GroomCoatModes.w) is marched from where the point rests, along the
// direction turned back through its root's motion: the neighbourhood it was
// groomed in, seen from the side the light now comes from, wherever the body
// has carried it. The rotation keeps the direction's length, which the exit
// distance relies on (see oloGroomCoatLightExitDistance).
//
// FROM THE TUBE'S LIT SIDE, NOT ITS AXIS (#1533; the #1428 rule, for light).
// A ribbon stands for a tube and the light enters it through the side that
// faces the light, one radius from the axis. A strand's radius is a fraction of
// a millimetre, far under a voxel, so its march is unchanged. A CARD's is its
// lock's half-width: marched from the axis, every card fragment was shadowed by
// the densest part of its own lock -- the centreline -- and the long coat's card
// tier read 17-27% darker than the strands it stands for under a converged
// volume. The strands a viewer sees of a lock are its outer ones; the offset
// makes the card answer for them.
float oloGroomCoatTau(vec3 worldDir)
{
	float tube = max(v_TubeRadius, 0.0);
	if (u_GroomCoatModes.w != 0)
	{
		vec3 dirObject = normalize(oloGroomQuatRotate(v_CoatRestFrame, mat3(u_GroomCoatWorldToObject) * worldDir));
		return oloGroomCoatOpticalDepthObject(u_GroomCoatVolume, u_GroomCoatBoundsMin.xyz, u_GroomCoatInvExtent.xyz,
		                                      v_CoatRestPos + dirObject * tube, dirObject, u_GroomCoatInvExtent.w,
		                                      u_GroomCoatModes.x);
	}
	return oloGroomCoatOpticalDepth(u_GroomCoatVolume, u_GroomCoatWorldToObject, u_GroomCoatBoundsMin.xyz,
	                                u_GroomCoatInvExtent.xyz, v_WorldPos + worldDir * tube, worldDir,
	                                u_GroomCoatInvExtent.w, u_GroomCoatModes.x);
}

float oloGroomCoatExitDistance(vec3 worldDir)
{
	if (u_GroomCoatModes.w != 0)
	{
		return oloGroomCoatLightExitDistanceObject(
		    u_GroomCoatBoundsMin.xyz, u_GroomCoatInvExtent.xyz, v_CoatRestPos,
		    oloGroomQuatRotate(v_CoatRestFrame, mat3(u_GroomCoatWorldToObject) * worldDir), u_GroomCoatModes.z);
	}
	return oloGroomCoatLightExitDistance(u_GroomCoatWorldToObject, u_GroomCoatBoundsMin.xyz, u_GroomCoatInvExtent.xyz,
	                                     v_WorldPos, worldDir, u_GroomCoatModes.z);
}

// WITHOUT AN OPAQUE COPY THE RECEIVER IS THE COAT'S LIGHT-EXIT POINT, NOT THE
// FRAGMENT (#1323), and that substitution is the double-count fix for a map
// that holds the fur (see the opaque copies below). Once grooms are shadow CASTERS
// their own strands are in the shadow map, so a strand sampling that map at its
// own position is occluded by its own coat TWICE: once by the density volume in
// oloGroomShadeFibre and once by the map. Moving the sample to where the light
// LEAVES the coat leaves the map answering only the part the volume did not —
// is the coat's lit surface itself in shadow.
//
// THE OFFSET IS GATED ON CASTING (u_GroomCoatModes.z), NOT ON THE VOLUME. The
// map's occlusion of the coat by its own strands exists the moment the coat is
// in the map, volume or not, and leaving it in turns a caster's coat black
// (44.98 -> 0.22 mean luma, measured on #1380): a shadow map is a BINARY test
// and a coat is not binary.
//
// A STRAND HAS NO SURFACE NORMAL. The engine's receiver bias is a world-metre
// offset along the shading normal, and a ribbon's v_ViewNormal is written for
// SSAO rather than for shading — it faces the camera, not the surface. So the
// bias direction passed below is L: an offset TOWARDS the light, which is the
// direction the exit offset already moves in and the only direction on a fibre
// that means anything for occlusion.
//
// THE CASCADES ARE THE FIRST DIRECTIONAL LIGHT'S. Scene.cpp builds them for UBO
// index 0 only, so a second directional light reads 1 here rather than the
// first one's map — the same rule every lit surface shader applies.
//
// THE OPAQUE COPIES ARE SAMPLED AT THE STRAND WHERE THEY EXIST (#1533;
// u_GroomCoatModes.y is a bitfield: 1 receives, 2 the cascades bound are the
// opaque copy, 4 the atlas bound is). The exit point sits OUTSIDE the groom's
// box, so the map answered only for what lies beyond it: the body the coat grows
// on never shadowed its own fur, and a chin over a chest, a leg against a belly
// or a coat lit from behind took the light through the animal. ShadowRenderPass
// now copies the cascades and the atlas after their opaque casters and before
// their grooms, and GroomRenderPass binds the copies for these draws, so the
// strand samples the light WHERE IT IS against maps with no fur in them: the
// body and every other opaque caster shadow it, and the coat's own extinction
// stays the density volume's alone. What a copy cannot hold is ANOTHER groom's
// fur: a second coat does not shadow this one (groom-into-the-shadow-techniques.md
// rule 8). The VSM keeps the exit point: its cached pages still hold the fur.
//
// `known` says whether the BODY's occlusion of this light is in the answer,
// which is not the same as a map having answered: a lookup at the exit point
// answers for what lies beyond the coat and knows nothing of the body inside
// it. So it is true only where the lookup ran at the strand itself -- an opaque
// copy, or a coat that does not cast and so has no exit offset. 1.0 with
// known == false is "nothing here can say", not "lit", and dual scattering
// needs the difference (see oloGroomShadeFibre).
// A STRAND AGAINST THE OPAQUE ATLAS MOVES TOWARD THE LIGHT, BY TEXELS OF THE ENTRY
// (#1533). Against a map with none of its own fur in it -- the opaque copy, or the
// full map under a coat that does not cast -- a strand has nothing of its own to
// self-shadow on; what it must clear is the body it grows from. It has no normal of
// its own, so it hands the shared atlas lookup the light direction as the offset
// direction, and the shared helper moves it ATLAS_NORMAL_OFFSET_TEXELS (1.5) texels
// of the entry AT THE RECEIVER: the unit every surface's offset is in, so it grows
// with the light's distance, shrinks with the tile's resolution and never depends
// on the object. It is bounded by what the map can resolve: an occluder nearer than
// a texel and a half along the light falls in the receiver's own texel or the next,
// where the map cannot tell it from the body. The fixed centimetre it replaced was
// two texels of a tile a few metres away and many times that close up, so under a
// close spot it stepped past a thin occluder millimetres above the fur, which then
// took none of its shadow (GroomSceneShadowVisualEvidenceTest, ShadowMapTest).
// The depth bias, in texels of the entry like every atlas lookup's (PBRCommon.glsl),
// is a quarter of one. The exit-point fallback keeps the surfaces' bias, no offset.
const float OLO_GROOM_STRAND_ATLAS_BIAS = 0.25;

float oloGroomSceneShadow(LightData light, int lightIndex, int lightType, vec3 L, out bool known)
{
	known = false;
	if (u_GroomCoatModes.y == 0)
	{
		return 1.0;
	}

	float exitDistance = oloGroomCoatExitDistance(L);
	vec3 shadowPos = v_WorldPos + L * exitDistance;
	// A receiver with no exit offset IS the strand, so a map sampled there sees
	// the body (a coat that does not cast; NaN also lands here, unshifted).
	bool atStrand = !(exitDistance > 0.0);
	bool opaqueCascades = (u_GroomCoatModes.y & 2) != 0;
	bool opaqueAtlas = (u_GroomCoatModes.y & 4) != 0;

	if (lightType == DIRECTIONAL_LIGHT)
	{
		if (u_DirectionalShadowEnabled == 0 || lightIndex != 0)
		{
			return 1.0;
		}
		// VSM owns the directional light when active (#702) — the CSM cascades
		// are not rendered at all in that case, so this is an either/or rather
		// than a blend. Read at runtime rather than through a shader variant,
		// as the lit shaders do.
		if (VSM_ENABLED != 0)
		{
			known = atStrand;
			return vsmShadowFactor(shadowPos, L);
		}
		// The opaque cascades are sampled at the strand itself: the body's
		// shadow. The fibre has no surface, so the receiver bias runs toward
		// the light either way.
		vec3 receiver = opaqueCascades ? v_WorldPos : shadowPos;
		known = opaqueCascades || atStrand;
		vec4 viewSpacePos = u_View * vec4(receiver, 1.0);
		return calculateCascadedShadowFactorCSM(u_ShadowMapCSM, u_ShadowMapCSMRaw, receiver, L, viewSpacePos.z,
		                                        u_DirectionalLightSpaceMatrices, u_CascadePlaneDistances,
		                                        u_ShadowParams, u_ShadowMapResolution, u_SoftShadowMode);
	}

	if (lightType == SPOT_LIGHT)
	{
		int atlasEntry = int(light.direction.w);
		float localShadow;
		if (vsmLocalShadow(shadowPos, L, atlasEntry, false, localShadow))
		{
			known = atStrand;
			return localShadow;
		}
		if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
		{
			bool atlasAtStrand = opaqueAtlas || atStrand;
			vec3 receiver = atlasAtStrand ? v_WorldPos : shadowPos;
			vec3 offsetDirection = atlasAtStrand ? L : vec3(0.0);
			float atlasBias = atlasAtStrand ? OLO_GROOM_STRAND_ATLAS_BIAS : u_AtlasDepthBiasTexels;
			known = atlasAtStrand;
			return calculateAtlasEntryShadow(receiver, offsetDirection, u_AtlasEntryMatrices[atlasEntry],
			                                 u_AtlasEntryScaleOffset[atlasEntry], u_ShadowAtlas, u_ShadowAtlasRaw,
			                                 atlasBias, u_AtlasResolution, u_SoftShadowMode, u_ShadowParams.z);
		}
		return 1.0;
	}

	if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
	{
		// direction.w carries the BASE atlas entry of the 6 face tiles. A sphere
		// area light shadows from its centre, the same representative point the
		// lighting treats it as.
		int baseEntry = int(light.direction.w);
		float localShadow;
		if (vsmLocalShadow(shadowPos, L, baseEntry, true, localShadow))
		{
			known = atStrand;
			return localShadow;
		}
		if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
		{
			bool atlasAtStrand = opaqueAtlas || atStrand;
			vec3 receiver = atlasAtStrand ? v_WorldPos : shadowPos;
			vec3 offsetDirection = atlasAtStrand ? L : vec3(0.0);
			float atlasBias = atlasAtStrand ? OLO_GROOM_STRAND_ATLAS_BIAS : u_AtlasDepthBiasTexels;
			known = atlasAtStrand;
			int entry = baseEntry + atlasCubeFace(receiver - light.position.xyz);
			return calculateAtlasEntryShadow(receiver, offsetDirection, u_AtlasEntryMatrices[entry],
			                                 u_AtlasEntryScaleOffset[entry], u_ShadowAtlas, u_ShadowAtlasRaw,
			                                 atlasBias, u_AtlasResolution,
			                                 0, // PCF only on cube faces, matching the surface path
			                                 u_ShadowParams.z);
		}
		return 1.0;
	}

	return 1.0;
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
// attenuations absorb light INSIDE the fibre being shaded, so tau itself stays
// geometric and colourless: fibre length density times diameter times the sine
// of the angle to the fibre, and nothing else. What dual scattering (below)
// adds is the pigment of the OTHER fibres the light crossed on its way here —
// once per crossing, never the shaded fibre's again.
//
// OCCLUSION BY THE REST OF THE SCENE is the shadow map's job, not this
// volume's, and it multiplies the same incoming radiance through
// oloGroomSceneShadow above (#1323) — sampled at the strand against the opaque
// copies (#1533), or at the coat's light-exit point where there is no copy, so
// the two terms never count the coat's own strands twice. The volume
// deliberately contains the groom's own strands and nothing else. With the coat
// term active the geometric root-to-tip ramp is bypassed — see main() —
// because that ramp was the crude stand-in for exactly this.
//
// DUAL SCATTERING (#1533) IS THE LIGHT THE COAT'S OTHER FIBRES PASS ON, and it
// runs only on a coat with a density volume, because the volume is what counts
// them: u_GroomFibreBackScatter.w is zero otherwise and every term below
// reduces to the #1247/#1248 picture. Zinke et al. 2008, with the constants
// GroomFibreComputeDualScattering derived from THIS fibre's attenuations:
//
//   * The volume's crossings no longer destroy what they intercept. Of the
//     light that reaches this strand through the coat, the part no fibre
//     intercepted (oloGroomCoatTransmittance) is shaded as before, and the part
//     other fibres FORWARDED (oloGroomCoatForwardTransmittance minus that) is
//     shaded as scattered light, weighted by the density factor d_f.
//   * The fibres around this one scatter light back to it: the local lobe A_b,
//     for the direct part and — pi times, as Zinke integrates it over the
//     azimuths the scattered light arrives from — for the forwarded part.
//
// A pale coat's colour lives in these terms: its fibres absorb little each, and
// the gold compounds with every crossing. Without them such a coat renders grey
// in its depths, and bright only where the environment's uniform transmission
// term — a lone fibre's answer — lit it from a direction the body blocks.
//
// THE FORWARDED PART IS NOT SCATTERED IN ANGLE. Zinke widens the single-scatter
// lobes by the spread the forwarded light picked up; this shades it with the
// unwidened ones, which keeps highlights a little crisper in the coat's depths
// than they should be. Stated rather than hidden, like the far-field choice.
struct OloGroomShading
{
	OloGroomFibreLobes Single; // this fibre's own four paths
	vec3 Multiple;             // what the coat's other fibres scattered back to it
};

OloGroomShading oloGroomShadeFibre()
{
	OloGroomFibre fibre = oloGroomFibreFromUniforms();

	vec3 T = normalize(v_WorldTangent);
	vec3 V = normalize(v_WorldView);

	OloGroomFibreLobes total;
	total.R = vec3(0.0);
	total.TT = vec3(0.0);
	total.TRT = vec3(0.0);
	total.Residual = vec3(0.0);
	vec3 multiple = vec3(0.0);

	bool dualScattering = u_GroomFibreBackScatter.w > 0.0;
	vec3 forwardScatter = u_GroomFibreForwardScatter.rgb;
	float densityForward = u_GroomFibreForwardScatter.w;
	vec3 multipleBackScatter = u_GroomFibreBackScatter.rgb;
	float densityBack = u_GroomFibreBackScatter.w;
	float sinThetaView = clamp(dot(T, V), -1.0, 1.0);
	vec3 perpView = V - (T * sinThetaView);
	float perpViewLength = length(perpView);

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

		// The scene's occlusion of this light (#1323): 1 unless this coat
		// receives scene shadows. FIRST, because a strand the scene shadows
		// completely -- the far side of the body, since #1533 -- receives none
		// of this light: the direct, forwarded and back-scattered terms below
		// all scale with the radiance, so its coat march is skipped. `<=`, not
		// `!(> 0)`, so a NaN still reaches the colour and shows.
		bool occlusionKnown = false;
		float sceneShadow = oloGroomSceneShadow(light, i, lightType, L, occlusionKnown);
		if (sceneShadow <= 0.0)
		{
			continue;
		}
		// How much of this coat is between the fragment and this light. Zero
		// crossings (or an inactive mode) gives transmittance 1, so a coat with
		// no volume built renders exactly as it did before this existed.
		float coatTau = oloGroomCoatTau(L);
		float coatShadow = oloGroomCoatTransmittance(coatTau, u_GroomCoatBoundsMin.w);
		// What the other fibres forwarded on top of it (dual scattering) —
		// ONLY where a shadow map accounts for the body, which is a map sampled
		// AT THE STRAND (see the scene-shadow receive above): one sampled at the
		// coat's exit point answers for what lies beyond the coat and not for
		// the body inside it. The volume holds strands, not the body, so a light nothing
		// shadows would forward straight through the animal: a rim light behind
		// the head lit the front of the face through it, as a frost of backlit
		// strands. Unscattered, such a light is still stopped by the coat's two
		// root layers, which is the #1248 behaviour; that part stays.
		vec3 forwarded = vec3(0.0);
		if (dualScattering && occlusionKnown)
		{
			forwarded = max(oloGroomCoatForwardTransmittance(coatTau, u_GroomCoatBoundsMin.w, forwardScatter) -
			                    vec3(coatShadow),
			                vec3(0.0));
		}

		vec3 radiance = light.color.rgb * light.color.w * attenuation * sceneShadow;

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

		vec3 arriving = radiance * (vec3(coatShadow) + (forwarded * densityForward));
		oloGroomAccumulate(total, oloGroomFibreEvaluateDirections(fibre, T, V, L), arriving * cosWeight);

		if (dualScattering)
		{
			float sinThetaLight = clamp(dot(T, L), -1.0, 1.0);
			vec3 perpLight = L - (T * sinThetaLight);
			float perpLightLength = length(perpLight);
			float cosPhi = 0.0;
			if (perpViewLength > 1.0e-6 && perpLightLength > 1.0e-6)
			{
				cosPhi = dot(perpView, perpLight) / (perpViewLength * perpLightLength);
			}
			// Projected: the lobe already carries cos(theta_i).
			vec3 back = oloGroomFibreBackScatterProjected(multipleBackScatter, u_GroomFibreBackLobe.x,
			                                              u_GroomFibreBackLobe.y, sinThetaView, sinThetaLight,
			                                              cosPhi);
			multiple += back * radiance *
			            ((vec3(coatShadow) * densityBack) + (forwarded * (densityForward * OLO_GROOM_FIBRE_PI * densityBack)));
		}
	}

	// THE ENVIRONMENT, through the SAME material parameters and the same
	// attenuations the loop above used — criterion 4's consistency as an
	// identity rather than a promise. The irradiance cube stores E/pi, which is
	// already the average radiance the uniform-environment approximation wants;
	// see oloGroomFibreEnvironmentRadiance for why there is no 1/pi (#1450).
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
		vec3 averageRadiance = oloGroomFibreEnvironmentRadiance(u_IrradianceMap, envDir, u_GroomFibreLobe.w);

		// The environment is occluded by the coat too, and along the SAME
		// direction it is sampled from — so this is the one extra march that is
		// consistent with the term it attenuates rather than an invented
		// ambient-occlusion factor. A strand buried in the coat sees the sky
		// through the coat; one on the surface sees it directly.
		float envTau = oloGroomCoatTau(envDir);
		float envShadow = oloGroomCoatTransmittance(envTau, u_GroomCoatBoundsMin.w);
		OloGroomFibreLobes ambient = oloGroomFibreAmbientResponse(fibre, sinThetaO);
		if (!dualScattering)
		{
			oloGroomAccumulate(total, ambient, averageRadiance * envShadow);
		}
		else
		{
			// IN A COAT THE SKY IS NOT ALL AROUND THE FIBRE. The uniform
			// approximation hands every path its albedo, TT included — and TT is
			// light from BEHIND the fibre, which on a coat is more coat and then
			// the body. So the paths split by where they look: R and TRT, and
			// half the isotropic residual, see the sky on the viewer's side
			// through the coat above; TT and the other half see it behind,
			// through the coat below.
			//
			// BEHIND, ONLY THE UNSCATTERED PART. The volume holds strands, not
			// the body, so a ray through the coat's roots and on through the
			// body would find a second coat and read the sky beyond it. Counting
			// what pale fibres forward along that ray would light the front of
			// the dog with the sky behind it; the colourless transmittance does
			// not, because the two root layers stop it.
			vec3 envForwarded = max(oloGroomCoatForwardTransmittance(envTau, u_GroomCoatBoundsMin.w, forwardScatter) -
			                            vec3(envShadow),
			                        vec3(0.0));
			vec3 front = averageRadiance * (vec3(envShadow) + (envForwarded * densityForward));

			vec3 behindDir = -envDir;
			float behindTau = oloGroomCoatTau(behindDir);
			vec3 behind = oloGroomFibreEnvironmentRadiance(u_IrradianceMap, behindDir, u_GroomFibreLobe.w) *
			              oloGroomCoatTransmittance(behindTau, u_GroomCoatBoundsMin.w);

			total.R += ambient.R * front;
			total.TRT += ambient.TRT * front;
			total.TT += ambient.TT * behind;
			total.Residual += ambient.Residual * (0.5 * (front + behind));
			// The sky's back-scatter: the lobe integrates to A_b over a uniform
			// hemisphere, times cos(theta_o) as every ambient path here is.
			float cosThetaO = sqrt(max(0.0, 1.0 - (sinThetaO * sinThetaO)));
			multiple += multipleBackScatter * (densityBack * cosThetaO) * front;
		}
	}

	// Intensity multiplies AFTER the lobes are separated, so a debug capture of
	// one lobe is exposed the same way the full frame is.
	OloGroomShading shading;
	shading.Single.R = total.R * fibre.Intensity;
	shading.Single.TT = total.TT * fibre.Intensity;
	shading.Single.TRT = total.TRT * fibre.Intensity;
	shading.Single.Residual = total.Residual * fibre.Intensity;
	shading.Multiple = multiple * fibre.Intensity;
	return shading;
}

// The coat tint (#1251) and the debug selection, together, because the tint
// is PIGMENT: it colours every path that entered a fibre and not R, which is
// the cuticle's surface reflection and carries the light's colour — the white
// sheen on a golden coat. Tinting R as well (#1251's first form) coloured the
// sheen with the coat and left the coat looking matte.
vec3 oloGroomComposite(OloGroomShading shading, int debugMode, vec3 tint)
{
	OloGroomFibreLobes tinted = shading.Single;
	tinted.TT *= tint;
	tinted.TRT *= tint;
	tinted.Residual *= tint;
	vec3 multiple = shading.Multiple * tint;
	if (debugMode == OLO_GROOM_FIBRE_DEBUG_MULTIPLE)
	{
		return multiple;
	}
	vec3 single = oloGroomFibreSelect(tinted, debugMode);
	return (debugMode == OLO_GROOM_FIBRE_DEBUG_FULL) ? (single + multiple) : single;
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

	// THE DEPTH PREPASS (#1533 E1). GroomRenderPass draws every coat twice with
	// this program: first with colour writes off and .w set, which decides
	// coverage above and writes depth, then with depth EQUAL and depth writes
	// off, which shades only the fragment that won. The coverage decision is a
	// pure function of the fragment (its pixel, frame, segment and alpha), so
	// the second draw keeps exactly the fragments the first did, and the same
	// program computes the same depth in both. A dense coat overlaps itself
	// tens of times per pixel; without this, every layer was shaded in full.
	if (u_GroomModeFrame.w != 0)
	{
		return;
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
			colour = oloGroomComposite(oloGroomShadeFibre(), u_GroomFibreModes.z, v_CoatTint) * ramp;
		}
	}
	else
	{
		colour = u_GroomColor.rgb * ramp;
	}

	// THE COAT TINT (#1251), applied LAST and to the shaded result — the root
	// tint at the follicle, the tip tint at the end, interpolated between (#1533).
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
	//
	// A LIT groom is tinted inside oloGroomComposite, path by path; this is the
	// UNLIT one, whose neutral ramp has no paths to tell apart.
	if (u_GroomFibreModes.x == 0)
	{
		colour *= v_CoatTint;
	}

	o_Color = vec4(colour, 1.0);
	o_EntityID = u_GroomIDs.x;
	o_ViewNormal = octEncode(normalize(v_ViewNormal));

	vec2 ndcCurr = v_ClipCurr.xy / max(v_ClipCurr.w, 1e-6);
	vec2 ndcPrev = v_ClipPrev.xy / max(v_ClipPrev.w, 1e-6);
	// .b is the strand's WIDENED alpha — the fraction of this pixel the
	// strand covers (#1256). A sub-pixel strand is widened to one pixel and
	// pays for it in alpha, so this is coverage by construction; see
	// Groom/GroomCoverage.h. Under StochasticAlpha it is also the value that
	// moves every frame, which the reactive term's dead band must ignore.
	o_Velocity = vec4((ndcCurr - ndcPrev) * 0.5, alpha, 0.0);

	// "No skin diffusion here." Attachment 4 is undefined unless written, and
	// an unwritten one is blurred into scene colour by SkinDiffusion.glsl.
	o_SkinDiffuse = vec4(0.0);
}
