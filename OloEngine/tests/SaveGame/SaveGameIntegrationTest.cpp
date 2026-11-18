#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <filesystem>

using namespace OloEngine;

namespace
{
    // The scene loader sets the resource handle when resolving MeshSourceHandle.
    // This test needs the identity and reference, without allocating a GPU mesh.
    struct BoundMeshSource final : MeshSource
    {
        explicit BoundMeshSource(AssetHandle handle)
        {
            SetHandle(handle);
        }
    };
} // namespace

class SaveGameIntegrationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = Ref<Scene>::Create();
        m_TempPath = OloEngine::Tests::TempFile("save.olosave");
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    static u32 CountEntities(const Ref<Scene>& scene)
    {
        u32 count = 0;
        auto view = scene->GetAllEntitiesWith<IDComponent>();
        for ([[maybe_unused]] auto e : view)
        {
            ++count;
        }
        return count;
    }

    Ref<Scene> m_Scene;
    std::filesystem::path m_TempPath;
};

TEST_F(SaveGameIntegrationTest, CaptureAndRestoreEmptyScene)
{
    auto data = SaveGameSerializer::CaptureSceneState(*m_Scene);
    EXPECT_GT(data.size(), 0u);

    Ref<Scene> newScene = Ref<Scene>::Create();
    EXPECT_TRUE(SaveGameSerializer::RestoreSceneState(*newScene, data));
}

TEST_F(SaveGameIntegrationTest, ContinueRetainsTheLoadedGeometryForMatchingAuthoredEntities)
{
    auto tower = m_Scene->CreateEntity("Tower");
    const UUID towerID = tower.GetUUID();
    const auto model = Ref<Model>::Create();
    auto& savedModel = tower.AddComponent<ModelComponent>(model, "Assets/Models/Tower.gltf");
    savedModel.m_Visible = false;
    auto bulb = m_Scene->CreateEntity("Optic");
    const UUID bulbID = bulb.GetUUID();
    const auto sphere = Ref<MeshSource>::Create();
    auto& savedMesh = bulb.AddComponent<MeshComponent>(sphere);
    savedMesh.m_Primitive = MeshPrimitive::Sphere;

    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    savedModel.m_Visible = true;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    const auto restoredTower = m_Scene->GetEntityByUUID(towerID);
    EXPECT_EQ(restoredTower.GetComponent<ModelComponent>().m_Model, model);
    EXPECT_FALSE(restoredTower.GetComponent<ModelComponent>().m_Visible);
    EXPECT_EQ(m_Scene->GetEntityByUUID(bulbID).GetComponent<MeshComponent>().m_MeshSource, sphere);

    // A different asset can occupy the same authored UUID in a later scene.
    // Its loaded geometry must not be attached to the saved identity.
    m_Scene->GetEntityByUUID(towerID).GetComponent<ModelComponent>().m_FilePath = "Assets/Models/Other.gltf";
    m_Scene->GetEntityByUUID(bulbID).GetComponent<MeshComponent>().m_Primitive = MeshPrimitive::Cube;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    EXPECT_NE(m_Scene->GetEntityByUUID(towerID).GetComponent<ModelComponent>().m_Model, model);
    EXPECT_NE(m_Scene->GetEntityByUUID(bulbID).GetComponent<MeshComponent>().m_MeshSource, sphere);
}

TEST_F(SaveGameIntegrationTest, ContinuePreservesTheAuthoredMeshSourceHandle)
{
    auto hull = m_Scene->CreateEntity("Boat Hull");
    const UUID hullID = hull.GetUUID();
    const Ref<MeshSource> authored = Ref<BoundMeshSource>::Create(AssetHandle(1234));
    hull.AddComponent<MeshComponent>(authored).m_LightmapStatic = true;
    auto transient = m_Scene->CreateEntity("Unregistered scratch mesh");
    const UUID transientID = transient.GetUUID();
    transient.AddComponent<MeshComponent>(Ref<MeshSource>::Create());

    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    hull.GetComponent<MeshComponent>().m_LightmapStatic = false;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    const auto& restored = m_Scene->GetEntityByUUID(hullID).GetComponent<MeshComponent>();
    EXPECT_EQ(restored.m_MeshSource, authored);
    EXPECT_TRUE(restored.m_LightmapStatic);
    EXPECT_FALSE(m_Scene->GetEntityByUUID(transientID).GetComponent<MeshComponent>().m_MeshSource);

    m_Scene->GetEntityByUUID(hullID).GetComponent<MeshComponent>().m_Primitive = MeshPrimitive::Cube;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    const auto& differentBinding = m_Scene->GetEntityByUUID(hullID).GetComponent<MeshComponent>();
    EXPECT_EQ(differentBinding.m_Primitive, MeshPrimitive::None);
    EXPECT_NE(differentBinding.m_MeshSource, authored);
}

