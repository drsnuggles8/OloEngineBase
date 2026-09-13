#ifndef OLO_SKELETAL_DEFORMATION_GLSL
#define OLO_SKELETAL_DEFORMATION_GLSL

// =============================================================================
// SkeletalDeformation.glsl — the single skeletal deformation producer (#1226).
//
// Every consumer of a skinned surface — colour (forward and G-Buffer), the
// depth prepass, the cascaded shadow pass, the two VSM depth passes and the
// velocity/motion-vector output that rides the colour passes — obtains its
// deformed vertex from OloDeformSkinnedVertex() here, and from nowhere else.
//
// Why this file exists: before #1226 the same linear-blend skinning was written
// out seven times, once per shader, and the copies had drifted apart. The
// shadow group (ShadowDepthSkinned, VSM_DepthSkinned, VSM_DepthLocalSkinned)
// indexed the bone palette with no bounds check and had no zero-weight guard,
// so an unweighted vertex accumulated a zero matrix and collapsed onto the
// model origin — visible as a shadow that detaches from its caster — while the
// exact same vertex rendered at its rest position in colour and depth. One
// producer makes that class of drift unrepresentable.
//
// Object space in, object space out. The caller applies its own model matrix
// and view-projection, because those legitimately differ per pass (the VSM
// passes carry a per-draw-instance transform and a clip-level projection).
// Keeping the deformation itself in object space is what makes the outputs
// comparable across consumers.
//
// Defines the caller may set BEFORE including this file:
//
//   OLO_DEFORM_WANT_PREV   Declare the previous-frame bone palette (binding 31)
//                          and fill OloDeformedSurface::PrevPosition from it.
//                          Only the two colour passes emit velocity, and an
//                          unbound-but-declared descriptor is a Vulkan
//                          validation error, so the prev palette stays opt-in.
//
// The bone-ID bounds test and the zero-weight identity fallback below are the
// behaviour the colour and depth passes already had; they are reproduced here
// unchanged, including the explicit component-wise weight sum, so that the
// deformed position of those passes stays bit-identical across this change and
// their `invariant gl_Position` depth-prepass contract is preserved.
// =============================================================================

#define OLO_MAX_BONES 100

// A vertex whose weights sum below this is treated as unskinned rather than as
// "collapse to the origin". Mesh importers leave rigid attachment vertices with
// all-zero weights, and a zero accumulation matrix maps every one of them onto
// the model pivot.
#define OLO_MIN_TOTAL_BONE_WEIGHT 0.001

layout(std140, binding = 4) uniform OloBoneMatrices
{
	mat4 u_BoneTransforms[OLO_MAX_BONES];
};

#ifdef OLO_DEFORM_WANT_PREV
// Previous-frame bone palette. CommandDispatch::UploadBoneMatrices always
// populates this UBO: when the draw carries no previous pose it aliases the
// current palette here, so a read returns either the real previous pose or the
// current one (zero bone motion) and never undefined memory.
layout(std140, binding = 31) uniform OloPrevBoneMatrices
{
	mat4 u_PrevBoneTransforms[OLO_MAX_BONES];
};
#endif

// The deformed surface, in object space, as every consumer sees it.
struct OloDeformedSurface
{
	// Current-pose position, HOMOGENEOUS. The skin matrix's bottom row is the
	// weighted sum of the palette's bottom rows, so w is the total influence
	// weight rather than exactly 1 for a mesh whose weights do not sum to one.
	// That has always been true of every consumer here and it is deliberately
	// preserved: each pass multiplies this vec4 straight into its own model
	// matrix exactly as it did before, which is what keeps the colour pass and
	// the depth prepass bit-identical under `invariant gl_Position`.
	vec4 Position;
	// Current-pose normal. NOT normalized — the linear-blend matrix is not
	// orthonormal, and callers differ on where they renormalize (the G-Buffer
	// path defers it to a derivative-guarded sanitize in the fragment stage).
	vec3 Normal;
	// Previous-pose position, homogeneous on the same terms as Position. Equals
	// Position unless OLO_DEFORM_WANT_PREV is defined, so a consumer that does
	// not emit velocity still reads a well-defined value.
	vec4 PrevPosition;
};

