#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMesher.h"

#include <algorithm>
#include <bit>

namespace OloEngine
{
    namespace
    {
        constexpr u32 CS = VoxelNeighbourhood::CS;
        constexpr u32 CS_P = VoxelNeighbourhood::CS_P;

        // Which axis each face direction runs along: X = 0, Y = 1, Z = 2.
        constexpr u32 kFaceAxis[6] = { 0, 0, 1, 1, 2, 2 };

        // True for the three directions that point along +axis.
        constexpr bool kFacePositive[6] = { true, false, true, false, true, false };

        // The greedy pass merges first along the plane's INNER coordinate (the
        // bit axis of a plane row) and then along its OUTER coordinate (the row
        // index). Which of the two the packed quad calls Width depends on the
        // face, because VoxelFaceBasis orders U/V so that cross(U, V) == normal
        // — see the table in VoxelQuad.h. Getting this wrong does not lose a
        // quad, it transposes it: a 1x8 strip renders as 8x1.
        //
        //   PosX: U = +Y = inner  -> Width is the inner extent
        //   NegX: U = +Z = outer  -> Width is the outer extent
        //   PosY: U = +Z = outer
        //   NegY: U = +X = inner
        //   PosZ: U = +X = inner
        //   NegZ: U = +Y = outer
        constexpr bool kWidthIsInnerExtent[6] = { true, false, false, true, true, false };

        // Plane coordinates per axis. The plane at slice `s` on axis `a` is
        // spanned by the other two axes; `inner` is the bit axis of a plane row
        // and `outer` is the row index.
        //   axis 0 (X faces): inner = Y, outer = Z
        //   axis 1 (Y faces): inner = X, outer = Z
        //   axis 2 (Z faces): inner = X, outer = Y
        constexpr u32 kInnerAxis[3] = { 1, 0, 0 };
        constexpr u32 kOuterAxis[3] = { 2, 2, 1 };

        // Chunk-local voxel coordinate of the face owner, from its plane address.
        constexpr void LocalFromPlane(u32 axis, u32 slice, u32 inner, u32 outer, u32& lx, u32& ly, u32& lz)
        {
            u32 coords[3] = { 0, 0, 0 };
            coords[axis] = slice;
            coords[kInnerAxis[axis]] = inner;
            coords[kOuterAxis[axis]] = outer;
            lx = coords[0];
            ly = coords[1];
            lz = coords[2];
        }

        // Bits [1, CS] of a padded column — the slices that belong to the centre
        // chunk. Bit 0 and bit CS+1 address padding voxels, which never own a face.
        constexpr u64 kCentreSliceMask = (((u64{ 1 } << CS) - 1) << 1);

        // Low `count` bits set, guarding the count == 32 shift that would be UB.
        constexpr u32 LowBits(u32 count)
        {
            return count >= 32u ? ~u32{ 0 } : ((u32{ 1 } << count) - 1u);
        }
    } // namespace

    bool VoxelNeighbourhood::IsCentreEmpty() const
    {
        for (u32 pz = 1; pz <= CS; ++pz)
        {
            for (u32 px = 1; px <= CS; ++px)
            {
                if ((SolidY[static_cast<sizet>(pz) * CS_P + px] & kCentreSliceMask) != 0)
                {
                    return false;
                }
            }
        }
        return true;
    }

    VoxelGreedyMesher::VoxelGreedyMesher()
    {
        for (auto& columns : m_AxisColumns)
        {
            columns.assign(VoxelNeighbourhood::COLUMN_COUNT, 0);
        }
        m_FaceMask.assign(VoxelNeighbourhood::COLUMN_COUNT, 0);
    }

    // -------------------------------------------------------------------------
    // Gather
    // -------------------------------------------------------------------------

