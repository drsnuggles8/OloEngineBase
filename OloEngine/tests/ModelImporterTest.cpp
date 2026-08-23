#include "OloEnginePCH.h"

// =============================================================================
// ModelImporterTest — unit test (headless, no GL).
//
// Pins OloEngine::ModelImporter, the single source of truth that wires a loaded
// model onto a scene entity. The same logic backs the editor's "Import Animated
// Model" button, viewport drag-drop, and SceneSerializer reload, so a regression
// here silently breaks all three (imported skeletal meshes stop animating, or
// re-imports clobber playback state / shader-graph materials).
//
// These tests drive the parts-based core (PopulateAnimatedEntityFromParts), so
// they need neither Assimp nor a render context — synthetic skeletons/clips from
// AnimationFixtures plus an empty MeshSource are enough to exercise the wiring.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"

#include "Functional/Helpers/AnimationFixtures.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

using namespace OloEngine;
namespace Fx = OloEngine::Functional::Fixtures;

namespace
{
    std::vector<Ref<AnimationClip>> MakeClips(int count)
    {
        std::vector<Ref<AnimationClip>> clips;
        clips.reserve(static_cast<sizet>(count));
        for (int i = 0; i < count; ++i)
        {
            clips.push_back(Fx::MakeTranslationClip(static_cast<f32>(i) + 1.0f));
        }
        return clips;
    }
} // namespace

// A model carrying a skeleton + clips gets the full animation component set, and
// fresh playback state (clip 0, Idle, stopped).
TEST(ModelImporterTest, AnimatedModelWiresAllComponents)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Character");

    auto mesh = Ref<MeshSource>::Create();
    auto skeleton = Fx::MakeSingleBoneSkeleton();
    auto clips = MakeClips(2);
    Material material;

    ModelImportResult result = ModelImporter::PopulateAnimatedEntityFromParts(
        entity, mesh, skeleton, clips, &material, "models/Character.gltf");

    EXPECT_TRUE(result.IsAnimated);
    EXPECT_TRUE(result.AddedMeshComponent);
    EXPECT_TRUE(result.AddedSkeletonComponent);
    EXPECT_TRUE(result.AddedAnimationStateComponent);
    EXPECT_TRUE(result.AddedMaterialComponent);
    EXPECT_TRUE(result.AddedAnyComponent());

    ASSERT_TRUE(entity.HasComponent<MeshComponent>());
    EXPECT_EQ(entity.GetComponent<MeshComponent>().m_MeshSource, mesh);

    ASSERT_TRUE(entity.HasComponent<SkeletonComponent>());
    EXPECT_EQ(entity.GetComponent<SkeletonComponent>().m_Skeleton, skeleton);

    ASSERT_TRUE(entity.HasComponent<AnimationStateComponent>());
    const auto& anim = entity.GetComponent<AnimationStateComponent>();
    EXPECT_EQ(anim.m_AvailableClips.size(), 2u);
    EXPECT_EQ(anim.m_CurrentClipIndex, 0);
    EXPECT_EQ(anim.m_CurrentClip, clips[0]);
    EXPECT_EQ(anim.m_SourceFilePath, std::string("models/Character.gltf"));
    EXPECT_EQ(anim.m_State, AnimationStateComponent::State::Idle);
    EXPECT_FALSE(anim.m_IsPlaying);

    EXPECT_TRUE(entity.HasComponent<MaterialComponent>());
}

// A model with no skeleton and no clips is purely static: only a MeshComponent.
TEST(ModelImporterTest, StaticModelOnlyAddsMeshComponent)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Prop");

    auto mesh = Ref<MeshSource>::Create();

    ModelImportResult result = ModelImporter::PopulateAnimatedEntityFromParts(
        entity, mesh, /*skeleton=*/nullptr, /*clips=*/{}, /*material=*/nullptr, "models/Prop.obj");

    EXPECT_FALSE(result.IsAnimated);
    EXPECT_TRUE(result.AddedMeshComponent);
    EXPECT_FALSE(result.AddedSkeletonComponent);
    EXPECT_FALSE(result.AddedAnimationStateComponent);
    EXPECT_FALSE(result.AddedMaterialComponent);

    EXPECT_TRUE(entity.HasComponent<MeshComponent>());
    EXPECT_FALSE(entity.HasComponent<SkeletonComponent>());
    EXPECT_FALSE(entity.HasComponent<AnimationStateComponent>());
    EXPECT_FALSE(entity.HasComponent<MaterialComponent>());
}

