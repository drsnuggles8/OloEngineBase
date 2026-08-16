#ifndef TERRAIN_QUADTREE_COMMON_GLSL
#define TERRAIN_QUADTREE_COMMON_GLSL

// =============================================================================
// Shared encoding for the GPU terrain LOD quadtree (issue #714).
//
// Every consumer of the GPU-built node list includes this file so the packing,
// the level-major node index and the seam-delta layout cannot drift between the
// four compute kernels and the three terrain vertex stages. The C++ twin of
// these rules lives in OloEngine/src/OloEngine/Terrain/TerrainGPUQuadtree.h
// (TerrainGpuNodeCoord) and is pinned by TerrainGPUQuadtreeTest.
//
// No samplers, no images, no OLO_BINDLESS token: including this file does not
// move an includer onto the raw-GLSL bindless route.
// =============================================================================

// ---- Node coordinate packing -------------------------------------------------
// A quadtree node is (level, x, y) with x,y in [0, 2^level). Packed into one
// uint: level in the top 4 bits, y in bits 14..27, x in bits 0..13. That caps
// the tree at level 13 (8192x8192 leaves), far past anything the CPU builder
// will produce — TerrainGPUQuadtree::kMaxDepth enforces the real limit.
#define OLO_TERRAIN_COORD_MASK 0x3FFFu

uint oloTerrainPackNode(uint level, uint nx, uint ny)
{
    return (level << 28u) | ((ny & OLO_TERRAIN_COORD_MASK) << 14u) | (nx & OLO_TERRAIN_COORD_MASK);
}

uint oloTerrainNodeLevel(uint packedNode)
{
    return packedNode >> 28u;
}

uint oloTerrainNodeX(uint packedNode)
{
    return packedNode & OLO_TERRAIN_COORD_MASK;
}

uint oloTerrainNodeY(uint packedNode)
{
    return (packedNode >> 14u) & OLO_TERRAIN_COORD_MASK;
}

// ---- Level-major node index --------------------------------------------------
// Nodes are stored level by level: level L occupies 4^L consecutive slots
// starting at (4^L - 1) / 3, addressed row-major as y * 2^L + x. Both the
// min/max-height pyramid and the split map use this index.
uint oloTerrainLevelOffset(uint level)
{
    // (4^level - 1) / 3 == ((1 << (2 * level)) - 1) / 3
    return ((1u << (2u * level)) - 1u) / 3u;
}

uint oloTerrainNodeIndex(uint level, uint nx, uint ny)
{
    return oloTerrainLevelOffset(level) + (ny << level) + nx;
}

// ---- Seam deltas -------------------------------------------------------------
// One uint per visible node holds four 8-bit "how many times coarser is my
// neighbour" counts. A node only ever DECIMATES its own edge toward a coarser
// neighbour (delta = max(0, myLevel - neighbourLevel)); the finer side of a
// boundary is always the one that snaps, so both sides land on the same set of
// heightmap samples and the edge cannot crack.
//
// Byte order matches TerrainQuadNode::NeighborLODs on the CPU: +X, -X, +Z, -Z.
#define OLO_TERRAIN_SEAM_PX 0u
#define OLO_TERRAIN_SEAM_NX 8u
#define OLO_TERRAIN_SEAM_PZ 16u
#define OLO_TERRAIN_SEAM_NZ 24u

uint oloTerrainPackSeams(uint dPX, uint dNX, uint dPZ, uint dNZ)
{
    return (dPX & 0xFFu) | ((dNX & 0xFFu) << OLO_TERRAIN_SEAM_NX) |
           ((dPZ & 0xFFu) << OLO_TERRAIN_SEAM_PZ) | ((dNZ & 0xFFu) << OLO_TERRAIN_SEAM_NZ);
}

uint oloTerrainSeamDelta(uint packedSeams, uint shift)
{
    return (packedSeams >> shift) & 0xFFu;
}

// Snap a grid index down to the nearest multiple of 2^delta. Dropping the low
// `delta` bits collapses the in-between vertices of a decimated edge onto the
// coarse neighbour's vertices, producing zero-area triangles rather than a gap.
int oloTerrainSnapEdgeIndex(int index, uint delta)
{
    return (index >> int(delta)) << int(delta);
}

#endif // TERRAIN_QUADTREE_COMMON_GLSL
