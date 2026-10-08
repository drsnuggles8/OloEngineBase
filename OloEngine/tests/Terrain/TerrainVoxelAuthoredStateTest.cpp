#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// TerrainVoxelAuthoredStateTest — unit test (headless, no GL).
//
// A terrain's voxel volume is either AUTHORED (carved, painted, imported:
// nothing can rebuild it) or AUTO-SEEDED (a copy of the height field the greedy
// mesher fills in, which it can rebuild at any time). Authored content used to
// be lost on every path the editor's own live session does not exercise:
//
//   * #1561 — TerrainComponent's copy constructor and assignment dropped the
//     volume, so delete -> undo and the Play-mode scene copy lost it;
//   * #1566 — SceneSerializer and the save game wrote only VoxelEnabled /
//     VoxelSize / VoxelMesher, so save -> reload lost it.
//
// The loss is silent: a greedy terrain re-seeds plausible geometry from the
// height field. So every check here compares the volume's exact VOX1 bytes,
// and the fixtures include an EDITED greedy volume, the case auto-seeding
// would otherwise hide. The negative controls at the end show those
// comparisons do fail on a dropped volume and on a re-seeded one.
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/ZlibSection.h"
#include "OloEngine/Terrain/Voxel/VoxelEdit.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "UndoRedo/ComponentCommands.h"
#include "UndoRedo/EditorCommand.h"
#include "UndoRedo/EntityCommands.h"
#include "UndoRedo/SpecializedCommands.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using namespace OloEngine;

namespace
{
    constexpr f32 kWorld = 4.0f;
    constexpr f32 kHeight = 8.0f;
    constexpr f32 kVoxelSize = 0.25f;

    [[nodiscard]] TArray<u8> Bytes(const Ref<VoxelOverride>& volume)
    {
        return volume ? volume->SerializeRLE() : TArray<u8>{};
    }

    // Exact authored content: same VOX1 bytes (SDF runs and materials), and
    // authored rather than a re-seed that happens to be non-empty.
    [[nodiscard]] ::testing::AssertionResult SameAuthoredVolume(const Ref<VoxelOverride>& actual,
                                                                const TArray<u8>& expected)
    {
        if (!actual)
            return ::testing::AssertionFailure() << "the voxel volume is gone";
        if (actual->IsAutoSeeded())
            return ::testing::AssertionFailure() << "the volume is an auto-seeded height-field copy, not the authored one";
        if (!(actual->SerializeRLE() == expected))
            return ::testing::AssertionFailure() << "the volume's chunks differ from the authored ones ("
                                                 << actual->GetChunkCount() << " chunks)";
        return ::testing::AssertionSuccess();
    }

    // A marching-cubes override: sparse carve-and-add content.
    void AuthorMarchingCubes(TerrainComponent& terrain)
    {
        terrain.m_VoxelEnabled = true;
        terrain.m_VoxelSize = kVoxelSize;
        terrain.m_VoxelMesher = VoxelMesherKind::MarchingCubes;
        terrain.m_VoxelOverride = Ref<VoxelOverride>::Create();
        terrain.m_VoxelOverride->Initialize(kWorld, kWorld, kHeight, kVoxelSize);
        terrain.m_VoxelOverride->AddSphere({ 2.0f, 5.0f, 2.0f }, 1.8f);
        terrain.m_VoxelOverride->CarveSphere({ 2.6f, 5.4f, 2.0f }, 0.7f);
    }

    // A greedy volume as SeedFromHeightmap leaves it: solid ground below a
    // flat surface with depth strata, flagged auto-seeded. Built cell by cell
    // because TerrainData uploads to the GPU and this file runs headless; the
    // real seeding path is driven by VoxelGreedyMeshVisualEvidenceTest.
    [[nodiscard]] Ref<VoxelOverride> SeededGreedyVolume()
    {
        auto volume = Ref<VoxelOverride>::Create();
        volume->Initialize(kWorld, kWorld, kHeight, kVoxelSize);
        for (i32 z = 0; z < 16; ++z)
            for (i32 y = 0; y < 13; ++y)
                for (i32 x = 0; x < 16; ++x)
                    volume->SetVoxel({ x, y, z }, static_cast<f32>(y) - 12.8f, y < 9 ? u8{ 0 } : y < 12 ? u8{ 1 } : u8{ 2 });
        volume->SetAutoSeeded(true);
        return volume;
    }

