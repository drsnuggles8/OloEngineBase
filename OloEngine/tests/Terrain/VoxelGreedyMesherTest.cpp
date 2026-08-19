// OLO_TEST_LAYER: unit
//
// Contract tests for the binary greedy mesher and its packed-quad encoding
// (issue #727). Pure CPU — no GL context, no task system.
//
// The load-bearing test here is GreedyFaceSetMatchesNaive: it expands every
// merged quad back into the unit faces it covers and compares that MULTISET
// against an independently written per-voxel mesher. Comparing counts alone
// would pass on exactly the bug this exists to catch (a quad emitted twice and
// another dropped), so the comparison is element-wise and in both directions.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMesher.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/Voxel/VoxelQuad.h"

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr u32 CS = VoxelChunk::CHUNK_SIZE;

    // Fills a chunk from a solidity predicate. Solid = negative SDF.
    void FillChunk(VoxelChunk& chunk, const std::function<bool(u32, u32, u32)>& isSolid)
    {
        for (u32 z = 0; z < CS; ++z)
        {
            for (u32 y = 0; y < CS; ++y)
            {
                for (u32 x = 0; x < CS; ++x)
                {
                    chunk.At(x, y, z) = isSolid(x, y, z) ? -1.0f : 1.0f;
                }
            }
        }
    }

    // Returns a Ref, not a value: VoxelOverride derives from RefCounted, which
    // deletes the copy constructor, so it cannot be returned by value.
    Ref<VoxelOverride> MakeVolume(const std::function<bool(u32, u32, u32)>& isSolid,
                                  const VoxelCoord& coord = { 0, 0, 0 })
    {
        auto voxels = Ref<VoxelOverride>::Create();
        voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
        FillChunk(voxels->GetOrCreateChunk(coord), isSolid);
        return voxels;
    }

    VoxelNeighbourhood GatherOne(const VoxelOverride& voxels, const VoxelCoord& coord = { 0, 0, 0 })
    {
        VoxelNeighbourhood neighbourhood;
        VoxelGreedyMesher::Gather(voxels, coord, neighbourhood);
        return neighbourhood;
    }

    std::vector<PackedQuad> MeshGreedy(const VoxelNeighbourhood& neighbourhood)
    {
        VoxelGreedyMesher mesher;
        std::vector<PackedQuad> quads;
        mesher.Mesh(neighbourhood, quads);
        return quads;
    }

    std::vector<PackedQuad> MeshNaive(const VoxelNeighbourhood& neighbourhood)
    {
        std::vector<PackedQuad> quads;
        VoxelGreedyMesher::MeshNaive(neighbourhood, quads);
        return quads;
    }

    std::vector<PackedQuad> ExpandAll(const std::vector<PackedQuad>& quads)
    {
        std::vector<PackedQuad> unit;
        for (const auto& quad : quads)
        {
            VoxelGreedyMesher::ExpandQuad(quad, unit);
        }
        return unit;
    }

    // Multiset keyed by the full 64 bits of the record, so a duplicated face
    // and a dropped face cannot cancel each other out in the comparison.
    std::map<std::pair<u32, u32>, u32> ToMultiset(const std::vector<PackedQuad>& quads)
    {
        std::map<std::pair<u32, u32>, u32> counts;
        for (const auto& quad : quads)
        {
            ++counts[{ quad.Geometry, quad.Material }];
        }
        return counts;
    }

    // Deterministic pseudo-random solidity — a fixed hash, not a PRNG, so the
    // volume is identical on every platform and every run.
    bool HashSolid(u32 x, u32 y, u32 z, u32 salt)
    {
        u32 h = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu ^ z * 0xC2B2AE35u ^ salt;
        h ^= h >> 15;
        h *= 0x2545F491u;
        h ^= h >> 13;
        return (h & 3u) != 0u; // ~75% solid: dense enough to merge, holey enough to be interesting
    }
} // namespace

// =============================================================================
// Packed-quad encoding
// =============================================================================