// The deserialize path (resetPlaybackState=false) must keep the playback scalars
// that were just read from YAML, only refreshing the heavy clip/skeleton refs.
TEST(ModelImporterTest, DeserializePreservesPlaybackScalars)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Saved");

    // Mimic SceneSerializer: AnimationStateComponent already exists with YAML scalars.
    auto& pre = entity.AddComponent<AnimationStateComponent>();
    pre.m_CurrentClipIndex = 1;
    pre.m_State = AnimationStateComponent::State::Custom;
    pre.m_CurrentTime = 0.75f;
    pre.m_IsPlaying = true;

    auto mesh = Ref<MeshSource>::Create();
    auto skeleton = Fx::MakeSingleBoneSkeleton();
    auto clips = MakeClips(3);

    ModelImportResult result = ModelImporter::PopulateAnimatedEntityFromParts(
        entity, mesh, skeleton, clips, nullptr, "models/Saved.gltf", /*resetPlaybackState=*/false);

    EXPECT_FALSE(result.AddedAnimationStateComponent); // it already existed

    const auto& anim = entity.GetComponent<AnimationStateComponent>();
    EXPECT_EQ(anim.m_AvailableClips.size(), 3u); // refreshed from the reloaded model
    EXPECT_EQ(anim.m_CurrentClipIndex, 1);       // preserved
    EXPECT_EQ(anim.m_CurrentClip, clips[1]);
    EXPECT_EQ(anim.m_State, AnimationStateComponent::State::Custom); // preserved
    EXPECT_NEAR(anim.m_CurrentTime, 0.75f, 1e-5f);                   // preserved
    EXPECT_TRUE(anim.m_IsPlaying);                                   // preserved
}

// An out-of-range deserialized clip index falls back to clip 0 (mirrors the old
// SceneSerializer behaviour) instead of indexing out of bounds.
TEST(ModelImporterTest, OutOfRangeClipIndexClampsToZero)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("BadIndex");

    auto& pre = entity.AddComponent<AnimationStateComponent>();
    pre.m_CurrentClipIndex = 99;

    auto mesh = Ref<MeshSource>::Create();
    auto clips = MakeClips(2);

    ModelImporter::PopulateAnimatedEntityFromParts(
        entity, mesh, Fx::MakeSingleBoneSkeleton(), clips, nullptr, "p.gltf", /*resetPlaybackState=*/false);

    const auto& anim = entity.GetComponent<AnimationStateComponent>();
    EXPECT_EQ(anim.m_CurrentClipIndex, 0);
    EXPECT_EQ(anim.m_CurrentClip, clips[0]);
}

// An entity whose material is driven by a shader graph must not have that material
// overwritten by the imported model's default material.
TEST(ModelImporterTest, ShaderGraphMaterialIsNotClobbered)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("GraphMat");

    auto& mat = entity.AddComponent<MaterialComponent>();
    mat.m_ShaderGraphHandle = AssetHandle(12345);
    mat.m_Material.SetBaseColorFactor(glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));

    Material modelMaterial;
    modelMaterial.SetBaseColorFactor(glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));

    ModelImportResult result = ModelImporter::PopulateAnimatedEntityFromParts(
        entity, Ref<MeshSource>::Create(), Fx::MakeSingleBoneSkeleton(), MakeClips(1), &modelMaterial, "p.gltf");

    EXPECT_FALSE(result.AddedMaterialComponent); // it already existed

    const glm::vec4 baseColor = entity.GetComponent<MaterialComponent>().m_Material.GetBaseColorFactor();
    EXPECT_NEAR(baseColor.r, 0.1f, 1e-5f); // shader-graph material left untouched
    EXPECT_NEAR(baseColor.g, 0.2f, 1e-5f);
    EXPECT_NEAR(baseColor.b, 0.3f, 1e-5f);
    EXPECT_EQ(static_cast<u64>(entity.GetComponent<MaterialComponent>().m_ShaderGraphHandle), 12345ull);
}

// Re-importing onto an already-wired entity updates the data but reports nothing
// as newly added (so the editor doesn't push spurious AddComponent undo steps).
TEST(ModelImporterTest, ReimportUpdatesDataWithoutReAddingComponents)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Reimport");

    Material material;
    ModelImportResult first = ModelImporter::PopulateAnimatedEntityFromParts(
        entity, Ref<MeshSource>::Create(), Fx::MakeSingleBoneSkeleton(), MakeClips(2), &material, "first.gltf");
    EXPECT_TRUE(first.AddedMeshComponent);
    EXPECT_TRUE(first.AddedSkeletonComponent);
    EXPECT_TRUE(first.AddedAnimationStateComponent);
    EXPECT_TRUE(first.AddedMaterialComponent);

    auto mesh2 = Ref<MeshSource>::Create();
    auto skeleton2 = Fx::MakeTwoBoneSkeleton();
    ModelImportResult second = ModelImporter::PopulateAnimatedEntityFromParts(
        entity, mesh2, skeleton2, MakeClips(1), &material, "second.gltf");

    EXPECT_FALSE(second.AddedMeshComponent);
    EXPECT_FALSE(second.AddedSkeletonComponent);
    EXPECT_FALSE(second.AddedAnimationStateComponent);
    EXPECT_FALSE(second.AddedMaterialComponent);

    EXPECT_EQ(entity.GetComponent<MeshComponent>().m_MeshSource, mesh2);
    EXPECT_EQ(entity.GetComponent<SkeletonComponent>().m_Skeleton, skeleton2);
    EXPECT_EQ(entity.GetComponent<AnimationStateComponent>().m_SourceFilePath, std::string("second.gltf"));
}

