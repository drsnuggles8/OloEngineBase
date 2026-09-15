#ifndef VIRTUAL_SKINNING_GLSL
#define VIRTUAL_SKINNING_GLSL

// =============================================================================
// VirtualSkinning.glsl — the ONE GLSL spelling of skinned virtual geometry
// (issue #1150): where a skin binding lives, how a bone palette is addressed,
// and the conservative deformed bound the cull uses.
//
// C++ twins, and a change to either half is a change to both:
//   * VirtualSkinningPacking.h — the arena layout and the 16-byte binding
//   * VirtualSkinningBounds.h  — the bound and the derivation behind it
// Pinned against each other by VirtualSkinnedBoundsTest.
//
// DECLARES NO BINDINGS, like VirtualGeometryGpuStructs.glsl: which buffers a
// stage binds is the stage's business, and an include that declared blocks
// would change every includer's reflected binding set. Every function here
// takes what it reads as a PARAMETER for that reason — a stage passes the
// arena elements and palette entries it has already fetched.
// =============================================================================

// Distinct bones one cluster may reference — MUST mirror kMaxClusterBones in
// VirtualMesh.h. A cluster whose list is all-sentinel did not fit and has no
// tight bound; the caller falls back to the instance-wide padding.
#define OLO_MAX_CLUSTER_BONES 8

// Empty bone slot in a cluster's list, and the packed-binding id that means
// "this influence slot is unused". The per-vertex form is 16-bit, so the two
// spellings differ and both are named rather than open-coded.
#define OLO_NO_CLUSTER_BONE 0xFFFFFFFFu
#define OLO_NO_SKIN_BONE 0xFFFFu

// ── The per-vertex skin binding ─────────────────────────────────────────────
//
// Two bindings to a 32-byte arena element, lane 0 in PositionU and lane 1 in
// NormalV. The field names belong to the vertex layout this region borrows;
// each lane is four raw 32-bit words:
//     word0 = boneId0 | boneId1 << 16      word2 = weight0 | weight1 << 16
//     word1 = boneId2 | boneId3 << 16      word3 = weight2 | weight3 << 16
// Weights are unorm16 and are RENORMALIZED below by what actually landed, so
// the quantization cannot scale a vertex toward the origin.
struct OloSkinBinding {
    uvec4 BoneIDs;
    vec4 Weights;
};

// Element of the skin tail holding this vertex. `skinningBase` is
// u_VirtualSkinningBase; callers MUST have checked it is non-zero first —
// past the arena a buffer-device-address read has no bounds (ADR 0011
// amendment (89)), which is the same rule the uv2 tail states.
uint oloVirtualSkinningElement(uint skinningBase, uint globalVertexIndex)
{
    return skinningBase + (globalVertexIndex >> 1u);
}

OloSkinBinding oloUnpackSkinBinding(vec4 elementLow, vec4 elementHigh, uint globalVertexIndex)
{
    vec4 lane = ((globalVertexIndex & 1u) == 0u) ? elementLow : elementHigh;
    uvec4 words = floatBitsToUint(lane);

    OloSkinBinding binding;
    uint id0 = words.x & 0xFFFFu;
    uint id1 = words.x >> 16u;
    uint id2 = words.y & 0xFFFFu;
    uint id3 = words.y >> 16u;
    binding.BoneIDs = uvec4(id0, id1, id2, id3);

    vec4 weights = vec4(float(words.z & 0xFFFFu), float(words.z >> 16u),
                        float(words.w & 0xFFFFu), float(words.w >> 16u)) / 65535.0;
    // An unused slot reads as weight 0 whatever its weight word says, which is
    // what keeps the sentinel id 0xFFFF from ever reaching a palette fetch.
    binding.Weights = vec4(id0 == OLO_NO_SKIN_BONE ? 0.0 : weights.x,
                           id1 == OLO_NO_SKIN_BONE ? 0.0 : weights.y,
                           id2 == OLO_NO_SKIN_BONE ? 0.0 : weights.z,
                           id3 == OLO_NO_SKIN_BONE ? 0.0 : weights.w);
    return binding;
}

// ── Bone scale ──────────────────────────────────────────────────────────────
//
// An UPPER BOUND on the operator 2-norm of a bone's linear part:
// sqrt(||A||_1 * ||A||_inf), the column-sum norm times the row-sum norm.
//
// Not "the length of the longest column", which is exact for a rotation-times-
// scale and is NOT an upper bound for a sheared matrix — and a bound that
// under-estimates only for unusual rigs is one that holds everywhere it gets
// tested. See SkinBoneScale in VirtualSkinningBounds.h, whose CPU twin this is.
float oloSkinMatrixNormBound(mat4 m, float diagonalOffset)
{
    float columnSum = 0.0;
    float rowSum = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        float column = 0.0;
        float row = 0.0;
        for (int r = 0; r < 3; ++r)
        {
            float offset = (i == r) ? diagonalOffset : 0.0;
            column += abs(m[i][r] - offset);
            row += abs(m[r][i] - offset);
        }
        columnSum = max(columnSum, column);
        rowSum = max(rowSum, row);
    }
    return sqrt(columnSum * rowSum);
}

float oloSkinBoneScale(mat4 boneMatrix)
{
    return oloSkinMatrixNormBound(boneMatrix, 0.0);
}

// ── The per-cluster bone set ────────────────────────────────────────────────
//
// kMaxClusterBones (8) u32 slots fill one 32-byte arena element exactly, so a
// cluster's set is at `clusterBoneBase + (clusterIndex - inst.ClusterBase)` with
// no base/count pair in the cluster record — which has one spare word, not two.
// Slots 0..3 are the element's low vec4, 4..7 the high one; the field names
// belong to the vertex layout this region borrows.
//
// An ALL-SENTINEL list means the cluster referenced more than eight bones and
// has no tight bound; the caller falls back to the instance-wide padding.
uint oloClusterBoneRef(vec4 elementLow, vec4 elementHigh, int slot)
{
    uvec4 words = floatBitsToUint(slot < 4 ? elementLow : elementHigh);
    int lane = slot & 3;
    return lane == 0 ? words.x : (lane == 1 ? words.y : (lane == 2 ? words.z : words.w));
}

#endif // VIRTUAL_SKINNING_GLSL