    void VoxelGreedyMesher::Gather(const VoxelOverride& voxels, const VoxelCoord& coord, VoxelNeighbourhood& out)
    {
        OLO_PROFILE_FUNCTION();

        out.SolidY.fill(0);
        out.Materials.clear();
        out.Coord = coord;

        const auto& chunks = voxels.GetChunks();
        auto findChunk = [&chunks](const VoxelCoord& c) -> const VoxelChunk*
        {
            auto it = chunks.find(c);
            return it != chunks.end() ? &it->second : nullptr;
        };

        const VoxelChunk* centre = findChunk(coord);
        if (centre == nullptr)
        {
            return;
        }

        // Centre chunk occupies padded [1, CS] on every axis.
        for (u32 z = 0; z < CS; ++z)
        {
            for (u32 x = 0; x < CS; ++x)
            {
                u64 column = 0;
                for (u32 y = 0; y < CS; ++y)
                {
                    // Negative SDF = solid. A NaN compares false, i.e. reads as
                    // empty, which is the safe direction for corrupt data.
                    if (centre->At(x, y, z) < 0.0f)
                    {
                        column |= (u64{ 1 } << (y + 1));
                    }
                }
                out.SolidY[static_cast<sizet>(z + 1) * CS_P + (x + 1)] = column;
            }
        }

        if (!centre->MaterialData.empty())
        {
            out.Materials = centre->MaterialData;
        }

        // Face neighbours only. An edge/corner neighbour could not contribute a
        // padded cell any face of a centre voxel reads (see VoxelNeighbourhood),
        // so 6 slabs is the whole of it — not 26.
        //
        // One generic slab copy rather than six near-identical loops: the axis
        // the direction runs along is the only thing that differs, and six
        // hand-written copies is six places for an inverted source/destination
        // face to hide.
        auto gatherNeighbourSlab = [&out, &findChunk, &coord](i32 dx, i32 dy, i32 dz)
        {
            const VoxelChunk* neighbour = findChunk({ coord.X + dx, coord.Y + dy, coord.Z + dz });
            if (neighbour == nullptr)
            {
                return; // Absent neighbour reads as empty: the outer face draws.
            }

            const i32 delta[3] = { dx, dy, dz };
            u32 axis = 0;
            for (u32 i = 0; i < 3; ++i)
            {
                if (delta[i] != 0)
                {
                    axis = i;
                }
            }

            // Toward +axis we want the neighbour's LOW face and our HIGH padding
            // plane, and vice versa. Getting this pair backwards culls against
            // the wrong slab, which looks like a seam only where the two chunks
            // happen to differ.
            const u32 sourceSlice = (delta[axis] > 0) ? 0u : (CS - 1);
            const u32 paddedSlice = (delta[axis] > 0) ? (CS + 1) : 0u;

            const u32 axisA = (axis + 1) % 3;
            const u32 axisB = (axis + 2) % 3;

            for (u32 a = 0; a < CS; ++a)
            {
                for (u32 b = 0; b < CS; ++b)
                {
                    u32 source[3] = { 0, 0, 0 };
                    source[axis] = sourceSlice;
                    source[axisA] = a;
                    source[axisB] = b;

                    if (neighbour->At(source[0], source[1], source[2]) >= 0.0f)
                    {
                        continue;
                    }

                    u32 padded[3] = { 0, 0, 0 };
                    padded[axis] = paddedSlice;
                    padded[axisA] = a + 1;
                    padded[axisB] = b + 1;
                    out.SetSolid(padded[0], padded[1], padded[2]);
                }
            }
        };

        gatherNeighbourSlab(-1, 0, 0);
        gatherNeighbourSlab(1, 0, 0);
        gatherNeighbourSlab(0, -1, 0);
        gatherNeighbourSlab(0, 1, 0);
        gatherNeighbourSlab(0, 0, -1);
        gatherNeighbourSlab(0, 0, 1);
    }

    // -------------------------------------------------------------------------
    // Greedy pass
    // -------------------------------------------------------------------------