    void AuthorEditedGreedy(TerrainComponent& terrain)
    {
        terrain.m_VoxelEnabled = true;
        terrain.m_VoxelSize = kVoxelSize;
        terrain.m_VoxelMesher = VoxelMesherKind::GreedyCubic;
        terrain.m_VoxelOverride = SeededGreedyVolume();
        // A tunnel through the seeded ground plus a painted cell.
        for (i32 x = 2; x < 9; ++x)
            terrain.m_VoxelOverride->SetVoxel({ x, 10, 6 }, 1.0f, 0);
        terrain.m_VoxelOverride->SetVoxel({ 3, 4, 3 }, -1.0f, 5);
    }

    using Author = void (*)(TerrainComponent&);

    struct Mesher
    {
        const char* Name;
        Author Build;
    };

    constexpr Mesher kMeshers[] = { { "MarchingCubes", &AuthorMarchingCubes }, { "EditedGreedy", &AuthorEditedGreedy } };

    [[nodiscard]] Entity MakeTerrain(Scene& scene, Author author)
    {
        Entity entity = scene.CreateEntity("Terrain");
        auto& terrain = entity.AddComponent<TerrainComponent>();
        terrain.m_WorldSizeX = kWorld;
        terrain.m_WorldSizeZ = kWorld;
        terrain.m_HeightScale = kHeight;
        author(terrain);
        return entity;
    }

    [[nodiscard]] TerrainComponent& TerrainOf(Scene& scene, UUID uuid)
    {
        return scene.GetEntityByUUID(uuid).GetComponent<TerrainComponent>();
    }
} // namespace

// ── #1561: delete -> undo -> redo -> undo ─────────────────────────────────────

TEST(TerrainVoxelAuthoredState, DeleteUndoRestoresTheExactAuthoredVolumeRepeatedly)
{
    for (const auto& mesher : kMeshers)
    {
        SCOPED_TRACE(mesher.Name);
        auto scene = Scene::Create();
        const Entity entity = MakeTerrain(*scene, mesher.Build);
        const UUID uuid = entity.GetUUID();
        const TArray<u8> authored = Bytes(entity.GetComponent<TerrainComponent>().m_VoxelOverride);
        ASSERT_FALSE(authored.IsEmpty());

        CommandHistory history;
        history.Execute(std::make_unique<DeleteEntityCommand>(scene, entity));
        for (int round = 0; round < 3; ++round)
        {
            SCOPED_TRACE(round);
            ASSERT_FALSE(scene->TryGetEntityWithUUID(uuid));
            history.Undo();
            ASSERT_TRUE(scene->TryGetEntityWithUUID(uuid));
            const auto& restored = TerrainOf(*scene, uuid);
            EXPECT_TRUE(SameAuthoredVolume(restored.m_VoxelOverride, authored));
            EXPECT_TRUE(restored.m_VoxelRemeshAll) << "the restored terrain has no meshes and must rebuild them";
            history.Redo();
        }
    }
}

// The voxel brush's history names the volume object. After the terrain comes
// back from a delete, undoing an earlier stroke must still reach it.
TEST(TerrainVoxelAuthoredState, StrokeUndoStillReachesTheVolumeAfterDeleteUndo)
{
    auto scene = Scene::Create();
    const Entity entity = MakeTerrain(*scene, &AuthorMarchingCubes);
    const UUID uuid = entity.GetUUID();
    auto volume = entity.GetComponent<TerrainComponent>().m_VoxelOverride;
    const TArray<u8> beforeStroke = Bytes(volume);

    VoxelEditStroke stroke;
    stroke.BeforeAutoSeeded = volume->IsAutoSeeded();
    stroke.Before.emplace(VoxelCoord{ 0, 0, 0 }, volume->HasChunk({ 0, 0, 0 }) ? std::optional<VoxelChunk>(volume->GetChunks().at({ 0, 0, 0 })) : std::nullopt);
    volume->SetVoxel({ 1, 1, 1 }, -1.0f, 3);
    stroke.After.emplace(VoxelCoord{ 0, 0, 0 }, volume->GetChunks().at({ 0, 0, 0 }));

    CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<VoxelEditCommand>(volume, std::move(stroke)));
    history.Execute(std::make_unique<DeleteEntityCommand>(scene, entity));
    history.Undo(); // the delete
    history.Undo(); // the stroke

    EXPECT_TRUE(SameAuthoredVolume(TerrainOf(*scene, uuid).m_VoxelOverride, beforeStroke));
}