// The AnimatedModel overload returns an empty result for a null model rather than
// crashing (defensive: a failed load shouldn't take the import path down with it).
TEST(ModelImporterTest, NullAnimatedModelIsNoOp)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("NullModel");

    ModelImportResult result = ModelImporter::PopulateAnimatedEntity(entity, nullptr, "missing.gltf");

    EXPECT_FALSE(result.AddedAnyComponent());
    EXPECT_FALSE(entity.HasComponent<MeshComponent>());
    EXPECT_FALSE(entity.HasComponent<AnimationStateComponent>());
}

// =============================================================================
// Automatic LOD chain on import (issue #711)
//
// These drive the flag logic only — with no graphics device and no asset manager
// EnsureAutoLODGroup declines before generating anything, which is exactly the
// seam that matters here: what happens to an EXISTING group when a re-import
// replaces the mesh underneath it.
// =============================================================================

// A generated chain describes the mesh that produced it. A re-import replaces
// m_MeshSource on the same entity, so keeping the old chain would draw the
// PREVIOUS model at every level past LOD 0.
TEST(ModelImporterTest, ReimportDiscardsAStaleGeneratedLODGroup)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Prop");

    auto& lod = entity.AddComponent<LODGroupComponent>();
    lod.m_AutoGenerated = true;
    lod.m_LODGroup.Levels.emplace_back(AssetHandle(0x7001ull), 10.0f, 900u, 0.01f);

    auto meshSource = Ref<MeshSource>::Create();
    ModelImporter::PopulateAnimatedEntityFromParts(entity, meshSource, nullptr, {}, nullptr, "prop.gltf");

    EXPECT_FALSE(entity.HasComponent<LODGroupComponent>())
        << "a generated chain for the previous mesh survived the re-import";
}

// The same, through the path that skips generation. A static model re-imported as
// an ANIMATED one takes the "no chain for skinned meshes" branch — the discard has
// to happen before that gate, or the stale chain outlives it forever.
TEST(ModelImporterTest, StaticToAnimatedReimportDiscardsTheGeneratedLODGroup)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Character");

    auto& lod = entity.AddComponent<LODGroupComponent>();
    lod.m_AutoGenerated = true;
    lod.m_LODGroup.Levels.emplace_back(AssetHandle(0x7002ull), 10.0f, 900u, 0.01f);

    auto meshSource = Ref<MeshSource>::Create();
    auto skeleton = Fx::MakeSingleBoneSkeleton();
    ModelImporter::PopulateAnimatedEntityFromParts(entity, meshSource, skeleton, MakeClips(1), nullptr,
                                                   "character.gltf");

    EXPECT_FALSE(entity.HasComponent<LODGroupComponent>())
        << "a skinned re-import kept a generated chain that renders in bind pose past LOD 0";
}

// An AUTHORED chain is the user's data and must survive a re-import untouched —
// only generated ones are derived from the mesh being replaced.
TEST(ModelImporterTest, ReimportKeepsAnAuthoredLODGroup)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Prop");

    auto& lod = entity.AddComponent<LODGroupComponent>();
    lod.m_AutoGenerated = false; // authored
    lod.m_LODGroup.Bias = 2.5f;
    lod.m_LODGroup.Levels.emplace_back(AssetHandle(0x7003ull), 10.0f, 900u, 0.0f);

    auto meshSource = Ref<MeshSource>::Create();
    ModelImporter::PopulateAnimatedEntityFromParts(entity, meshSource, nullptr, {}, nullptr, "prop.gltf");

    ASSERT_TRUE(entity.HasComponent<LODGroupComponent>()) << "an authored chain was discarded";
    const auto& kept = entity.GetComponent<LODGroupComponent>();
    ASSERT_EQ(kept.m_LODGroup.Levels.size(), 1u);
    EXPECT_EQ(static_cast<u64>(kept.m_LODGroup.Levels[0].MeshHandle), 0x7003ull);
    EXPECT_FLOAT_EQ(kept.m_LODGroup.Bias, 2.5f);
}

// A DESERIALIZE (resetPlaybackState = false) must not touch the group at all: the
// scene file's own LODGroupComponent node owns that decision, and discarding here
// would race it.
TEST(ModelImporterTest, DeserializeLeavesTheGeneratedLODGroupAlone)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Prop");

    auto& lod = entity.AddComponent<LODGroupComponent>();
    lod.m_AutoGenerated = true;
    lod.m_LODGroup.Levels.emplace_back(AssetHandle(0x7004ull), 10.0f, 900u, 0.01f);

    auto meshSource = Ref<MeshSource>::Create();
    ModelImporter::PopulateAnimatedEntityFromParts(entity, meshSource, nullptr, {}, nullptr, "prop.gltf",
                                                   /*resetPlaybackState=*/false);

    ASSERT_TRUE(entity.HasComponent<LODGroupComponent>());
    EXPECT_EQ(entity.GetComponent<LODGroupComponent>().m_LODGroup.Levels.size(), 1u);
}
