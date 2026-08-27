// OLO_TEST_LAYER: integration
// =============================================================================
// VoxelEditRemeshTest.cpp — the render-visible half of voxel undo (issue #728).
//
// tests/Terrain/VoxelEditTest.cpp proves the SDF grid round-trips exactly
// through a stroke and its undo. It cannot prove the screen agrees: the mesh
// builder keeps its own chunk -> GPU mesh map, keyed by chunk coordinate, and a
// chunk that an undo deleted outright carries no dirty flag to drive a rebuild
// with — it simply stops existing. Nothing in the dirty-chunk path can ever
// mention it again, so without an explicit prune its quads keep drawing forever
// and the undone brush stroke stays visible while every CPU assertion passes.
//
// That is why this drives the REAL VoxelGreedyMeshBuilder end to end, GPU
// upload included, rather than inspecting the volume: the bug lives in the
// builder's map, not in the volume.
//
// SKIPs cleanly with no GL 4.6 context, so headless CI is a no-op.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Terrain/Voxel/VoxelEdit.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMeshBuilder.h"
#include "PropertyTests/RenderPropertyTest.h"

using namespace OloEngine;

TEST(VoxelEditRemesh, UndoingAStrokeThatCreatedAChunkAlsoDropsItsMesh)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto voxels = Ref<VoxelOverride>::Create();
    voxels->Initialize(256.0f, 256.0f, 64.0f, 1.0f);

    // One solid cell at the far edge of chunk (0,0,0) gives the ray something
    // to land on, and puts the placement brush within reach of chunk (1,0,0).
    voxels->SetVoxel({ 31, 0, 0 }, -1.0f, 1);

    VoxelGreedyMeshBuilder builder;
    builder.FlushPending(*voxels);
    ASSERT_TRUE(builder.GetMeshes().contains(VoxelCoord{ 0, 0, 0 }));
    ASSERT_FALSE(builder.GetMeshes().contains(VoxelCoord{ 1, 0, 0 }));

    const VoxelRayHit hit = RaycastVoxels(*voxels, Ray({ 29.5f, 0.5f, 0.5f }, { 1.0f, 0.0f, 0.0f }, 0.0f, 10.0f));
    ASSERT_TRUE(hit.Hit);

    // Radius 2 around the placement cell spills past x = 31 into chunk (1,0,0),
    // which the stroke therefore has to CREATE.
    const VoxelEditStroke stroke = ApplyVoxelBrush(*voxels, hit, { VoxelBrushOperation::Place, 2.0f, 6 });
    ASSERT_FALSE(stroke.Empty());
    ASSERT_TRUE(voxels->HasChunk({ 1, 0, 0 }));

    builder.FlushPending(*voxels);
    EXPECT_TRUE(builder.GetMeshes().contains(VoxelCoord{ 1, 0, 0 }))
        << "the stroke created chunk (1,0,0) but the builder never meshed it";

    stroke.ApplyBefore(*voxels);
    ASSERT_FALSE(voxels->HasChunk({ 1, 0, 0 }));

    builder.FlushPending(*voxels);
    EXPECT_FALSE(builder.GetMeshes().contains(VoxelCoord{ 1, 0, 0 }))
        << "undo deleted chunk (1,0,0) but its mesh is still being drawn";
    // The chunk the stroke only MODIFIED must survive the same undo — a prune
    // that keys off dirtiness rather than existence would take this one too.
    EXPECT_TRUE(builder.GetMeshes().contains(VoxelCoord{ 0, 0, 0 }));
}