TEST_F(SaveGameIntegrationTest, ContinueRetainsTheMeshOfAMatchingMorphImport)
{
    auto face = m_Scene->CreateEntity("Face");
    const UUID faceID = face.GetUUID();
    auto source = Ref<MeshSource>::Create();
    auto targets = Ref<MorphTargetSet>::Create();
    ASSERT_TRUE(targets->AddTarget(MorphTarget("Smile", 3)));
    source->SetMorphTargets(targets);
    const auto imported = ModelImporter::PopulateAnimatedEntityFromParts(face, source, nullptr, {}, nullptr, "Assets/Models/./Face.gltf");
    ASSERT_TRUE(imported.IsAnimated);
    ASSERT_FALSE(face.HasComponent<SkeletonComponent>());
    ASSERT_EQ(static_cast<u64>(source->GetHandle()), 0u);
    face.GetComponent<MorphTargetComponent>().Weights["Smile"] = 0.75f;
    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);

    // Re-authoring the scene with an equivalent spelling still refers to Face.
    face.GetComponent<AnimationStateComponent>().m_SourceFilePath = "Assets/Models/Face.gltf";
    face.GetComponent<MorphTargetComponent>().Weights["Smile"] = 0.1f;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    auto restored = m_Scene->GetEntityByUUID(faceID);
    const auto& restoredSource = restored.GetComponent<MeshComponent>().m_MeshSource;
    ASSERT_EQ(restoredSource, source);
    EXPECT_EQ(restoredSource->GetMorphTargets(), targets);
    EXPECT_FLOAT_EQ(restored.GetComponent<MorphTargetComponent>().Weights.at("Smile"), 0.75f);

    const Ref<MeshSource> other = Ref<BoundMeshSource>::Create(AssetHandle(4321));
    restored.GetComponent<MeshComponent>().m_MeshSource = other;
    restored.GetComponent<AnimationStateComponent>().m_SourceFilePath = "Assets/Models/Different.gltf";
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    EXPECT_FALSE(m_Scene->GetEntityByUUID(faceID).GetComponent<MeshComponent>().m_MeshSource);
}

TEST_F(SaveGameIntegrationTest, ContinueSamplesEachRigAtItsSavedClockAndPreservesQueuedRequests)
{
    using namespace OloEngine::Functional::Fixtures;
    auto actor = m_Scene->CreateEntity("Walking");
    auto paused = m_Scene->CreateEntity("Paused");
    const UUID actorID = actor.GetUUID(), pausedID = paused.GetUUID();
    const auto mesh = Ref<MeshSource>::Create();
    auto rig = MakeSingleBoneSkeleton();
    std::array clips{ MakeTranslationClip(1.0f), MakeTranslationClip(2.0f, "Root", {}, { 4, 0, 0 }) };
    clips[0]->Name = "Idle";
    clips[1]->Name = "Walk";
    for (auto entity : { actor, paused })
        ModelImporter::PopulateAnimatedEntityFromParts(entity, mesh, rig, clips, nullptr, "Fox.gltf");
    auto& surface = actor.AddComponent<MaterialComponent>();
    surface.m_Material.SetRoughnessFactor(0.73f);
    surface.m_Material.SetAlphaMode(AlphaMode::Mask);
    surface.m_ShaderGraphHandle = AssetHandle(314);
    auto& walking = actor.GetComponent<AnimationStateComponent>();
    walking.m_CurrentClipIndex = 1;
    walking.m_CurrentTime = 0.5f;
    walking.m_PlaybackSpeed = 0.5f;
    walking.m_IsPlaying = true;
    walking.m_BoneEntityIds = { pausedID };
    auto& stationary = paused.GetComponent<AnimationStateComponent>();
    stationary.m_RequestedClip = "Walk";
    stationary.m_RequestedLoop = false;
    stationary.m_CurrentTime = 0.75f;
    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    walking.m_CurrentClipIndex = 0;
    surface.m_Material.SetRoughnessFactor(0.1f);
    stationary.m_IsPlaying = true;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    auto restored = m_Scene->GetEntityByUUID(actorID);
    const auto& restoredSurface = restored.GetComponent<MaterialComponent>();
    EXPECT_FLOAT_EQ(restoredSurface.m_Material.GetRoughnessFactor(), 0.73f);
    EXPECT_EQ(restoredSurface.m_Material.GetAlphaMode(), AlphaMode::Mask);
    EXPECT_EQ(static_cast<u64>(restoredSurface.m_ShaderGraphHandle), 314u);
    auto& state = restored.GetComponent<AnimationStateComponent>();
    auto pose = restored.GetComponent<SkeletonComponent>().m_Skeleton;
    ASSERT_TRUE(pose);
    EXPECT_NE(pose, rig);
    ASSERT_EQ(state.m_AvailableClips.size(), 2u);
    EXPECT_EQ(state.m_CurrentClip, clips[1]);
    EXPECT_EQ(state.m_BoneEntityIds, std::vector<UUID>{ pausedID });
    EXPECT_TRUE(state.m_IsPlaying);
    EXPECT_FLOAT_EQ(pose->m_FinalBoneMatrices[0][3].x, 1.0f);

    auto restoredPaused = m_Scene->GetEntityByUUID(pausedID);
    const auto pausedPose = restoredPaused.GetComponent<SkeletonComponent>().m_Skeleton;
    const auto& pausedState = restoredPaused.GetComponent<AnimationStateComponent>();
    ASSERT_TRUE(pausedPose);
    EXPECT_NE(pose, pausedPose);
    EXPECT_FALSE(pausedState.m_IsPlaying);
    EXPECT_EQ(pausedState.m_RequestedClip, "Walk");
    EXPECT_FALSE(pausedState.m_RequestedLoop);
    EXPECT_FLOAT_EQ(pausedPose->m_FinalBoneMatrices[0][3].y, 0.375f);

    m_Scene->SetRenderingEnabled(false);
    constexpr f32 tick = 1.0f / 60.0f;
    m_Scene->OnUpdateRuntime(Timestep{ tick });
    EXPECT_FLOAT_EQ(restored.GetComponent<AnimationStateComponent>().m_CurrentTime, 0.5f + tick * 0.5f);
    EXPECT_FLOAT_EQ(pose->m_FinalBoneMatrices[0][3].x, 1.0f + tick);
    EXPECT_FLOAT_EQ(pausedPose->m_FinalBoneMatrices[0][3].y, 0.375f);
}