TEST(VoxelQuadEncoding, RoundTripsEveryFieldExtreme)
{
    const std::vector<VoxelQuadFields> cases = {
        { 0, 0, 0, 1, 1, VoxelFace::PosX },
        { CS - 1, CS - 1, CS - 1, 1, 1, VoxelFace::NegZ },
        { 5, 17, 30, 7, 12, VoxelFace::PosY },
        // The full-chunk span: exactly the case the reference implementation's
        // unbiased 5-bit extents cannot express (32 masks to 0).
        { 0, 0, 0, CS, CS, VoxelFace::NegY },
        { 0, 0, 0, CS, 1, VoxelFace::PosZ },
        { 0, 0, 0, 1, CS, VoxelFace::NegX },
    };

    for (const auto& fields : cases)
    {
        ASSERT_TRUE(VoxelQuadCodec::IsEncodable(fields));
        const u32 geometry = VoxelQuadCodec::EncodeGeometry(fields);
        EXPECT_EQ(VoxelQuadCodec::DecodeGeometry(geometry), fields);
    }
}

TEST(VoxelQuadEncoding, FullSpanExtentSurvivesTheRoundTrip)
{
    // Guards the single divergence from the reference encoding. If someone
    // "simplifies" the bias away, this is what fails.
    const VoxelQuadFields fields{ 0, 0, 0, CS, CS, VoxelFace::PosY };
    const VoxelQuadFields decoded = VoxelQuadCodec::DecodeGeometry(VoxelQuadCodec::EncodeGeometry(fields));
    EXPECT_EQ(decoded.Width, CS);
    EXPECT_EQ(decoded.Height, CS);
}

TEST(VoxelQuadEncoding, RejectsValuesTheWidthCannotRepresent)
{
    EXPECT_FALSE(VoxelQuadCodec::IsEncodable({ CS, 0, 0, 1, 1, VoxelFace::PosX }));     // x out of range
    EXPECT_FALSE(VoxelQuadCodec::IsEncodable({ 0, 0, 0, 0, 1, VoxelFace::PosX }));      // zero width
    EXPECT_FALSE(VoxelQuadCodec::IsEncodable({ 0, 0, 0, CS + 1, 1, VoxelFace::PosX })); // width past a chunk
    EXPECT_FALSE(VoxelQuadCodec::IsEncodable({ 0, 0, 0, 1, CS + 1, VoxelFace::PosX }));
    EXPECT_FALSE(VoxelQuadCodec::IsEncodable({ 0, 0, 0, 1, 1, static_cast<VoxelFace>(6) }));
}

TEST(VoxelQuadEncoding, GeometryWordLeavesTheReservedBitsZero)
{
    const VoxelQuadFields fields{ CS - 1, CS - 1, CS - 1, CS, CS, VoxelFace::NegZ };
    const u32 geometry = VoxelQuadCodec::EncodeGeometry(fields);
    const u32 reservedMask = ~((1u << VoxelQuadEncoding::kUsedBits) - 1u);
    EXPECT_EQ(geometry & reservedMask, 0u) << "reserved bits must stay zero for a future reader";
}

TEST(VoxelQuadEncoding, MaterialRoundTrips)
{
    for (u32 material = 0; material < 256; ++material)
    {
        const PackedQuad quad = VoxelQuadCodec::Encode({ 1, 2, 3, 4, 5, VoxelFace::PosZ }, static_cast<u8>(material));
        EXPECT_EQ(VoxelQuadCodec::DecodeMaterialIndex(quad.Material), static_cast<u8>(material));
        EXPECT_EQ(quad.Material >> VoxelQuadEncoding::kMaterialIndexBits, 0u);
    }
}

// =============================================================================
// The face basis — the CPU half of the shader contract
// =============================================================================

TEST(VoxelFaceBasisContract, CrossOfUAndVIsTheOutwardNormal)
{
    // If this fails, the corresponding face renders inside-out under backface
    // culling — visible only when you fly past that one direction.
    for (u32 face = 0; face < 6; ++face)
    {
        const auto& u = VoxelFaceBasis::kAxisU[face];
        const auto& v = VoxelFaceBasis::kAxisV[face];
        const auto& n = VoxelFaceBasis::kNormal[face];

        const i32 cross[3] = {
            u[1] * v[2] - u[2] * v[1],
            u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0],
        };

        EXPECT_EQ(cross[0], n[0]) << "face " << face;
        EXPECT_EQ(cross[1], n[1]) << "face " << face;
        EXPECT_EQ(cross[2], n[2]) << "face " << face;
    }
}