// Sum of the four influence weights. Written out component-wise rather than as
// a dot product on purpose: the colour and depth passes must keep producing
// bit-identical positions for the GL_LEQUAL depth-prepass contract, and a dot
// product is free to contract into fused multiply-adds that the explicit sum is
// not.
float OloTotalBoneWeight(vec4 boneWeights)
{
	return boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w;
}

// True when this vertex carries usable skinning influence.
bool OloVertexIsSkinned(vec4 boneWeights)
{
	return OloTotalBoneWeight(boneWeights) > OLO_MIN_TOTAL_BONE_WEIGHT;
}

// Linear-blend skinning matrix for the current pose. Out-of-range bone IDs
// contribute nothing: a palette overrun is an out-of-bounds uniform read, which
// is undefined behaviour on both backends and a device-fault risk on Vulkan
// without robust buffer access.
mat4 OloSkinMatrix(ivec4 boneIDs, vec4 boneWeights)
{
	if (!OloVertexIsSkinned(boneWeights))
		return mat4(1.0);

	mat4 skinMatrix = mat4(0.0);
	for (int i = 0; i < 4; ++i)
	{
		int boneID = boneIDs[i];
		if (boneID >= 0 && boneID < OLO_MAX_BONES)
			skinMatrix += u_BoneTransforms[boneID] * boneWeights[i];
	}
	return skinMatrix;
}

#ifdef OLO_DEFORM_WANT_PREV
// Previous-pose counterpart of OloSkinMatrix. The guards must match it exactly:
// if the two disagree about which influences count, a vertex picks up a motion
// vector purely from the disagreement and smears under TAA and motion blur.
mat4 OloSkinMatrixPrev(ivec4 boneIDs, vec4 boneWeights)
{
	if (!OloVertexIsSkinned(boneWeights))
		return mat4(1.0);

	mat4 skinMatrix = mat4(0.0);
	for (int i = 0; i < 4; ++i)
	{
		int boneID = boneIDs[i];
		if (boneID >= 0 && boneID < OLO_MAX_BONES)
			skinMatrix += u_PrevBoneTransforms[boneID] * boneWeights[i];
	}
	return skinMatrix;
}
#endif

// Position-only entry point for the depth-only consumers (the depth prepass and
// the three shadow passes). Shares OloSkinMatrix with the full producer, so a
// depth-only pass cannot drift from the colour pass it is depth-tested against.
// It exists because the Vulkan vertex-pull route of those shaders reads only
// the position and bone streams — asking them for a normal they never fetch
// would mean inventing one.
vec4 OloDeformSkinnedPosition(vec3 restPosition, ivec4 boneIDs, vec4 boneWeights)
{
	return OloSkinMatrix(boneIDs, boneWeights) * vec4(restPosition, 1.0);
}

// THE producer. Call this, use what you need, ignore the rest — the unread
// members dead-code away, including the entire previous-pose palette read.
OloDeformedSurface OloDeformSkinnedVertex(vec3 restPosition, vec3 restNormal, ivec4 boneIDs, vec4 boneWeights)
{
	mat4 skinMatrix = OloSkinMatrix(boneIDs, boneWeights);

	OloDeformedSurface surface;
	surface.Position = skinMatrix * vec4(restPosition, 1.0);
	surface.Normal = mat3(skinMatrix) * restNormal;
#ifdef OLO_DEFORM_WANT_PREV
	mat4 prevSkinMatrix = OloSkinMatrixPrev(boneIDs, boneWeights);
	surface.PrevPosition = prevSkinMatrix * vec4(restPosition, 1.0);
#else
	surface.PrevPosition = surface.Position;
#endif
	return surface;
}

#endif // OLO_SKELETAL_DEFORMATION_GLSL