// ── #1561: Play-mode scene copy and duplicate ─────────────────────────────────

TEST(TerrainVoxelAuthoredState, SceneCopyIsolatesTheAuthoredVolumeBothWays)
{
    for (const auto& mesher : kMeshers)
    {
        SCOPED_TRACE(mesher.Name);
        auto edit = Scene::Create();
        const UUID uuid = MakeTerrain(*edit, mesher.Build).GetUUID();
        const TArray<u8> authored = Bytes(TerrainOf(*edit, uuid).m_VoxelOverride);

        auto play = Scene::Copy(edit);
        auto& playTerrain = TerrainOf(*play, uuid);
        auto& editTerrain = TerrainOf(*edit, uuid);
        ASSERT_TRUE(SameAuthoredVolume(playTerrain.m_VoxelOverride, authored)) << "Play mode lost the authored volume";
        ASSERT_NE(playTerrain.m_VoxelOverride, editTerrain.m_VoxelOverride) << "Play mode shares the edit scene's volume";

        playTerrain.m_VoxelOverride->SetVoxel({ 7, 7, 7 }, -1.0f, 2);
        EXPECT_TRUE(SameAuthoredVolume(editTerrain.m_VoxelOverride, authored)) << "a Play-mode edit reached the edit scene";

        const TArray<u8> playBytes = Bytes(playTerrain.m_VoxelOverride);
        editTerrain.m_VoxelOverride->SetVoxel({ 6, 6, 6 }, 1.0f, 0);
        EXPECT_TRUE(SameAuthoredVolume(playTerrain.m_VoxelOverride, playBytes)) << "an edit-scene write reached Play mode";
    }
}

TEST(TerrainVoxelAuthoredState, DuplicateEntityCarvesItsOwnVolume)
{
    auto scene = Scene::Create();
    const Entity original = MakeTerrain(*scene, &AuthorEditedGreedy);
    const TArray<u8> authored = Bytes(original.GetComponent<TerrainComponent>().m_VoxelOverride);

    Entity duplicate = scene->DuplicateEntity(original);
    auto& copy = duplicate.GetComponent<TerrainComponent>();
    ASSERT_TRUE(SameAuthoredVolume(copy.m_VoxelOverride, authored));
    copy.m_VoxelOverride->SetVoxel({ 0, 0, 0 }, 1.0f, 0);
    EXPECT_TRUE(SameAuthoredVolume(original.GetComponent<TerrainComponent>().m_VoxelOverride, authored));
}

// An inspector property undo assigns a whole snapshot back. It must not touch
// the voxel content, which has its own history.
TEST(TerrainVoxelAuthoredState, PropertyUndoKeepsTheVoxelContent)
{
    auto scene = Scene::Create();
    Entity entity = MakeTerrain(*scene, &AuthorMarchingCubes);
    auto& terrain = entity.GetComponent<TerrainComponent>();
    const TerrainComponent before = terrain;
    Ref<VoxelOverride> volume = terrain.m_VoxelOverride;
    volume->SetVoxel({ 2, 2, 2 }, -1.0f, 1); // a later voxel stroke
    terrain.m_HeightScale = 12.0f;
    const TArray<u8> current = Bytes(volume);

    CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<ComponentChangeCommand<TerrainComponent>>(scene, entity.GetUUID(), before, terrain));
    history.Undo();

    EXPECT_FLOAT_EQ(entity.GetComponent<TerrainComponent>().m_HeightScale, kHeight);
    EXPECT_EQ(entity.GetComponent<TerrainComponent>().m_VoxelOverride, volume);
    EXPECT_TRUE(SameAuthoredVolume(entity.GetComponent<TerrainComponent>().m_VoxelOverride, current));
}