TEST(VoxelFaceBasisContract, PositiveFacesOffsetOneVoxelAlongTheirAxis)
{
    for (u32 face = 0; face < 6; ++face)
    {
        for (u32 axis = 0; axis < 3; ++axis)
        {
            const i32 expected = (VoxelFaceBasis::kNormal[face][axis] > 0) ? 1 : 0;
            EXPECT_EQ(VoxelFaceBasis::kOriginOffset[face][axis], expected) << "face " << face << " axis " << axis;
        }
    }
}

TEST(VoxelFaceBasisContract, UAndVAreBothPositiveUnitAxes)
{
    // The expansion in ExpandQuad and the shader's origin + U*w + V*h both
    // assume non-negative axis components; a negative one would walk a quad
    // out of the chunk.
    for (u32 face = 0; face < 6; ++face)
    {
        i32 sumU = 0;
        i32 sumV = 0;
        for (u32 axis = 0; axis < 3; ++axis)
        {
            EXPECT_GE(VoxelFaceBasis::kAxisU[face][axis], 0);
            EXPECT_GE(VoxelFaceBasis::kAxisV[face][axis], 0);
            sumU += VoxelFaceBasis::kAxisU[face][axis];
            sumV += VoxelFaceBasis::kAxisV[face][axis];
        }
        EXPECT_EQ(sumU, 1) << "face " << face;
        EXPECT_EQ(sumV, 1) << "face " << face;
    }
}

// =============================================================================
// Greedy vs naive: identical face sets
// =============================================================================

class VoxelGreedyMesherFaceSet : public ::testing::TestWithParam<int>
{
};

TEST_P(VoxelGreedyMesherFaceSet, GreedyFaceSetMatchesNaive)
{
    const int shape = GetParam();
    auto solidity = [shape](u32 x, u32 y, u32 z) -> bool
    {
        switch (shape)
        {
            case 0: // fully solid
                return true;
            case 1: // hollow-ish sphere
            {
                const f32 dx = static_cast<f32>(x) - 15.5f;
                const f32 dy = static_cast<f32>(y) - 15.5f;
                const f32 dz = static_cast<f32>(z) - 15.5f;
                return (dx * dx + dy * dy + dz * dz) < (12.0f * 12.0f);
            }
            case 2: // 3D checkerboard — nothing can merge
                return ((x + y + z) & 1u) == 0u;
            case 3: // heightfield: a blocky terrain column per (x, z)
                return y < (8u + ((x * 3u + z * 5u) % 12u));
            case 4: // single voxel
                return x == 7 && y == 9 && z == 11;
            case 5: // pseudo-random
                return HashSolid(x, y, z, 0x1234u);
            case 6: // slabs — merges hugely along two axes, not the third
                return (y % 4u) < 2u;
            default:
                return false;
        }
    };

    const auto voxels = MakeVolume(solidity);
    const VoxelNeighbourhood neighbourhood = GatherOne(*voxels);

    const std::vector<PackedQuad> greedy = MeshGreedy(neighbourhood);
    const std::vector<PackedQuad> naive = MeshNaive(neighbourhood);
    const std::vector<PackedQuad> expanded = ExpandAll(greedy);

    // Element-wise multiset equality, not a count comparison.
    EXPECT_EQ(ToMultiset(expanded), ToMultiset(naive))
        << "shape " << shape << ": greedy quads do not expand to the naive face set";

    // And no unit face appears twice — a double-drawn face is a distinct bug
    // from a wrong face set and would z-fight rather than change the silhouette.
    for (const auto& [key, count] : ToMultiset(expanded))
    {
        EXPECT_EQ(count, 1u) << "shape " << shape << ": a unit face is covered by more than one quad";
    }

    if (shape != 4)
    {
        EXPECT_FALSE(greedy.empty()) << "shape " << shape;
    }
    EXPECT_LE(greedy.size(), naive.size()) << "merging must never increase the quad count";
}

INSTANTIATE_TEST_SUITE_P(Shapes, VoxelGreedyMesherFaceSet, ::testing::Values(0, 1, 2, 3, 4, 5, 6));

// =============================================================================
// Merge quality
// =============================================================================