TEST_F(SaveGameIntegrationTest, ContinueResumesTheSelectedBlendDestinationAndHandlesChangedClips)
{
    using namespace OloEngine::Functional::Fixtures;
    auto actor = m_Scene->CreateEntity("Blending");
    const UUID id = actor.GetUUID();
    std::array clips{ MakeTranslationClip(1.0f), MakeTranslationClip(2.0f) };
    clips[1]->Name = "Walk";
    ModelImporter::PopulateAnimatedEntityFromParts(actor, Ref<MeshSource>::Create(), MakeSingleBoneSkeleton(), clips, nullptr, "Fox.gltf");
    actor.AddComponent<MaterialComponent>().m_Material.SetAlphaMode(AlphaMode::Mask);
    auto& before = actor.GetComponent<AnimationStateComponent>();
    before.m_CurrentTime = 0.7f;
    before.m_RequestedClip = "Walk";
    before.m_RequestedLoop = false;
    Animation::AnimationSystem::Update(before, *actor.GetComponent<SkeletonComponent>().m_Skeleton, 0.125f);
    ASSERT_TRUE(before.m_Blending);
    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    auto state = [&]() -> AnimationStateComponent&
    { return m_Scene->GetEntityByUUID(id).GetComponent<AnimationStateComponent>(); };
    EXPECT_EQ(state().m_CurrentClip, clips[1]);
    EXPECT_FLOAT_EQ(state().m_CurrentTime, 0.125f);
    EXPECT_FALSE(state().m_Loop);
    EXPECT_FALSE(state().m_Blending);
    EXPECT_FALSE(state().m_NextClip);

    // V50 identifies a slot. A reordered catalogue selects the new occupant,
    // a removed slot falls back to zero, and an empty catalogue has no clip.
    state().m_AvailableClips = { clips[1], clips[0] };
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    EXPECT_EQ(state().m_CurrentClip, clips[0]);
    state().m_AvailableClips = { clips[1] };
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    EXPECT_EQ(state().m_CurrentClipIndex, 0);
    EXPECT_EQ(state().m_CurrentClip, clips[1]);
    state().m_AvailableClips.clear();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    EXPECT_FALSE(state().m_CurrentClip);
    EXPECT_TRUE(m_Scene->GetEntityByUUID(id).GetComponent<SkeletonComponent>().m_Skeleton);

    state().m_SourceFilePath = "Different.gltf";
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, payload));
    auto mismatch = m_Scene->GetEntityByUUID(id);
    EXPECT_FALSE(mismatch.GetComponent<MeshComponent>().m_MeshSource);
    EXPECT_FALSE(mismatch.GetComponent<SkeletonComponent>().m_Skeleton);
    EXPECT_EQ(mismatch.GetComponent<MaterialComponent>().m_Material.GetAlphaMode(), AlphaMode::Opaque);
    EXPECT_TRUE(state().m_AvailableClips.empty());
}