// An inspector snapshot taken while the volume was still seeded must keep
// naming that volume: a later carve turns it authored in place, and undoing and
// redoing the earlier property change must not swap in a re-seeded copy that
// the carve's redo would then miss.
TEST(TerrainVoxelAuthoredState, PropertyUndoAcrossASeededToAuthoredCarveKeepsOneVolume)
{
    auto scene = Scene::Create();
    Entity entity = MakeTerrain(*scene, &AuthorEditedGreedy);
    const UUID uuid = entity.GetUUID();
    auto& terrain = entity.GetComponent<TerrainComponent>();
    terrain.m_VoxelOverride = SeededGreedyVolume();
    Ref<VoxelOverride> volume = terrain.m_VoxelOverride;

    CommandHistory history;
    const TerrainComponent before = terrain;
    terrain.m_CollisionEnabled = !terrain.m_CollisionEnabled;
    history.PushAlreadyExecuted(std::make_unique<ComponentChangeCommand<TerrainComponent>>(scene, uuid, before, terrain));

    const VoxelRayHit hit{ .Hit = true, .Voxel = { 4, 12, 4 } };
    VoxelEditStroke stroke = ApplyVoxelBrush(*volume, hit, { VoxelBrushOperation::Carve, 0.0f, 0 });
    ASSERT_FALSE(stroke.Empty());
    const TArray<u8> carved = Bytes(volume);
    history.PushAlreadyExecuted(std::make_unique<VoxelEditCommand>(volume, std::move(stroke)));

    history.Undo(); // carve
    history.Undo(); // property
    history.Redo(); // property
    history.Redo(); // carve

    EXPECT_EQ(TerrainOf(*scene, uuid).m_VoxelOverride, volume) << "the property undo swapped the volume out";
    EXPECT_TRUE(SameAuthoredVolume(TerrainOf(*scene, uuid).m_VoxelOverride, carved)) << "the carve's redo was lost";
}

// ── Authored vs rebuildable ───────────────────────────────────────────────────

TEST(TerrainVoxelAuthoredState, AnEditTurnsASeededVolumeIntoAuthoredContentAndUndoTurnsItBack)
{
    TerrainComponent terrain;
    terrain.m_VoxelOverride = SeededGreedyVolume();
    ASSERT_TRUE(terrain.m_VoxelOverride->IsAutoSeeded());
    EXPECT_EQ(TerrainComponent(terrain).m_VoxelOverride, terrain.m_VoxelOverride) << "copies share the volume";
    {
        TerrainComponent separateWorld = terrain;
        separateWorld.DetachVoxelVolume();
        EXPECT_FALSE(separateWorld.m_VoxelOverride) << "a separate world re-seeds from its own height field";
    }

    const VoxelRayHit hit{ .Hit = true, .Voxel = { 4, 12, 4 } };
    const VoxelEditStroke stroke = ApplyVoxelBrush(*terrain.m_VoxelOverride, hit, { VoxelBrushOperation::Carve, 0.0f, 0 });
    ASSERT_FALSE(stroke.Empty());
    EXPECT_FALSE(terrain.m_VoxelOverride->IsAutoSeeded());
    {
        TerrainComponent separateWorld = terrain;
        separateWorld.DetachVoxelVolume();
        EXPECT_TRUE(SameAuthoredVolume(separateWorld.m_VoxelOverride, Bytes(terrain.m_VoxelOverride)))
            << "an edited volume is authored, and a separate world gets its own copy of it";
        EXPECT_NE(separateWorld.m_VoxelOverride, terrain.m_VoxelOverride);
    }

    stroke.ApplyBefore(*terrain.m_VoxelOverride);
    EXPECT_TRUE(terrain.m_VoxelOverride->IsAutoSeeded());
    stroke.ApplyAfter(*terrain.m_VoxelOverride);
    EXPECT_FALSE(terrain.m_VoxelOverride->IsAutoSeeded());
}

// ── Negative controls ─────────────────────────────────────────────────────────

// The pre-fix copy dropped the volume; a greedy terrain then re-seeded from
// its height field. The comparison above must call both of those a loss, or
// every test in this file could pass on the old behaviour.
TEST(TerrainVoxelAuthoredState, NegativeControl_TheComparisonCatchesADroppedOrReseededVolume)
{
    TerrainComponent edited;
    AuthorEditedGreedy(edited);
    const TArray<u8> authored = Bytes(edited.m_VoxelOverride);

    EXPECT_FALSE(SameAuthoredVolume(nullptr, authored)) << "a dropped volume must fail";
    EXPECT_FALSE(SameAuthoredVolume(SeededGreedyVolume(), authored)) << "a re-seeded volume must fail";

    auto reseededAndMarked = SeededGreedyVolume();
    reseededAndMarked->SetAutoSeeded(false);
    EXPECT_FALSE(SameAuthoredVolume(reseededAndMarked, authored)) << "the bytes alone must tell the edit was lost";
}