    void VoxelGreedyMesher::Mesh(const VoxelNeighbourhood& neighbourhood, std::vector<PackedQuad>& outQuads)
    {
        OLO_PROFILE_FUNCTION();

        if (neighbourhood.IsCentreEmpty())
        {
            return;
        }

        // Axis 1 (bits along Y) IS the snapshot's own layout; the other two are
        // transposes of it. Iterating set bits keeps this O(solid voxels)
        // rather than O(padded volume).
        std::ranges::copy(neighbourhood.SolidY, m_AxisColumns[1].begin());
        std::ranges::fill(m_AxisColumns[0], 0);
        std::ranges::fill(m_AxisColumns[2], 0);

        for (u32 pz = 0; pz < CS_P; ++pz)
        {
            for (u32 px = 0; px < CS_P; ++px)
            {
                u64 column = neighbourhood.SolidY[static_cast<sizet>(pz) * CS_P + px];
                while (column != 0)
                {
                    const u32 py = static_cast<u32>(std::countr_zero(column));
                    column &= column - 1;

                    m_AxisColumns[0][static_cast<sizet>(pz) * CS_P + py] |= (u64{ 1 } << px);
                    m_AxisColumns[2][static_cast<sizet>(py) * CS_P + px] |= (u64{ 1 } << pz);
                }
            }
        }

        for (u32 face = 0; face < 6; ++face)
        {
            const u32 axis = kFaceAxis[face];
            const auto& columns = m_AxisColumns[axis];

            // A face exists where a solid voxel has an empty neighbour one step
            // along the direction. The shifted-out end reads as empty, which is
            // harmless: only slices [1, CS] are ever emitted.
            if (kFacePositive[face])
            {
                for (sizet i = 0; i < columns.size(); ++i)
                {
                    m_FaceMask[i] = columns[i] & ~(columns[i] >> 1);
                }
            }
            else
            {
                for (sizet i = 0; i < columns.size(); ++i)
                {
                    m_FaceMask[i] = columns[i] & ~(columns[i] << 1);
                }
            }

            MeshDirection(neighbourhood, static_cast<VoxelFace>(face), m_FaceMask, outQuads);
        }
    }