TEST_F(SaveGameIntegrationTest, ContinueRestoresALegacyBindPoseRigWithoutAddingPlayback)
{
    using namespace OloEngine::Functional::Fixtures;
    auto actor = m_Scene->CreateEntity("Bind pose");
    const UUID id = actor.GetUUID();
    const auto mesh = Ref<MeshSource>::Create();
    auto rig = MakeSingleBoneSkeleton();
    actor.AddComponent<MeshComponent>(mesh);
    actor.AddComponent<SkeletonComponent>(rig);
    const auto legacy = SaveGameSerializer::CaptureSceneState(*m_Scene);
    ModelImporter::PopulateAnimatedEntityFromParts(actor, mesh, rig, {}, nullptr, "Fox.gltf");
    ASSERT_EQ(actor.GetComponent<AnimationStateComponent>().m_SourceFilePath, "Fox.gltf");
    rig->m_LocalTransforms[0][3].x = 5.0f;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*m_Scene, legacy));
    const auto restored = m_Scene->GetEntityByUUID(id);
    EXPECT_EQ(restored.GetComponent<MeshComponent>().m_MeshSource, mesh);
    EXPECT_FALSE(restored.HasComponent<AnimationStateComponent>());
    auto pose = restored.GetComponent<SkeletonComponent>().m_Skeleton;
    ASSERT_TRUE(pose);
    EXPECT_FLOAT_EQ(pose->m_FinalBoneMatrices[0][3].x, 0.0f);
}

TEST_F(SaveGameIntegrationTest, CaptureAndRestoreWithEntities)
{
    // Create entities with various components
    Entity e1 = m_Scene->CreateEntity("Player");
    auto& transform = e1.GetComponent<TransformComponent>();
    transform.Translation = { 10.0f, 20.0f, 30.0f };
    transform.SetRotationEuler({ 0.1f, 0.2f, 0.3f });
    transform.Scale = { 2.0f, 2.0f, 2.0f };

    Entity e2 = m_Scene->CreateEntity("Camera");
    auto& cam = e2.AddComponent<CameraComponent>();
    cam.Primary = true;

    Entity e3 = m_Scene->CreateEntity("Sprite");
    auto& sprite = e3.AddComponent<SpriteRendererComponent>();
    sprite.Color = { 1.0f, 0.0f, 0.0f, 1.0f };
    sprite.TilingFactor = 2.5f;

    // Capture
    auto data = SaveGameSerializer::CaptureSceneState(*m_Scene);
    EXPECT_GT(data.size(), 0u);

    // Restore into fresh scene
    Ref<Scene> newScene = Ref<Scene>::Create();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*newScene, data));

    // Verify entity count
    EXPECT_EQ(CountEntities(newScene), 3u);

    // Verify component data by finding entities by tag
    bool foundPlayer = false;
    bool foundCamera = false;
    bool foundSprite = false;
    auto view = newScene->GetAllEntitiesWith<IDComponent>();
    for (auto e : view)
    {
        Entity entity = { e, newScene.get() };
        const auto& tag = entity.GetComponent<TagComponent>().Tag;

        if (tag == "Player")
        {
            foundPlayer = true;
            const auto& t = entity.GetComponent<TransformComponent>();
            EXPECT_NEAR(t.Translation.x, 10.0f, 0.001f);
            EXPECT_NEAR(t.Translation.y, 20.0f, 0.001f);
            EXPECT_NEAR(t.Translation.z, 30.0f, 0.001f);
            EXPECT_NEAR(t.GetRotationEuler().x, 0.1f, 0.001f);
            EXPECT_NEAR(t.GetRotationEuler().y, 0.2f, 0.001f);
            EXPECT_NEAR(t.GetRotationEuler().z, 0.3f, 0.001f);
            EXPECT_NEAR(t.Scale.x, 2.0f, 0.001f);
            EXPECT_NEAR(t.Scale.y, 2.0f, 0.001f);
            EXPECT_NEAR(t.Scale.z, 2.0f, 0.001f);
        }
        else if (tag == "Camera")
        {
            foundCamera = true;
            ASSERT_TRUE(entity.HasComponent<CameraComponent>());
            const auto& c = entity.GetComponent<CameraComponent>();
            EXPECT_TRUE(c.Primary);
        }
        else if (tag == "Sprite")
        {
            foundSprite = true;
            ASSERT_TRUE(entity.HasComponent<SpriteRendererComponent>());
            const auto& s = entity.GetComponent<SpriteRendererComponent>();
            EXPECT_NEAR(s.Color.r, 1.0f, 0.001f);
            EXPECT_NEAR(s.Color.g, 0.0f, 0.001f);
            EXPECT_NEAR(s.Color.b, 0.0f, 0.001f);
            EXPECT_NEAR(s.Color.a, 1.0f, 0.001f);
            EXPECT_NEAR(s.TilingFactor, 2.5f, 0.001f);
        }
        else
        {
            // No additional handling required.
        }
    }
    EXPECT_TRUE(foundPlayer);
    EXPECT_TRUE(foundCamera);
    EXPECT_TRUE(foundSprite);
}