TEST(VoxelGreedyMesher, SolidChunkCollapsesToSixFullFaces)
{
    // The strongest concrete statement of maximality, and the case that needs
    // the biased extent encoding: a 32-wide run.
    const auto voxels = MakeVolume([](u32, u32, u32)
                                   { return true; });
    const std::vector<PackedQuad> quads = MeshGreedy(GatherOne(*voxels));

    ASSERT_EQ(quads.size(), 6u);

    std::vector<bool> seenFace(6, false);
    for (const auto& quad : quads)
    {
        const VoxelQuadFields fields = VoxelQuadCodec::DecodeGeometry(quad.Geometry);
        EXPECT_EQ(fields.Width, CS);
        EXPECT_EQ(fields.Height, CS);
        const auto faceIndex = static_cast<u32>(fields.Face);
        ASSERT_LT(faceIndex, 6u);
        EXPECT_FALSE(seenFace[faceIndex]) << "face " << faceIndex << " emitted twice";
        seenFace[faceIndex] = true;
    }
    EXPECT_EQ(std::ranges::count(seenFace, true), 6);
}

TEST(VoxelGreedyMesher, CheckerboardCannotMerge)
{
    const auto voxels = MakeVolume([](u32 x, u32 y, u32 z)
                                   { return ((x + y + z) & 1u) == 0u; });
    const VoxelNeighbourhood neighbourhood = GatherOne(*voxels);

    EXPECT_EQ(MeshGreedy(neighbourhood).size(), MeshNaive(neighbourhood).size())
        << "no two faces of a checkerboard are adjacent, so there is nothing to merge";
}

TEST(VoxelGreedyMesher, FlatSlabMergesToFarFewerQuadsThanFaces)
{
    // A blocky terrain's dominant case. Not a performance assertion — a
    // correctness one: if the merge silently stopped working, this is the
    // cheapest place it shows.
    const auto voxels = MakeVolume([](u32, u32 y, u32)
                                   { return y < 8; });
    const VoxelNeighbourhood neighbourhood = GatherOne(*voxels);

    const sizet greedyQuads = MeshGreedy(neighbourhood).size();
    const sizet naiveFaces = MeshNaive(neighbourhood).size();

    EXPECT_GT(naiveFaces, greedyQuads * 50) << greedyQuads << " merged quads vs " << naiveFaces << " raw faces";
}

TEST(VoxelGreedyMesher, EmptyChunkEmitsNothing)
{
    const auto voxels = MakeVolume([](u32, u32, u32)
                                   { return false; });
    EXPECT_TRUE(MeshGreedy(GatherOne(*voxels)).empty());
}

TEST(VoxelGreedyMesher, MissingChunkEmitsNothing)
{
    auto voxels = Ref<VoxelOverride>::Create();
    voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
    EXPECT_TRUE(MeshGreedy(GatherOne(*voxels, { 4, 4, 4 })).empty());
}

// =============================================================================
// Materials
// =============================================================================

TEST(VoxelGreedyMesher, MergeStopsAtAMaterialBoundary)
{
    auto voxels = Ref<VoxelOverride>::Create();
    voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
    VoxelChunk& chunk = voxels->GetOrCreateChunk({ 0, 0, 0 });
    FillChunk(chunk, [](u32, u32 y, u32)
              { return y < 8; });

    // Split the top surface down the middle into two materials.
    for (u32 z = 0; z < CS; ++z)
    {
        for (u32 y = 0; y < CS; ++y)
        {
            for (u32 x = 0; x < CS; ++x)
            {
                chunk.SetMaterialAt(x, y, z, static_cast<u8>(x < 16 ? 2 : 3));
            }
        }
    }

    const VoxelNeighbourhood neighbourhood = GatherOne(*voxels);
    const std::vector<PackedQuad> quads = MeshGreedy(neighbourhood);

    // Face set is still exactly right...
    EXPECT_EQ(ToMultiset(ExpandAll(quads)), ToMultiset(MeshNaive(neighbourhood)));

    // ...and every quad covers voxels of exactly one material.
    for (const auto& quad : quads)
    {
        const u8 material = VoxelQuadCodec::DecodeMaterialIndex(quad.Material);
        std::vector<PackedQuad> unit;
        VoxelGreedyMesher::ExpandQuad(quad, unit);
        for (const auto& face : unit)
        {
            const VoxelQuadFields fields = VoxelQuadCodec::DecodeGeometry(face.Geometry);
            EXPECT_EQ(neighbourhood.MaterialAt(fields.X, fields.Y, fields.Z), material)
                << "a merged quad spans two materials";
        }
    }

    // The +Y surface must be split by the boundary: 2 quads, not 1.
    u32 topQuads = 0;
    for (const auto& quad : quads)
    {
        if (VoxelQuadCodec::DecodeGeometry(quad.Geometry).Face == VoxelFace::PosY)
        {
            ++topQuads;
        }
    }
    EXPECT_EQ(topQuads, 2u) << "the material seam should split the top face in two";
}

