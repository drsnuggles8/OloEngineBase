// OLO_TEST_LAYER: L8
// Held views of the keeper on the actual Stacks Light stair. The tower, mesh LODs,
// material patches, face, eye attachments, groom and binding all come from Drift.
// The weather director and boat are exercised in Play; these pairs keep lighting
// fixed so a wave crest or an advancing sun cannot masquerade as a visible beard.
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "TestTempDir.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#ifdef OLO_WITH_ALEMBIC
#include "OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h"
#endif
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBindingCooker.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomMeshSurface.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>
#include <stb_image/stb_image_write.h>
#include <yaml-cpp/yaml.h>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        const fs::path kSandbox = fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject";
        const fs::path kModels = fs::path("Assets") / "Models" / "DriftKeeper";
        constexpr u32 kWidth = 960, kHeight = 640;
        constexpr char kKeeper[] = "Stacks Light Keeper";

        struct Pose
        {
            const char* Name;
            glm::vec3 Target;
            glm::vec3 Direction; // keeper-local direction toward the camera
            f32 Distance;
            f32 Fov;
            i32 Level;
        };

        const Pose kFront{ "Front", { 0.0f, 1.622f, 0.055f }, { 0.0f, 0.015f, 1.0f }, 0.59f, 29.0f, 0 };
        const Pose kQuarter{ "Quarter", { 0.0f, 1.622f, 0.045f }, { 0.68f, 0.06f, 1.0f }, 0.59f, 29.0f, 0 };
        const Pose kProfile{ "Profile", { 0.0f, 1.622f, 0.0f }, { 1.0f, 0.03f, 0.02f }, 0.60f, 29.0f, 0 };
        const Pose kHigh{ "High17deg", { 0.0f, 1.622f, 0.045f }, { 0.40f, 0.33f, 1.0f }, 0.62f, 29.0f, 0 };
        // Long views look up from the approach. Thirty metres is slant distance
        // to the figure. At 150 m the camera is at the boat's eye height.
        const Pose kMiddle{ "30m", { 0.0f, 0.91f, 0.0f }, { 0.28f, -0.52f, 1.0f }, 30.0f, 32.0f, 1 };
        const Pose kFar{ "150m", { 0.0f, 0.91f, 0.0f }, { 0.28f, -0.205f, 1.0f }, 150.0f, 32.0f, 2 };

        struct Delta
        {
            u32 Changed = 0;
            i32 Peak = 0;
        };

        // Only count pixels whose keeper difference clears the repeat-control
        // noise at the same pixel. The material and groom passes can jitter.
        Delta Compare(const std::vector<u8>& on, const std::vector<u8>& off, const std::vector<u8>& repeat)
        {
            Delta result;
            for (sizet i = 0; i < on.size(); i += 4)
            {
                i32 signal = 0, noise = 0;
                for (sizet c = 0; c < 3; ++c)
                {
                    signal = std::max(signal, std::abs(on[i + c] - off[i + c]));
                    noise = std::max(noise, std::abs(repeat[i + c] - off[i + c]));
                }
                result.Changed += signal >= 4 && signal > 2 * noise ? 1u : 0u;
                result.Peak = std::max(result.Peak, signal);
            }
            return result;
        }
        // Mark descendants by parent UUID, so additions to the keeper assembly
        // travel into the photograph without having to duplicate the rig here.
        YAML::Node ReviewEntities(const YAML::Node& source)
        {
            YAML::Node result(YAML::NodeType::Sequence);
            std::unordered_set<u64> keeperIds;
            for (const auto& e : source)
                if (e["TagComponent"]["Tag"].as<std::string>() == kKeeper)
                    keeperIds.insert(e["Entity"].as<u64>());
            sizet count;
            do
            {
                count = keeperIds.size();
                for (const auto& e : source)
                    if (const auto rel = e["RelationshipComponent"]; rel && keeperIds.contains(rel["ParentHandle"].as<u64>(0)))
                        keeperIds.insert(e["Entity"].as<u64>());
            } while (count != keeperIds.size());
            for (const auto& e : source)
            {
                const std::string name = e["TagComponent"]["Tag"].as<std::string>();
                if (name == "Stacks Light" || name == "Sun" || keeperIds.contains(e["Entity"].as<u64>()))
                    result.push_back(e);
            }
            return result;
        }
    } // namespace

    class DriftKeeperAuthoring : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(DriftKeeperAuthoring, CooksTheGroomAndBinding)
    {
        const char* destination = std::getenv("OLO_KEEPER_COOK_DIR");
        if (!destination || destination[0] == '\0')
            GTEST_SKIP() << "Set OLO_KEEPER_COOK_DIR to an empty output directory to regenerate the shipped groom";
#ifdef OLO_WITH_ALEMBIC
        const fs::path output(destination);
        ASSERT_TRUE(output.is_absolute());
        fs::create_directories(output);
        const fs::path coat = output / "Keeper.ologroom";
        const fs::path binding = output / "Keeper.ologroombinding";
        ASSERT_FALSE(fs::exists(coat));
        ASSERT_FALSE(fs::exists(binding));
        AlembicGroomImporter::Options options;
        options.ProvenancePath = (kModels / "Keeper.abc").generic_string();
        options.BuildCardLod = true;
        const auto imported = AlembicGroomImporter::Import(kSandbox / options.ProvenancePath, options);
        ASSERT_TRUE(imported.Succeeded()) << imported.Diagnostic;
        ASSERT_TRUE(imported.Warnings.empty()) << ::testing::PrintToString(imported.Warnings);
        ASSERT_TRUE(imported.AuthoredCards);
        auto model = Ref<AnimatedModel>::Create((kSandbox / kModels / "KeeperHairFoundation.gltf").string());
        const auto surface = model->GetEntityMeshSource();
        ASSERT_TRUE(surface && surface->HasMorphTargets());
        GroomBindingBuildStats stats;
        Ref<GroomBindingAsset> bound;
        std::vector<u8> coatBytes, bindingBytes;
        std::string error;
        ASSERT_TRUE(GroomCooker::CookToBytes(*imported.Groom, coatBytes, error)) << error;
        ASSERT_TRUE(GroomBindingCooker::CookPair(*imported.Groom, MakeGroomSurfaceView(*surface, model->GetSkeleton().Raw()),
                                                 (kModels / "KeeperHairFoundation.gltf").generic_string(), {}, bindingBytes, bound, stats, error))
            << error;
        ASSERT_EQ(stats.RootsBound, imported.Groom->GetCurveCount());
        EXPECT_EQ(stats.RootsDistant, 0u);
        for (const auto& [path, bytes] : { std::pair{ coat, &coatBytes }, std::pair{ binding, &bindingBytes } })
        {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            file.close();
            ASSERT_TRUE(file) << path.string();
        }
        GTEST_LOG_(INFO) << "Keeper groom curves=" << stats.RootsBound << " guides=" << imported.Groom->GetGuideCount()
                         << " mean/max rest distance (m)=" << stats.MeanRestDistance << "/" << stats.MaxRestDistance;
#else
        FAIL() << "Regenerating the keeper groom requires OLO_WITH_ALEMBIC";
#endif
    }

    class DriftKeeperEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Body, m_Coat;
        glm::mat4 m_Placement{ 1.0f };
        Ref<Project> m_Previous;
        Ref<AssetManagerBase> m_Manager;
        bool m_Saved = false;

        bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            m_Previous = Project::GetActive();
            m_Manager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            m_Saved = true;
            // Initialize(false) still scans the tree and serializes the registry.
            // Keep stable scene handles but give each case its own registry and
            // mesh caches, including when ctest runs the cells concurrently.
            const fs::path scratch = TempDir();
            fs::copy_file(kSandbox / "AssetRegistry.oar", scratch / "AssetRegistry.oar");
            for (const fs::path& folder : { kModels, fs::path("Assets/Grooms/DriftKeeper"), fs::path("Assets/Models/DriftLighthouse") })
            {
                for (const auto& entry : fs::recursive_directory_iterator(kSandbox / folder))
                {
                    if (!entry.is_regular_file())
                        continue;
                    const auto ext = entry.path().extension();
                    if (ext == ".abc" || ext == ".py" || ext == ".zip" || ext == ".md")
                        continue;
                    const auto target = scratch / entry.path().lexically_relative(kSandbox);
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target);
                }
            }
            ProjectConfig config;
            config.AssetDirectory = "Assets";
            ASSERT_TRUE(Project::NewInMemory(scratch, config));
            auto assets = Ref<EditorAssetManager>::Create();
            assets->Initialize(false);
            Project::SetAssetManager(assets);
            EnableRendering(kWidth, kHeight);
            const YAML::Node source = YAML::LoadFile((kSandbox / "Assets/Scenes/Drift.olo").string());
            const auto entities = ReviewEntities(source["Entities"]);
            RunInFrame([&]()
                       { EXPECT_EQ(SceneSerializer(GetSceneRef()).DeserializeAdditive(entities).Num(), static_cast<i32>(entities.size())); });
            m_Body = GetScene().FindEntityByName(kKeeper);
            m_Coat = GetScene().FindEntityByName("Keeper Groom");
            ASSERT_TRUE(m_Body && m_Coat && m_Body.GetParent());
            ASSERT_TRUE(m_Body.HasComponent<SkeletonComponent>() && m_Body.HasComponent<LODGroupComponent>() && m_Body.HasComponent<AnimationStateComponent>());
            ASSERT_TRUE(m_Coat.HasComponent<GroomComponent>() && m_Coat.HasComponent<GroomBindingComponent>());
            ASSERT_TRUE(m_Coat.GetComponent<GroomComponent>().m_RenderStrands);
            ASSERT_TRUE(GetScene().FindEntityByName("Keeper Eye L") && GetScene().FindEntityByName("Keeper Eye R"));
            m_Placement = m_Body.GetParent().GetTransform().GetTransform() * m_Body.GetTransform().GetTransform();
            m_Body.GetComponent<AnimationStateComponent>().m_IsPlaying = false;
            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.ForwardPlusAutoSwitch = false;
            renderer.EditorDebugDrawsEnabled = false;
            renderer.ShowGrid = false;
            renderer.ShowWorldAxisHelper = false;
            renderer.ShowComponentGizmos = false;
            Renderer3D::GetPostProcessSettings() = PostProcessSettings{};
            Time::SetMockTime(12.0f);
        }

        void TearDown() override
        {
            Time::ClearMockTime();
            RendererAttachedTest::TearDown();
            if (m_Saved)
                Project::RestoreActive(m_Previous, m_Manager);
        }

        void Capture(const EditorCamera& camera, const std::string& stem, std::vector<u8>& pixels)
        {
            Renderer3D::ResetFrameSequences();
            Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::SceneReset);
            RunEditorFrames(camera, 8, 0.0f);
            ASSERT_FALSE(HasFatalFailure());
            if (Renderer3D::GetPostProcessSettings().Technique == UpscalerTechnique::Temporal &&
                Renderer3D::GetPostProcessSettings().Upscale != UpscaleMode::Off)
                ASSERT_TRUE(Renderer3D::IsTemporalUpscaleActive()) << "the renderer fell back from temporal upscale";
            u32 width = 0, height = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
            ASSERT_EQ(pixels.size(), static_cast<sizet>(width) * height * 4u);
            if (stem.empty())
                return;
            const auto path = fs::path(OLO_TEST_EDITOR_ROOT) / "assets/tests/visual" / (stem + ".png");
            fs::create_directories(path.parent_path());
            // ReadbackComposite uses lower-left origin on both backends.
            const i32 stride = static_cast<i32>(width * 4u);
            ASSERT_NE(stbi_write_png(path.string().c_str(), static_cast<i32>(width), static_cast<i32>(height), 4,
                                     pixels.data() + static_cast<sizet>(height - 1u) * stride, -stride),
                      0);
        }

        void Measure(const std::string& mode, const Pose& pose)
        {
            SCOPED_TRACE(mode + "_" + pose.Name);
            const auto target = glm::vec3(m_Placement * glm::vec4(pose.Target, 1.0f));
            const glm::vec3 dir = glm::normalize(glm::mat3(m_Placement) * pose.Direction);
            const auto eye = target + dir * pose.Distance;
            EditorCamera camera(pose.Fov, static_cast<f32>(kWidth) / kHeight, 0.015f, 400.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, -std::atan2(dir.x, dir.z), std::asin(dir.y));
            const std::string suffix = mode + "_" + pose.Name;
            const std::string name = TestRenderBackend::EvidenceName("DriftKeeper") + suffix;
            std::vector<u8> on, off, repeat;
            Capture(camera, name, on);
            ASSERT_FALSE(HasFatalFailure());
            EXPECT_EQ(m_Body.GetComponent<LODGroupComponent>().m_ActiveAnimatedLOD, pose.Level) << suffix;
            if (pose.Level == 0)
            {
                auto& coat = m_Coat.GetComponent<GroomComponent>();
                coat.m_RenderStrands = false;
                Capture(camera, TestRenderBackend::EvidenceName("DriftKeeperGroomOff") + suffix, off);
                Capture(camera, {}, repeat);
                coat.m_RenderStrands = true;
            }
            else
            {
                auto& transform = m_Body.GetComponent<TransformComponent>();
                const auto original = transform.Translation;
                transform.Translation.x += 1000.0f;
                Capture(camera, TestRenderBackend::EvidenceName("DriftKeeperOff") + suffix, off);
                Capture(camera, {}, repeat);
                transform.Translation = original;
            }
            ASSERT_FALSE(HasFatalFailure());
            const auto delta = Compare(on, off, repeat);
            GTEST_LOG_(INFO) << name << ".png " << (pose.Level == 0 ? "groom" : "character")
                             << " changed px (>=4/255 and >2x repeat noise)=" << delta.Changed << " peak=" << delta.Peak
                             << " LOD=" << pose.Level << " camera=(" << eye.x << "," << eye.y << "," << eye.z << ")";
            EXPECT_GE(delta.Changed, pose.Level == 0 ? 40u : 2u) << "the subject of the comparison is invisible";
        }

        static void SetPath(RenderingPath path, u32 samples)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            settings.Deferred.MSAASampleCount = samples;
            Renderer3D::ApplyRendererSettings();
        }

        void Native(RenderingPath path, const char* name)
        {
            SetPath(path, 1);
            Measure(name, kQuarter);
            ASSERT_FALSE(HasFatalFailure());
            Measure(name, kMiddle);
        }
    };

    TEST_F(DriftKeeperEvidenceTest, Forward)
    {
        Native(RenderingPath::Forward, "Forward");
    }
    TEST_F(DriftKeeperEvidenceTest, ForwardPlus)
    {
        Native(RenderingPath::ForwardPlus, "ForwardPlus");
    }
    TEST_F(DriftKeeperEvidenceTest, Deferred)
    {
        Native(RenderingPath::Deferred, "Deferred");
        for (const Pose& view : { kFront, kProfile, kHigh, kFar })
        {
            ASSERT_FALSE(HasFatalFailure());
            Measure("Deferred", view);
        }
    }

    TEST_F(DriftKeeperEvidenceTest, ResolvedHair)
    {
        const u32 msaa = std::min(4u, Renderer3D::GetMaxMSAASamples());
        if (msaa <= 1)
            GTEST_SKIP() << "The active renderer reports single-sample targets only";
        SetPath(RenderingPath::Deferred, msaa);
        ASSERT_EQ(Renderer3D::GetRendererSettings().Deferred.MSAASampleCount, msaa);
        Measure("Deferred_MSAA" + std::to_string(msaa), kQuarter);
    }

    TEST_F(DriftKeeperEvidenceTest, TemporallyUpscaledHair)
    {
        if (!TemporalUpscalerUsable())
            GTEST_SKIP() << "Temporal upscaler unavailable on this backend/build";
        auto& post = Renderer3D::GetPostProcessSettings();
        post.Upscale = UpscaleMode::Quality;
        post.Technique = UpscalerTechnique::Temporal;
        SetPath(RenderingPath::Deferred, 1);
        Measure("Deferred_TemporalQuality", kQuarter);
        EXPECT_TRUE(Renderer3D::IsTemporalUpscaleActive());
    }
} // namespace OloEngine::Tests