// Skin diffusion became a scene setting in format 50 (issue #1689): a save
// carries it, and a restore replaces whatever the target scene held.
TEST_F(SaveGameIntegrationTest, SkinDiffusionSettingsSurviveCaptureAndRestore)
{
    SkinDiffusionSettings authored;
    authored.Enabled = false;
    authored.Quality = SkinDiffusionQuality::High;
    authored.DepthRejectionScale = 3.5f;
    authored.RadiusScale = 1.75f;
    ASSERT_NE(authored, SkinDiffusionSettings{});
    m_Scene->SetSkinDiffusionSettings(authored);

    const auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Ref<Scene>::Create();
    restored->GetSkinDiffusionSettings().RadiusScale = 0.25f;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));
    EXPECT_EQ(restored->GetSkinDiffusionSettings(), authored);

    // The play-mode copy carries it too (scene-copy-must-carry-scene-level-settings.md).
    EXPECT_EQ(Scene::Copy(m_Scene)->GetSkinDiffusionSettings(), authored);
}

TEST_F(SaveGameIntegrationTest, FullFileRoundTrip)
{
    // Create test entity
    Entity e = m_Scene->CreateEntity("TestEntity");
    auto& t = e.GetComponent<TransformComponent>();
    t.Translation = { 1.0f, 2.0f, 3.0f };

    // Capture scene state
    auto payload = SaveGameSerializer::CaptureSceneState(*m_Scene);
    ASSERT_GT(payload.size(), 0u);

    // Compress
    std::vector<u8> compressed;
    ASSERT_TRUE(SaveGameFile::Compress(payload, compressed));

    // Build metadata
    SaveGameMetadata meta;
    meta.DisplayName = "Integration Test";
    meta.SceneName = "TestScene";
    meta.TimestampUTC = 1700000000;
    meta.EntityCount = 1;

    // Build header
    SaveGameHeader header;
    header.EntityCount = 1;
    header.SetCompression(SaveGameCompression::Zlib);
    header.PayloadUncompressedSize = payload.size();

    // Write file
    ASSERT_TRUE(SaveGameFile::Write(m_TempPath, header, meta, {}, compressed));

    // Validate checksum
    EXPECT_TRUE(SaveGameFile::ValidateChecksum(m_TempPath));

    // Read metadata
    SaveGameHeader readHeader;
    SaveGameMetadata readMeta;
    ASSERT_TRUE(SaveGameFile::ReadMetadata(m_TempPath, readHeader, readMeta));
    EXPECT_EQ(readMeta.DisplayName, "Integration Test");
    EXPECT_EQ(readMeta.EntityCount, 1u);

    // Read and decompress payload
    std::vector<u8> readPayload;
    ASSERT_TRUE(SaveGameFile::ReadPayload(m_TempPath, readPayload));
    EXPECT_EQ(readPayload.size(), payload.size());
    EXPECT_EQ(readPayload, payload);

    // Restore into fresh scene
    Ref<Scene> newScene = Ref<Scene>::Create();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*newScene, readPayload));

    // Verify
    EXPECT_EQ(CountEntities(newScene), 1u);
}

TEST_F(SaveGameIntegrationTest, EmptyPayloadReturnsError)
{
    std::vector<u8> empty;
    Ref<Scene> newScene = Ref<Scene>::Create();
    EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*newScene, empty));
}

TEST_F(SaveGameIntegrationTest, CorruptedDataReturnsError)
{
    std::vector<u8> garbage = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 };
    Ref<Scene> newScene = Ref<Scene>::Create();
    EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*newScene, garbage));
}
