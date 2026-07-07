// OLO_TEST_LAYER: integration
// =============================================================================
// AnimatedModelDedupTest — issue #525 cheap-wins slice.
//
// Pins the SceneSerializer::DeserializeEntityComponents dedup cache: when a
// scene has multiple entities whose AnimationStateComponent shares the same
// resolved SourceFilePath (e.g. a crowd of the same rigged character), the
// deserializer must reuse the already-loaded Ref<AnimatedModel> instead of
// re-running Assimp import + allocating a fresh MeshSource/GPU-buffer graph
// per entity. Model/mesh data (MeshComponent::m_MeshSource,
// SkeletonComponent::m_Skeleton) must end up SHARED (same Ref target), while
// per-entity playback state (AnimationStateComponent scalars) must stay
// independent — that's the correctness subtlety the HANDOVER.md research
// flagged as needing verification, not just a speed measurement.
//
// Needs a live GL context: loading the real glTF exercises the full
// AnimatedModel -> MeshSource::Build() path (glCreateVertexArrays / GPU
// buffers), same as AssetSceneLoadTest / DeccerCubesLoadingTest. Skips
// cleanly on a headless box with no GPU.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include "Rendering/PropertyTests/RenderPropertyTest.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        Entity FindByTag(Scene& scene, const std::string& tag)
        {
            auto view = scene.GetAllEntitiesWith<TagComponent>();
            for (auto e : view)
            {
                if (view.get<TagComponent>(e).Tag == tag)
                    return Entity{ e, &scene };
            }
            return {};
        }

        // Points Project::GetAssetDirectory() at the real OloEditor/assets tree
        // (read-only — AnimatedModel::Create loads directly from disk, no asset
        // manager involved) by writing a throwaway .oloproj into a temp dir whose
        // AssetDirectory field is an ABSOLUTE path. ProjectSerializer keeps an
        // absolute AssetDirectory as-is (does not resolve it against the project
        // file's own directory), so Project::GetAssetDirectory() resolves to
        // exactly that path regardless of where the .oloproj itself lives.
        fs::path WriteThrowawayProjectPointingAt(const fs::path& assetDir, const fs::path& tempDir)
        {
            std::error_code ec;
            fs::create_directories(tempDir, ec);

            const fs::path projectFile = tempDir / "DedupTest.oloproj";
            std::ofstream out(projectFile);
            out << "Project:\n"
                << "  Name: DedupTest\n"
                << "  StartScene: \"dummy.olo\"\n"
                << "  AssetDirectory: \"" << assetDir.generic_string() << "\"\n"
                << "  ScriptModulePath: \"dummy.dll\"\n";
            out.close();
            return projectFile;
        }
    } // namespace

    TEST(AnimatedModelDedup, SharedSourcePathReusesModelButKeepsPlaybackStateIndependent)
    {
        // AnimatedModel -> MeshSource::Build() needs a live GL 4.6 context.
        OLO_ENSURE_GPU_OR_SKIP();

        if (!Renderer3D::IsInitialized())
        {
            Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
        }
        GLStateGuard glGuard("AnimatedModelDedup.Deserialize", GLStateGuard::Policy::Restore);

        const fs::path assetDir = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets";
        const fs::path modelPath = assetDir / "models" / "RiggedSimple" / "RiggedSimple.gltf";
        ASSERT_TRUE(fs::exists(modelPath))
            << "Fixture asset missing: " << modelPath.string();

        const fs::path tempDir = fs::temp_directory_path() / "OloEngineAnimatedModelDedupTest";
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        const fs::path projectFile = WriteThrowawayProjectPointingAt(assetDir, tempDir);
        struct Cleanup
        {
            fs::path Dir;
            ~Cleanup()
            {
                std::error_code cleanupEc;
                fs::remove_all(Dir, cleanupEc);
            }
        } cleanup{ tempDir };

        ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for throwaway test project.";
        ASSERT_EQ(Project::GetAssetDirectory(), assetDir)
            << "Absolute AssetDirectory must resolve verbatim, not relative to the project file.";

        // Two entities pointing at the SAME source model, with distinct
        // playback scalars so a state-sharing bug (rather than a model-sharing
        // win) would be visible after reload.
        constexpr const char* kTagA = "DedupCrowdMemberA";
        constexpr const char* kTagB = "DedupCrowdMemberB";
        std::string yaml;
        {
            auto scene = Scene::Create();

            Entity entityA = scene->CreateEntity(kTagA);
            auto& animA = entityA.AddComponent<AnimationStateComponent>();
            animA.m_SourceFilePath = modelPath.string();
            animA.m_CurrentTime = 0.25f;
            animA.m_IsPlaying = true;

            Entity entityB = scene->CreateEntity(kTagB);
            auto& animB = entityB.AddComponent<AnimationStateComponent>();
            animB.m_SourceFilePath = modelPath.string();
            animB.m_CurrentTime = 0.75f;
            animB.m_IsPlaying = false;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "Deserialize failed for the shared-source-path crowd scene.";

        Entity restoredA = FindByTag(*reloaded, kTagA);
        Entity restoredB = FindByTag(*reloaded, kTagB);
        ASSERT_TRUE(static_cast<bool>(restoredA)) << "Entity '" << kTagA << "' missing after reload.";
        ASSERT_TRUE(static_cast<bool>(restoredB)) << "Entity '" << kTagB << "' missing after reload.";

        // --- Model data is SHARED (the dedup win) ---
        ASSERT_TRUE(restoredA.HasComponent<MeshComponent>());
        ASSERT_TRUE(restoredB.HasComponent<MeshComponent>());
        EXPECT_EQ(restoredA.GetComponent<MeshComponent>().m_MeshSource,
                  restoredB.GetComponent<MeshComponent>().m_MeshSource)
            << "Two entities sharing a SourceFilePath must reuse the same MeshSource "
               "instead of each re-running Assimp import (issue #525 dedup cache).";

        ASSERT_TRUE(restoredA.HasComponent<SkeletonComponent>());
        ASSERT_TRUE(restoredB.HasComponent<SkeletonComponent>());
        EXPECT_EQ(restoredA.GetComponent<SkeletonComponent>().m_Skeleton,
                  restoredB.GetComponent<SkeletonComponent>().m_Skeleton)
            << "Two entities sharing a SourceFilePath must reuse the same Skeleton.";

        // --- Playback state stays INDEPENDENT per entity ---
        ASSERT_TRUE(restoredA.HasComponent<AnimationStateComponent>());
        ASSERT_TRUE(restoredB.HasComponent<AnimationStateComponent>());
        const auto& restoredAnimA = restoredA.GetComponent<AnimationStateComponent>();
        const auto& restoredAnimB = restoredB.GetComponent<AnimationStateComponent>();
        EXPECT_NEAR(restoredAnimA.m_CurrentTime, 0.25f, 1e-5f);
        EXPECT_NEAR(restoredAnimB.m_CurrentTime, 0.75f, 1e-5f);
        EXPECT_TRUE(restoredAnimA.m_IsPlaying);
        EXPECT_FALSE(restoredAnimB.m_IsPlaying);

        // Mutating one entity's playback state post-load must not leak into the
        // other, even though they share the underlying model/skeleton Refs.
        restoredA.GetComponent<AnimationStateComponent>().m_CurrentTime = 0.9f;
        EXPECT_NEAR(restoredB.GetComponent<AnimationStateComponent>().m_CurrentTime, 0.75f, 1e-5f)
            << "Entity B's playback state was corrupted by mutating entity A — "
               "the dedup cache must not alias per-entity AnimationStateComponents.";
    }
} // namespace OloEngine::Tests