    void VoxelGreedyMesher::MeshDirection(const VoxelNeighbourhood& neighbourhood, VoxelFace face,
                                          const std::vector<u64>& faceMask, std::vector<PackedQuad>& outQuads)
    {
        const u32 faceIndex = static_cast<u32>(face);
        const u32 axis = kFaceAxis[faceIndex];

        for (auto& plane : m_Planes)
        {
            plane.fill(0);
        }

        // Transpose the axis-aligned face columns into one bitmap per slice.
        // Only the centre columns can own a face, so the double loop is CS^2 and
        // the inner set-bit walk is O(faces).
        for (u32 outerP = 1; outerP <= CS; ++outerP)
        {
            for (u32 innerP = 1; innerP <= CS; ++innerP)
            {
                u64 column = faceMask[static_cast<sizet>(outerP) * CS_P + innerP] & kCentreSliceMask;
                while (column != 0)
                {
                    const u32 sliceP = static_cast<u32>(std::countr_zero(column));
                    column &= column - 1;
                    m_Planes[sliceP - 1][outerP - 1] |= (u32{ 1 } << (innerP - 1));
                }
            }
        }

        // A chunk with no material array is uniformly material 0, so every
        // material comparison below can be skipped — the merge is then purely
        // bitwise, which is the common case and the fast path.
        const bool uniformMaterial = neighbourhood.Materials.empty();

        auto materialAt = [&](u32 slice, u32 inner, u32 outer) -> u8
        {
            u32 lx = 0;
            u32 ly = 0;
            u32 lz = 0;
            LocalFromPlane(axis, slice, inner, outer, lx, ly, lz);
            return neighbourhood.MaterialAt(lx, ly, lz);
        };

        for (u32 slice = 0; slice < CS; ++slice)
        {
            auto& plane = m_Planes[slice];

            for (u32 outer = 0; outer < CS; ++outer)
            {
                u32 rowBits = plane[outer];
                while (rowBits != 0)
                {
                    const u32 inner = static_cast<u32>(std::countr_zero(rowBits));

                    // Longest run of visible faces along the inner axis...
                    u32 innerExtent = static_cast<u32>(std::countr_one(rowBits >> inner));

                    // ...clipped to the run that shares one material.
                    const u8 material = uniformMaterial ? u8{ 0 } : materialAt(slice, inner, outer);
                    if (!uniformMaterial)
                    {
                        u32 uniformRun = 1;
                        while (uniformRun < innerExtent && materialAt(slice, inner + uniformRun, outer) == material)
                        {
                            ++uniformRun;
                        }
                        innerExtent = uniformRun;
                    }

                    const u32 runMask = LowBits(innerExtent) << inner;

                    // Expand along the outer axis while whole rows match.
                    u32 outerExtent = 1;
                    while (outer + outerExtent < CS)
                    {
                        u32& candidate = plane[outer + outerExtent];
                        if ((candidate & runMask) != runMask)
                        {
                            break;
                        }
                        if (!uniformMaterial)
                        {
                            bool materialsMatch = true;
                            for (u32 i = 0; i < innerExtent; ++i)
                            {
                                if (materialAt(slice, inner + i, outer + outerExtent) != material)
                                {
                                    materialsMatch = false;
                                    break;
                                }
                            }
                            if (!materialsMatch)
                            {
                                break;
                            }
                        }

                        candidate &= ~runMask; // Consumed — never emitted twice.
                        ++outerExtent;
                    }

                    u32 lx = 0;
                    u32 ly = 0;
                    u32 lz = 0;
                    LocalFromPlane(axis, slice, inner, outer, lx, ly, lz);

                    VoxelQuadFields fields;
                    fields.X = lx;
                    fields.Y = ly;
                    fields.Z = lz;
                    fields.Face = face;
                    fields.Width = kWidthIsInnerExtent[faceIndex] ? innerExtent : outerExtent;
                    fields.Height = kWidthIsInnerExtent[faceIndex] ? outerExtent : innerExtent;

                    OLO_CORE_ASSERT(VoxelQuadCodec::IsEncodable(fields), "Greedy quad outside the packed encoding range");
                    outQuads.push_back(VoxelQuadCodec::Encode(fields, material));

                    rowBits &= ~runMask;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Reference mesher (tests only)
    // -------------------------------------------------------------------------

    void VoxelGreedyMesher::MeshNaive(const VoxelNeighbourhood& neighbourhood, std::vector<PackedQuad>& outQuads)
    {
        // Deliberately naive and deliberately independent of Mesh(): plain
        // per-voxel neighbour tests straight off the padded solidity, so a bug
        // in the bitwise face culling cannot hide behind shared code.
        for (u32 lz = 0; lz < CS; ++lz)
        {
            for (u32 ly = 0; ly < CS; ++ly)
            {
                for (u32 lx = 0; lx < CS; ++lx)
                {
                    if (!neighbourhood.IsSolid(lx + 1, ly + 1, lz + 1))
                    {
                        continue;
                    }

                    const u8 material = neighbourhood.MaterialAt(lx, ly, lz);

                    for (u32 faceIndex = 0; faceIndex < 6; ++faceIndex)
                    {
                        const i32 nx = static_cast<i32>(lx + 1) + VoxelFaceBasis::kNormal[faceIndex][0];
                        const i32 ny = static_cast<i32>(ly + 1) + VoxelFaceBasis::kNormal[faceIndex][1];
                        const i32 nz = static_cast<i32>(lz + 1) + VoxelFaceBasis::kNormal[faceIndex][2];

                        if (neighbourhood.IsSolid(static_cast<u32>(nx), static_cast<u32>(ny), static_cast<u32>(nz)))
                        {
                            continue;
                        }

                        VoxelQuadFields fields;
                        fields.X = lx;
                        fields.Y = ly;
                        fields.Z = lz;
                        fields.Width = 1;
                        fields.Height = 1;
                        fields.Face = static_cast<VoxelFace>(faceIndex);
                        outQuads.push_back(VoxelQuadCodec::Encode(fields, material));
                    }
                }
            }
        }
    }

    void VoxelGreedyMesher::ExpandQuad(const PackedQuad& quad, std::vector<PackedQuad>& outUnitQuads)
    {
        const VoxelQuadFields fields = VoxelQuadCodec::DecodeGeometry(quad.Geometry);
        const u32 faceIndex = static_cast<u32>(fields.Face);

        // Step the owning voxel along the same U/V axes the shader extends the
        // quad along, so the expansion is the exact inverse of the merge.
        for (u32 v = 0; v < fields.Height; ++v)
        {
            for (u32 u = 0; u < fields.Width; ++u)
            {
                VoxelQuadFields unit;
                unit.X = fields.X + u * static_cast<u32>(VoxelFaceBasis::kAxisU[faceIndex][0]) +
                         v * static_cast<u32>(VoxelFaceBasis::kAxisV[faceIndex][0]);
                unit.Y = fields.Y + u * static_cast<u32>(VoxelFaceBasis::kAxisU[faceIndex][1]) +
                         v * static_cast<u32>(VoxelFaceBasis::kAxisV[faceIndex][1]);
                unit.Z = fields.Z + u * static_cast<u32>(VoxelFaceBasis::kAxisU[faceIndex][2]) +
                         v * static_cast<u32>(VoxelFaceBasis::kAxisV[faceIndex][2]);
                unit.Width = 1;
                unit.Height = 1;
                unit.Face = fields.Face;

                outUnitQuads.push_back(PackedQuad{ VoxelQuadCodec::EncodeGeometry(unit), quad.Material });
            }
        }
    }
} // namespace OloEngine
