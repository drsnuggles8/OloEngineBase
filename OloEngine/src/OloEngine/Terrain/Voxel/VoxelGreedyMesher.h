#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/Voxel/VoxelQuad.h"

#include <array>
#include <vector>

namespace OloEngine
{
    // =========================================================================
    // Binary greedy meshing for blocky (cubic) voxel volumes — issue #727
    // =========================================================================
    //
    // Runs alongside MarchingCubes, which stays the right mesher for organic
    // isosurfaces. This one treats the SDF as pure solid/empty and merges
    // co-planar visible faces into maximal quads with bitwise operations.
    //
    // Ported from D:\repos\VoxelEngine (MIT) with two deliberate divergences,
    // both recorded here because each fixes a limitation of the original:
    //
    //  1. Extents are stored biased-by-one (see VoxelQuad.h), so a full
    //     32-voxel span is representable. The reference cannot express it.
    //  2. Padding is uniform on all three axes (CS_P^3 conceptually) instead of
    //     padding X/Z in the array dimensions and special-casing +/-Y against
    //     the neighbour chunk's edge bit. The reference needs u32 columns to
    //     match its 32-voxel chunk, which leaves no room for the two Y padding
    //     bits; we use u64 columns for a 34-bit padded span, so all six
    //     directions run the identical code path. That symmetry is the whole
    //     reason the chunk-boundary cases are not a separate bug surface.

    // -------------------------------------------------------------------------
    // The padded neighbourhood snapshot
    // -------------------------------------------------------------------------
    //
    // Gathered on the thread that owns the VoxelOverride, then handed to a
    // worker. This is the thread-safety seam: everything downstream of it is a
    // pure function of this immutable blob, so a concurrent CarveSphere cannot
    // race the mesher.
    struct VoxelNeighbourhood
    {
        static constexpr u32 CS = VoxelChunk::CHUNK_SIZE; // 32
        static constexpr u32 CS_P = CS + 2;               // 34, one padding voxel per side
        static constexpr sizet COLUMN_COUNT = static_cast<sizet>(CS_P) * CS_P;

        static_assert(CS_P <= 64, "The padded span must fit in one u64 column");

        // Solidity as bit columns along +Y: index [pz * CS_P + px], bit py.
        // Padded coordinate p maps to chunk-local coordinate p - 1, so the
        // centre chunk occupies [1, CS] on every axis and the border planes
        // hold the neighbours' facing slabs.
        //
        // Edge and corner padded cells are never read: a face of a centre voxel
        // only ever queries the cell one step along ONE axis, which is always a
        // face-neighbour cell. Those cells stay zero and that is correct.
        std::array<u64, COLUMN_COUNT> SolidY{};

        // Per-voxel material index of the CENTRE chunk only (CS^3, row-major
        // like VoxelChunk). A face always belongs to a centre voxel, so no
        // padding is needed here. Empty => material 0 everywhere, which enables
        // the fully bitwise merge path.
        std::vector<u8> Materials;

        VoxelCoord Coord{};

        // True when the centre chunk has no solid voxel at all — nothing to mesh.
        [[nodiscard]] bool IsCentreEmpty() const;

        [[nodiscard]] bool IsSolid(u32 px, u32 py, u32 pz) const
        {
            return (SolidY[static_cast<sizet>(pz) * CS_P + px] & (u64{ 1 } << py)) != 0;
        }

        void SetSolid(u32 px, u32 py, u32 pz)
        {
            SolidY[static_cast<sizet>(pz) * CS_P + px] |= (u64{ 1 } << py);
        }

        [[nodiscard]] u8 MaterialAt(u32 lx, u32 ly, u32 lz) const
        {
            return Materials.empty() ? u8{ 0 } : Materials[VoxelChunk::Index(lx, ly, lz)];
        }
    };

    // -------------------------------------------------------------------------
    // The mesher
    // -------------------------------------------------------------------------
    //
    // Holds ~50 KiB of scratch, so it is an object rather than a free function:
    // create one per worker (or one per rebuild) and reuse it across chunks.
    // NOT thread-safe — one instance per thread.
    class VoxelGreedyMesher
    {
      public:
        VoxelGreedyMesher();

        // Gathers the padded neighbourhood for `coord` out of `voxels`.
        // Missing neighbour chunks read as empty, which is the correct
        // open-world behaviour: the chunk's outer faces are drawn.
        static void Gather(const VoxelOverride& voxels, const VoxelCoord& coord, VoxelNeighbourhood& out);

        // The greedy pass. Appends to `outQuads` (does not clear it).
        void Mesh(const VoxelNeighbourhood& neighbourhood, std::vector<PackedQuad>& outQuads);

        // Reference mesher: one 1x1 quad per visible face, derived directly
        // from the padded solidity with per-voxel neighbour tests. Deliberately
        // shares NO code with Mesh() — it exists so a test can compare two
        // independent derivations of the same face set. Not used at runtime.
        static void MeshNaive(const VoxelNeighbourhood& neighbourhood, std::vector<PackedQuad>& outQuads);

        // Expands one merged quad into the unit faces it covers. Used by tests
        // to compare a greedy mesh against a naive one face-for-face.
        static void ExpandQuad(const PackedQuad& quad, std::vector<PackedQuad>& outUnitQuads);

      private:
        static constexpr u32 CS = VoxelNeighbourhood::CS;
        static constexpr u32 CS_P = VoxelNeighbourhood::CS_P;

        // Emits every quad of one face direction into outQuads.
        void MeshDirection(const VoxelNeighbourhood& neighbourhood, VoxelFace face,
                           const std::vector<u64>& faceMask, std::vector<PackedQuad>& outQuads);

        // Solid bit columns, one array per axis:
        //   [0] bits along X, indexed [pz * CS_P + py]
        //   [1] bits along Y, indexed [pz * CS_P + px]  (the snapshot's own layout)
        //   [2] bits along Z, indexed [py * CS_P + px]
        std::array<std::vector<u64>, 3> m_AxisColumns;

        // Face mask of the direction being meshed, in that direction's axis layout.
        std::vector<u64> m_FaceMask;

        // Per-slice face bitmaps for the current direction: m_Planes[slice][row],
        // bit = the plane's inner coordinate. Slice/row/inner are chunk-local.
        std::array<std::array<u32, CS>, CS> m_Planes{};
    };
} // namespace OloEngine