TEST(VoxelGreedyMesher, UniformMaterialTakesTheSameResultAsAnExplicitZeroArray)
{
    // The empty-MaterialData fast path must be indistinguishable from an
    // all-zero array — otherwise the fast path is a second implementation that
    // nothing compares against.
    const auto implicitVoxels = MakeVolume([](u32 x, u32 y, u32 z)
                                           { return HashSolid(x, y, z, 0x99u); });

    auto explicitVoxels = Ref<VoxelOverride>::Create();
    explicitVoxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
    VoxelChunk& chunk = explicitVoxels->GetOrCreateChunk({ 0, 0, 0 });
    FillChunk(chunk, [](u32 x, u32 y, u32 z)
              { return HashSolid(x, y, z, 0x99u); });
    chunk.MaterialData.assign(VoxelChunk::TOTAL_VOXELS, u8{ 0 });

    ASSERT_TRUE(implicitVoxels->GetChunks().at({ 0, 0, 0 }).MaterialData.empty());

    EXPECT_EQ(ToMultiset(MeshGreedy(GatherOne(*implicitVoxels))),
              ToMultiset(MeshGreedy(GatherOne(*explicitVoxels))));
}

// =============================================================================
// Chunk boundaries — the seam criterion, in CPU form
// =============================================================================

TEST(VoxelGreedyMesherBoundary, LoneChunkDrawsAllSixOuterFaces)
{
    const auto voxels = MakeVolume([](u32, u32, u32)
                                   { return true; });
    EXPECT_EQ(MeshGreedy(GatherOne(*voxels)).size(), 6u);
}

TEST(VoxelGreedyMesherBoundary, SharedFaceBetweenSolidNeighboursIsCulledOnBothSides)
{
    // A seam is exactly this: the interior face between two full chunks must
    // vanish, and its absence must be symmetric. If padding were dropped, both
    // chunks would draw an interior wall; if it were applied to one side only,
    // the pair would be lit inconsistently.
    auto voxels = Ref<VoxelOverride>::Create();
    voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
    for (i32 x = 0; x <= 1; ++x)
    {
        FillChunk(voxels->GetOrCreateChunk({ x, 0, 0 }), [](u32, u32, u32)
                  { return true; });
    }

    const std::vector<PackedQuad> left = MeshGreedy(GatherOne(*voxels, { 0, 0, 0 }));
    const std::vector<PackedQuad> right = MeshGreedy(GatherOne(*voxels, { 1, 0, 0 }));

    ASSERT_EQ(left.size(), 5u) << "the +X face of the left chunk must be culled";
    ASSERT_EQ(right.size(), 5u) << "the -X face of the right chunk must be culled";

    auto hasFace = [](const std::vector<PackedQuad>& quads, VoxelFace face)
    {
        return std::ranges::any_of(quads, [face](const PackedQuad& q)
                                   { return VoxelQuadCodec::DecodeGeometry(q.Geometry).Face == face; });
    };

    EXPECT_FALSE(hasFace(left, VoxelFace::PosX));
    EXPECT_TRUE(hasFace(left, VoxelFace::NegX));
    EXPECT_FALSE(hasFace(right, VoxelFace::NegX));
    EXPECT_TRUE(hasFace(right, VoxelFace::PosX));
}

