// OLO_TEST_LAYER: L8
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <gtest/gtest.h>
#include <array>
#include <stb_image/stb_image_write.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <tuple>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    class ParallelRecordingSceneEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;
            EnableRendering(960, 540);
            m_TempDir = TempDir("parallel-recording-scene");
            fs::create_directories(m_TempDir / "Assets");
            const auto source = fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject/Assets/Scenes/Benchmark/ParallelRecording.olo";
            const auto scenePath = m_TempDir / "Assets/ParallelRecording.olo";
            fs::copy_file(source, scenePath, fs::copy_options::overwrite_existing);
            const auto projectPath = m_TempDir / "Recording.oloproj";
            {
                std::ofstream project(projectPath);
                project << "Project:\n  Name: Recording\n  StartScene: \"\"\n  AssetDirectory: Assets\n  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectPath));
            m_Assets = Ref<EditorAssetManager>::Create();
            m_Assets->Initialize(false);
            Project::SetAssetManager(m_Assets);
            SceneSerializer serializer(GetSceneRef());
            ASSERT_TRUE(serializer.Deserialize(scenePath));
            GetScene().SetGridVisible(false);
            GetScene().SetWorldAxisHelperVisible(false);
            GetScene().SetLightGizmosVisible(false);
            GetScene().SetCameraFrustumsVisible(false);
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::Deferred;
            settings.EnableDDGI = false;
            Renderer3D::GetPostProcessSettings().TAAEnabled = false;
            Renderer3D::ApplyRendererSettings();
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            if (m_Assets)
                m_Assets->Shutdown();
            m_Assets.Reset();
            Project::Unload();
            if (!m_TempDir.empty())
            {
                std::error_code error;
                fs::remove_all(m_TempDir, error);
            }
        }

        void Capture(std::string_view name, glm::vec3 position, f32 yaw, f32 pitch)
        {
            EditorCamera camera(60.0f, 960.0f / 540.0f, 0.05f, 300.0f);
            camera.SetViewportSize(960.0f, 540.0f);
            camera.SetPose(position, glm::radians(yaw), glm::radians(pitch));
            RunEditorFrames(camera, 3);
            std::vector<u8> pixels;
            u32 width = 0, height = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            ASSERT_EQ(width, 960u);
            ASSERT_EQ(height, 540u);
            const sizet stride = width * 4;
            for (u32 y = 0; y < height / 2; ++y)
                std::swap_ranges(pixels.begin() + y * stride, pixels.begin() + (y + 1) * stride,
                                 pixels.begin() + (height - 1 - y) * stride);
            const auto output = fs::path("assets/tests/visual") / ("ParallelRecording_" + std::string(name) + ".png");
            fs::create_directories(output.parent_path());
            ASSERT_NE(stbi_write_png(output.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                     4, pixels.data(), static_cast<int>(stride)),
                      0);
            sizet casterPixels = 0;
            sizet blackPixels = 0;
            for (sizet pixel = 0; pixel < pixels.size(); pixel += 4)
            {
                // The authored casters are blue; the floor and clear color
                // are neutral. A floor-only image must not pass as evidence.
                casterPixels += pixels[pixel + 2] > pixels[pixel] + 8 &&
                                pixels[pixel + 2] > pixels[pixel + 1] + 4;
                blackPixels += std::max({ pixels[pixel], pixels[pixel + 1], pixels[pixel + 2] }) <= 1;
            }
            const sizet pixelCount = static_cast<sizet>(width) * height;
            EXPECT_GT(casterPixels, pixelCount / 50) << name << ": expected visible blue caster geometry";
            EXPECT_LT(blackPixels, pixelCount / 20) << name << ": unexpected solid-black coverage";
        }

      private:
        fs::path m_TempDir;
        Ref<EditorAssetManager> m_Assets;
    };

    TEST_F(ParallelRecordingSceneEvidenceTest, ImportPreservesDistinctShadowKeysAndRendersMultipleViews)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto models = GetScene().GetAllEntitiesWith<ModelComponent>();
        u32 modelCount = 0;
        for (const auto entity : models)
        {
            const auto& model = models.get<ModelComponent>(entity).m_Model;
            ASSERT_TRUE(model);
            ASSERT_EQ(model->GetMeshCount(), 3600u);
            // The actual shadow batch key uses VAO + index range. Material
            // differences alone would not prevent automatic shadow batching.
            std::set<std::tuple<u32, u32, u32, u32>> shadowKeys;
            for (const auto& mesh : model->GetMeshes())
            {
                ASSERT_TRUE(mesh);
                ASSERT_TRUE(mesh->GetVertexArray());
                const auto vao = mesh->GetVertexArray()->GetRHIHandle();
                shadowKeys.emplace(vao.Index, vao.Generation, mesh->GetBaseIndex(), mesh->GetIndexCount());
            }
            EXPECT_EQ(shadowKeys.size(), 3600u);
            ++modelCount;
        }
        ASSERT_EQ(modelCount, 1u);
        ASSERT_NO_FATAL_FAILURE(Capture("overview", { 0, 50, 85 }, 0, 35));
        ASSERT_NO_FATAL_FAILURE(Capture("near", { 0, 9, 14 }, 0, 35));
        ASSERT_NO_FATAL_FAILURE(Capture("side", { 55, 24, 15 }, -70, 25));
    }
    class MeshParticleRecordingSceneEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(480, 320);
            GetScene().SetGridVisible(false);
            GetScene().SetWorldAxisHelperVisible(false);
            GetScene().SetLightGizmosVisible(false);
            GetScene().SetCameraFrustumsVisible(false);
        }
    };

    TEST_F(MeshParticleRecordingSceneEvidenceTest, ThreeRangesRemainVisibleFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto emitter = GetScene().CreateEntity("Mesh particles");
        auto& particles = emitter.AddComponent<ParticleSystemComponent>();
        particles.ParticleMesh = MeshPrimitives::CreateCube();
        ASSERT_TRUE(particles.ParticleMesh);
        auto& system = particles.System;
        system.RenderMode = ParticleRenderMode::Mesh;
        system.DepthSortEnabled = false;
        system.Emitter.RateOverTime = 0;
        system.GravityModule.Enabled = false;
        system.DragModule.Enabled = false;
        auto& pool = system.GetPool();
        ASSERT_EQ(pool.Emit(96), 96u);
        for (u32 i = 0; i < 96; ++i)
        {
            pool.m_Positions[i] = { -2.75f + static_cast<f32>(i % 12) * 0.5f,
                                    -0.75f + static_cast<f32>(i / 12) * 0.5f, 0.0f };
            pool.m_PrevPositions[i] = pool.m_Positions[i];
            pool.m_Colors[i] = i < 32 ? glm::vec4(1, 0, 0, 1) : (i < 64 ? glm::vec4(0, 1, 0, 1) : glm::vec4(0, 0, 1, 1));
            pool.m_InitialColors[i] = pool.m_Colors[i];
            pool.m_Sizes[i] = pool.m_PrevSizes[i] = pool.m_InitialSizes[i] = 0.3f;
            pool.m_Lifetimes[i] = pool.m_MaxLifetimes[i] = 100.0f;
        }
        system.Update(0.001f, glm::vec3(0.0f)); // publish real emitter bounds before scene culling
        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        for (const auto& pose : { Pose{ "front", { 0, 1, 8 }, 0, 0 },
                                  Pose{ "side", { 4, 2, 8 }, -25, 5 },
                                  Pose{ "above", { 0, 6, 8 }, 0, 32 } })
        {
            EditorCamera camera(60.0f, 1.5f, 0.1f, 100.0f);
            camera.SetPose(pose.Position, glm::radians(pose.Yaw), glm::radians(pose.Pitch));
            RunEditorFrames(camera, 2);
            std::vector<u8> pixels;
            u32 width = 0, height = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            const sizet stride = width * 4;
            for (u32 y = 0; y < height / 2; ++y)
                std::swap_ranges(pixels.begin() + y * stride, pixels.begin() + (y + 1) * stride,
                                 pixels.begin() + (height - 1 - y) * stride);
            const auto output = fs::path("assets/tests/visual") / ("ParallelMeshParticles_" + std::string(pose.Name) + ".png");
            fs::create_directories(output.parent_path());
            ASSERT_NE(stbi_write_png(output.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                     4, pixels.data(), static_cast<int>(stride)),
                      0);
            std::array<sizet, 3> colored{};
            for (sizet pixel = 0; pixel < pixels.size(); pixel += 4)
                for (u32 channel = 0; channel < 3; ++channel)
                    colored[channel] += pixels[pixel + channel] > 2u * pixels[pixel + (channel + 1) % 3] + 20u &&
                                        pixels[pixel + channel] > 2u * pixels[pixel + (channel + 2) % 3] + 20u;
            for (const auto count : colored)
                EXPECT_GT(count, 100u) << pose.Name << ": each mesh-particle range must be visible";
        }
    }
} // namespace OloEngine::Tests
