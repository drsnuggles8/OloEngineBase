#ifndef VOXEL_QUAD_UNPACK_GLSL
#define VOXEL_QUAD_UNPACK_GLSL

// =============================================================================
// VoxelQuadUnpack.glsl — GPU half of the packed-quad contract (issue #727)
// =============================================================================
//
// EXACT MIRROR of OloEngine/src/OloEngine/Terrain/Voxel/VoxelQuad.h. The bit
// layout, the face numbering and the per-face U/V basis all live in both files
// and MUST be edited together. A mismatch does not fail to compile and does not
// fail a CPU test — it silently transposes, offsets or inside-outs the quads,
// which only shows up as a wrong-looking frame.
//
// Geometry word (32 bits, 28 used):
//   [ 0.. 4] x        voxel-local origin, [0, 31]
//   [ 5.. 9] y
//   [10..14] z
//   [15..19] width  - 1   -> extent along the face's U axis, [1, 32]
//   [20..24] height - 1   -> extent along the face's V axis, [1, 32]
//   [25..27] face direction, 0..5
//   [28..31] reserved, zero
//
// Material word (32 bits, 8 used):
//   [ 0.. 7] material index
//   [ 8..31] reserved, zero

#define OLO_VOXEL_POS_BITS    5u
#define OLO_VOXEL_EXTENT_BITS 5u
#define OLO_VOXEL_POS_MASK    31u
#define OLO_VOXEL_EXTENT_MASK 31u

#define OLO_VOXEL_FACE_POS_X 0u
#define OLO_VOXEL_FACE_NEG_X 1u
#define OLO_VOXEL_FACE_POS_Y 2u
#define OLO_VOXEL_FACE_NEG_Y 3u
#define OLO_VOXEL_FACE_POS_Z 4u
#define OLO_VOXEL_FACE_NEG_Z 5u

struct OloVoxelQuad
{
    vec3  Origin;   // corner of the quad, in chunk-local voxel units
    vec3  AxisU;    // unit axis the quad extends along by Width
    vec3  AxisV;    // unit axis the quad extends along by Height
    vec3  Normal;   // outward face normal; equals cross(AxisU, AxisV)
    float Width;
    float Height;
    uint  Material;
};

// U/V are ordered per face so cross(U, V) == Normal. That, plus the shared
// unit quad's fixed {0,1,2, 2,3,0} winding, is what makes all six directions
// front-face outward under GL_CCW. Swapping a row inverts that one face.
vec3 oloVoxelFaceNormal(uint face)
{
    if (face == OLO_VOXEL_FACE_POS_X) return vec3( 1.0,  0.0,  0.0);
    if (face == OLO_VOXEL_FACE_NEG_X) return vec3(-1.0,  0.0,  0.0);
    if (face == OLO_VOXEL_FACE_POS_Y) return vec3( 0.0,  1.0,  0.0);
    if (face == OLO_VOXEL_FACE_NEG_Y) return vec3( 0.0, -1.0,  0.0);
    if (face == OLO_VOXEL_FACE_POS_Z) return vec3( 0.0,  0.0,  1.0);
    return vec3(0.0, 0.0, -1.0);
}

vec3 oloVoxelFaceAxisU(uint face)
{
    if (face == OLO_VOXEL_FACE_POS_X) return vec3(0.0, 1.0, 0.0); // +Y
    if (face == OLO_VOXEL_FACE_NEG_X) return vec3(0.0, 0.0, 1.0); // +Z
    if (face == OLO_VOXEL_FACE_POS_Y) return vec3(0.0, 0.0, 1.0); // +Z
    if (face == OLO_VOXEL_FACE_NEG_Y) return vec3(1.0, 0.0, 0.0); // +X
    if (face == OLO_VOXEL_FACE_POS_Z) return vec3(1.0, 0.0, 0.0); // +X
    return vec3(0.0, 1.0, 0.0);                                   // +Y
}

vec3 oloVoxelFaceAxisV(uint face)
{
    if (face == OLO_VOXEL_FACE_POS_X) return vec3(0.0, 0.0, 1.0); // +Z
    if (face == OLO_VOXEL_FACE_NEG_X) return vec3(0.0, 1.0, 0.0); // +Y
    if (face == OLO_VOXEL_FACE_POS_Y) return vec3(1.0, 0.0, 0.0); // +X
    if (face == OLO_VOXEL_FACE_NEG_Y) return vec3(0.0, 0.0, 1.0); // +Z
    if (face == OLO_VOXEL_FACE_POS_Z) return vec3(0.0, 1.0, 0.0); // +Y
    return vec3(1.0, 0.0, 0.0);                                   // +X
}

// Positive-facing directions sit one voxel further along their own axis than
// the minimum corner of the voxel that owns them.
vec3 oloVoxelFaceOriginOffset(uint face)
{
    if (face == OLO_VOXEL_FACE_POS_X) return vec3(1.0, 0.0, 0.0);
    if (face == OLO_VOXEL_FACE_POS_Y) return vec3(0.0, 1.0, 0.0);
    if (face == OLO_VOXEL_FACE_POS_Z) return vec3(0.0, 0.0, 1.0);
    return vec3(0.0, 0.0, 0.0);
}

OloVoxelQuad oloUnpackVoxelQuad(uint geometry, uint materialWord)
{
    uint face = (geometry >> 25u) & 7u;

    OloVoxelQuad q;
    q.Width  = float(((geometry >> 15u) & OLO_VOXEL_EXTENT_MASK) + 1u);
    q.Height = float(((geometry >> 20u) & OLO_VOXEL_EXTENT_MASK) + 1u);
    q.Normal = oloVoxelFaceNormal(face);
    q.AxisU  = oloVoxelFaceAxisU(face);
    q.AxisV  = oloVoxelFaceAxisV(face);
    q.Material = materialWord & 255u;

    vec3 voxelMin = vec3(
        float((geometry >>  0u) & OLO_VOXEL_POS_MASK),
        float((geometry >>  5u) & OLO_VOXEL_POS_MASK),
        float((geometry >> 10u) & OLO_VOXEL_POS_MASK));
    q.Origin = voxelMin + oloVoxelFaceOriginOffset(face);

    return q;
}

// corner is the shared unit quad's (u, v) in [0, 1]^2.
vec3 oloVoxelQuadCorner(OloVoxelQuad q, vec2 corner)
{
    return q.Origin + q.AxisU * (q.Width * corner.x) + q.AxisV * (q.Height * corner.y);
}

// Fallback palette used when the terrain texture arrays are unbound (they
// sample to black). Without this every material index shades identically and a
// scene that has no terrain layer textures cannot show that the merge respects
// material boundaries at all.
vec3 oloVoxelFallbackAlbedo(uint material)
{
    if (material == 1u) return vec3(0.31, 0.22, 0.13); // dirt
    if (material == 2u) return vec3(0.24, 0.42, 0.16); // grass
    if (material == 3u) return vec3(0.72, 0.66, 0.45); // sand
    if (material == 4u) return vec3(0.85, 0.88, 0.92); // snow
    return vec3(0.35, 0.32, 0.28);                     // 0 = stone
}

#endif // VOXEL_QUAD_UNPACK_GLSL