TEST(VoxelGreedyMesherBoundary, EveryAxisCullsAgainstItsNeighbour)
{
    // The reference implementation special-cases +/-Y because its column type
    // has no room for Y padding. Ours does not, so all six directions must
    // behave identically — this is what proves that.
    constexpr i32 kOffsets[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    constexpr VoxelFace kFacingNeighbour[6] = {
        VoxelFace::PosX, VoxelFace::NegX, VoxelFace::PosY, VoxelFace::NegY, VoxelFace::PosZ, VoxelFace::NegZ
    };

    for (u32 i = 0; i < 6; ++i)
    {
        auto voxels = Ref<VoxelOverride>::Create();
        voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
        FillChunk(voxels->GetOrCreateChunk({ 0, 0, 0 }), [](u32, u32, u32)
                  { return true; });
        FillChunk(voxels->GetOrCreateChunk({ kOffsets[i][0], kOffsets[i][1], kOffsets[i][2] }),
                  [](u32, u32, u32)
                  { return true; });

        const std::vector<PackedQuad> quads = MeshGreedy(GatherOne(*voxels, { 0, 0, 0 }));
        EXPECT_EQ(quads.size(), 5u) << "neighbour direction " << i;

        const bool drawsFacingNeighbour =
            std::ranges::any_of(quads, [&](const PackedQuad& q)
                                { return VoxelQuadCodec::DecodeGeometry(q.Geometry).Face == kFacingNeighbour[i]; });
        EXPECT_FALSE(drawsFacingNeighbour) << "direction " << i << " draws a face into its solid neighbour";
    }
}

TEST(VoxelGreedyMesherBoundary, PartialNeighbourCullsOnlyTheCoveredFaces)
{
    // The half-covered case: only the faces the neighbour actually occludes
    // may disappear. An all-or-nothing padding bug passes the two tests above
    // and fails this one.
    auto voxels = Ref<VoxelOverride>::Create();
    voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
    FillChunk(voxels->GetOrCreateChunk({ 0, 0, 0 }), [](u32, u32, u32)
              { return true; });
    // Neighbour to +X is solid only in the lower half of Y.
    FillChunk(voxels->GetOrCreateChunk({ 1, 0, 0 }), [](u32, u32 y, u32)
              { return y < CS / 2; });

    const std::vector<PackedQuad> quads = MeshGreedy(GatherOne(*voxels, { 0, 0, 0 }));

    u32 posXArea = 0;
    for (const auto& quad : quads)
    {
        const VoxelQuadFields fields = VoxelQuadCodec::DecodeGeometry(quad.Geometry);
        if (fields.Face == VoxelFace::PosX)
        {
            posXArea += fields.Width * fields.Height;
        }
    }

    EXPECT_EQ(posXArea, CS * CS / 2) << "exactly the uncovered half of the +X wall should survive";
}

TEST(VoxelGreedyMesherBoundary, ChunkLocalCoordinatesStayInsideTheChunk)
{
    // The packed encoding has no room for an out-of-range coordinate, so an
    // off-by-one in the padded->local mapping would wrap silently.
    const auto voxels = MakeVolume([](u32 x, u32 y, u32 z)
                                   { return HashSolid(x, y, z, 0x77u); });
    for (const auto& quad : MeshGreedy(GatherOne(*voxels)))
    {
        const VoxelQuadFields fields = VoxelQuadCodec::DecodeGeometry(quad.Geometry);
        EXPECT_LT(fields.X, CS);
        EXPECT_LT(fields.Y, CS);
        EXPECT_LT(fields.Z, CS);
        EXPECT_GE(fields.Width, 1u);
        EXPECT_GE(fields.Height, 1u);
        EXPECT_LE(fields.Width, CS);
        EXPECT_LE(fields.Height, CS);
    }
}

TEST(VoxelGreedyMesher, RepeatedMeshingOfTheSameSnapshotIsStable)
{
    // The mesher carries scratch state across calls; a missing reset would show
    // up here as a second run that differs from the first.
    const auto voxels = MakeVolume([](u32 x, u32 y, u32 z)
                                   { return HashSolid(x, y, z, 0x55u); });
    const VoxelNeighbourhood neighbourhood = GatherOne(*voxels);

    VoxelGreedyMesher mesher;
    std::vector<PackedQuad> first;
    std::vector<PackedQuad> second;
    mesher.Mesh(neighbourhood, first);
    mesher.Mesh(neighbourhood, second);

    EXPECT_EQ(ToMultiset(first), ToMultiset(second));
}
