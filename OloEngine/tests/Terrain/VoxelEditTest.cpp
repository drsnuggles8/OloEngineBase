// OLO_TEST_LAYER: unit
//
// Pure CPU contracts for voxel picking/editing. These deliberately traverse
// negative coordinates and a chunk edge: those are the two places where a
// seemingly-correct local-index implementation normally breaks.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include <cstring>

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Terrain/Voxel/VoxelEdit.h"

using namespace OloEngine;

namespace
{
    Ref<VoxelOverride> MakeVoxels()
    {
        auto voxels = Ref<VoxelOverride>::Create();
        voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);
        return voxels;
    }
} // namespace

TEST(VoxelRaycast, HitsExactVoxelAndEnteredFaceAcrossChunkBoundary)
{
    auto voxels = MakeVoxels();
    voxels->GetOrCreateChunk({ 0, 0, 0 });    // empty source chunk; traversal must cross into chunk 1
    voxels->SetVoxel({ 32, 2, 3 }, -1.0f, 4); // first cell of chunk (1, 0, 0)

    const VoxelRayHit hit = RaycastVoxels(*voxels, Ray({ 30.25f, 2.5f, 3.5f }, { 1.0f, 0.0f, 0.0f }, 0.0f, 10.0f));

    ASSERT_TRUE(hit.Hit);
    EXPECT_EQ(hit.Voxel, (VoxelGridCoord{ 32, 2, 3 }));
    EXPECT_EQ(hit.FaceNormal, glm::ivec3(-1, 0, 0));
    EXPECT_FLOAT_EQ(hit.Distance, 1.75f);
}

TEST(VoxelRaycast, MapsNegativeGridCoordinatesToTheCorrectChunk)
{
    auto voxels = MakeVoxels();
    voxels->SetVoxel({ -1, 0, 0 }, -1.0f);

    const VoxelRayHit hit = RaycastVoxels(*voxels, Ray({ 1.5f, 0.5f, 0.5f }, { -1.0f, 0.0f, 0.0f }, 0.0f, 4.0f));

    ASSERT_TRUE(hit.Hit);
    EXPECT_EQ(hit.Voxel, (VoxelGridCoord{ -1, 0, 0 }));
    EXPECT_EQ(hit.FaceNormal, glm::ivec3(1, 0, 0));
    EXPECT_TRUE(voxels->HasChunk({ -1, 0, 0 }));
}

TEST(VoxelEdit, UndoRedoRestoresMultiChunkStrokeExactly)
{
    auto voxels = MakeVoxels();
    voxels->SetVoxel({ 31, 0, 0 }, -1.0f, 1);
    const VoxelRayHit hit = RaycastVoxels(*voxels, Ray({ 29.5f, 0.5f, 0.5f }, { 1.0f, 0.0f, 0.0f }, 0.0f, 10.0f));
    ASSERT_TRUE(hit.Hit);

    const VoxelEditStroke stroke = ApplyVoxelBrush(*voxels, hit, { VoxelBrushOperation::Place, 2.0f, 6 });
    ASSERT_FALSE(stroke.Empty());
    ASSERT_TRUE(voxels->HasChunk({ 1, 0, 0 }));
    EXPECT_EQ(voxels->GetVoxelMaterial({ 32, 0, 0 }), 6);

    stroke.ApplyBefore(*voxels);
    EXPECT_FALSE(voxels->HasChunk({ 1, 0, 0 }));
    EXPECT_FLOAT_EQ(voxels->GetVoxelSDF({ 31, 0, 0 }), -1.0f);
    EXPECT_EQ(voxels->GetVoxelMaterial({ 31, 0, 0 }), 1);

    stroke.ApplyAfter(*voxels);
    EXPECT_TRUE(voxels->HasChunk({ 1, 0, 0 }));
    EXPECT_EQ(voxels->GetVoxelMaterial({ 32, 0, 0 }), 6);
}

TEST(VoxelEdit, BoundaryWriteMarksOnlyOwningAndExistingNeighbourChunksDirty)
{
    auto voxels = MakeVoxels();
    voxels->GetOrCreateChunk({ 0, 0, 0 });
    voxels->GetOrCreateChunk({ 1, 0, 0 });
    auto& left = voxels->GetChunks().at({ 0, 0, 0 });
    auto& right = voxels->GetChunks().at({ 1, 0, 0 });
    left.Dirty = false;
    right.Dirty = false;

    voxels->SetVoxel({ 31, 1, 1 }, -1.0f);

    EXPECT_TRUE(left.Dirty);
    EXPECT_TRUE(right.Dirty);
    EXPECT_EQ(voxels->GetChunkCount(), 2u);
}

TEST(VoxelEdit, PaintedMaterialSurvivesVoxelSerialization)
{
    auto voxels = MakeVoxels();
    voxels->SetVoxel({ -1, 4, 2 }, -1.0f, 23);

    auto restored = MakeVoxels();
    ASSERT_TRUE(restored->DeserializeRLE(voxels->SerializeRLE()));
    EXPECT_FLOAT_EQ(restored->GetVoxelSDF({ -1, 4, 2 }), -1.0f);
    EXPECT_EQ(restored->GetVoxelMaterial({ -1, 4, 2 }), 23);
}

TEST(VoxelEdit, DeserializeRejectsAWrongVersionOrAHeaderlessBlob)
{
    auto voxels = MakeVoxels();
    voxels->SetVoxel({ 3, 4, 5 }, -1.0f, 7);
    const TArray<u8> blob = voxels->SerializeRLE();
    ASSERT_GE(static_cast<sizet>(blob.Num()), sizet{ 12 });

    auto restored = MakeVoxels();
    ASSERT_TRUE(restored->DeserializeRLE(blob));
    const u32 chunksBefore = restored->GetChunkCount();

    // Version word bumped past the current one: rejected, state untouched.
    TArray<u8> wrongVersion = blob;
    const i32 nextVersion = VoxelOverride::RLEVersion + 1;
    std::memcpy(wrongVersion.GetData() + 4, &nextVersion, sizeof(nextVersion));
    EXPECT_FALSE(restored->DeserializeRLE(wrongVersion));

    // The pre-VOX1 layout started directly with the chunk count; it is no longer read.
    TArray<u8> headerless;
    headerless.Append(blob.GetData() + 8, blob.Num() - 8);
    EXPECT_FALSE(restored->DeserializeRLE(headerless));

    EXPECT_EQ(restored->GetChunkCount(), chunksBefore);
    EXPECT_FLOAT_EQ(restored->GetVoxelSDF({ 3, 4, 5 }), -1.0f);
}
