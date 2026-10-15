// OLO_TEST_LAYER: L8
// =============================================================================
// GroomSceneLightingEvidenceTest.cpp — a coat lit by the scene its body is lit
// by (issue #1618).
//
// Four questions, each answered on the real horse coat and its real bound body
// (GroomSoftIntersectionHorse.olo), on every render path:
//
//   1. FOG. Does the fog the body receives reach the coat? The fog pass reads
//      the scene depth after the coat has drawn; this captures that depth at
//      the fog pass and the fog fraction of every coat and body pixel.
//   2. DDGI. In a closed, red-walled interior, does the coat's ambient follow
//      the probe volume the way its body's does -- same chromatic direction --
//      rather than the open sky above the roof?
//   3. CLUSTERS. On Forward+ the strands walk their cluster's light list; the
//      coat must look the same as the whole-buffer walk with 32 local lights.
//   4. COST. GroomPass GPU ms with 1, 32 and 256 local lights (L6 perf rows).
//
// PNGs land in .v-build/1618/visual, named after the cell.
// =============================================================================
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        const std::filesystem::path kEditor{ OLO_TEST_EDITOR_ROOT };
        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr std::array kPaths{ std::pair{ RenderingPath::Forward, "Forward" },
                                     std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
                                     std::pair{ RenderingPath::Deferred, "Deferred" } };

        // Display (tonemap None, gamma 2.2) back to linear.
        f32 Linear(u8 value)
        {
            return std::pow(static_cast<f32>(value) / 255.0f, 2.2f);
        }

        struct RegionMean
        {
            glm::vec3 Linear{ 0.0f };
            u32 Pixels = 0;
        };

        RegionMean MeanOf(const std::vector<u8>& rgba, const std::vector<i32>& ids, i32 id)
        {
            RegionMean mean;
            for (sizet i = 0; i < ids.size(); ++i)
            {
                if (ids[i] != id)
                    continue;
                mean.Linear += glm::vec3(Linear(rgba[i * 4u]), Linear(rgba[i * 4u + 1u]), Linear(rgba[i * 4u + 2u]));
                ++mean.Pixels;
            }
            if (mean.Pixels > 0u)
                mean.Linear /= static_cast<f32>(mean.Pixels);
            return mean;
        }
    } // namespace

    class GroomSceneLightingEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Coat;
        Entity m_Body;
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssets;
        PostProcessSettings m_SavedPost;
        FogSettings m_SavedFog;
        bool m_StateCaptured = false;
        std::vector<i32> m_Ids;
        std::vector<f32> m_FogDepth;
        std::vector<f32> m_PreGroomDepth;

        void BuildScene() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            m_SavedPost = Renderer3D::GetPostProcessSettings();
            m_SavedFog = Renderer3D::GetFogSettings();
            m_StateCaptured = true;
            ASSERT_TRUE(Project::Load(kEditor / "SandboxProject/Sandbox.oloproj"));
            auto assets = Ref<EditorAssetManager>::Create();
            assets->Initialize(false);
            Project::SetAssetManager(assets);
            ASSERT_TRUE(SceneSerializer(GetSceneRef())
                            .Deserialize((kEditor / "SandboxProject/Assets/Scenes/Tests/GroomSoftIntersectionHorse.olo").string()));
            EnableRendering(kWidth, kHeight);
            m_Coat = GetScene().FindEntityByName("LongCoatHorseCoat");
            m_Body = GetScene().FindEntityByName("LongCoatHorse");
            ASSERT_TRUE(m_Coat && m_Body);
            // The water is a blended surface with its own fog-free refraction;
            // these questions are about the coat and the body, not the water.
            GetScene().FindEntityByName("ShallowWater").GetComponent<WaterComponent>().m_Enabled = false;
            auto& lod = m_Coat.GetComponent<GroomLodComponent>();
            lod.m_Enabled = false;
            lod.m_HoldFrames = 0;
            lod.m_VisibilitySteps = 0;
            lod.m_SimulationSteps = 0;
            lod.m_ShadowSteps = 0;
            auto& settings = Renderer3D::GetRendererSettings();
            settings.EditorDebugDrawsEnabled = false;
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowComponentGizmos = false;
            // Linear display: tonemap None at gamma 2.2, so a pixel can be taken
            // back to the linear radiance the fog composite mixed.
            auto& post = Renderer3D::GetPostProcessSettings();
            post = PostProcessSettings{};
            post.Tonemap = TonemapOperator::None;
            post.Exposure = 1.0f;
            post.Gamma = 2.2f;
            post.TAAEnabled = true;
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            if (!m_StateCaptured)
                return;
            Renderer3D::GetPostProcessSettings() = m_SavedPost;
            Renderer3D::GetFogSettings() = m_SavedFog;
            Project::Unload();
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            if (m_PreviousAssets)
                Project::SetAssetManager(m_PreviousAssets);
        }

        [[nodiscard]] i32 CoatId() const
        {
            return static_cast<i32>(static_cast<u32>(static_cast<entt::entity>(m_Coat)));
        }
        [[nodiscard]] i32 BodyId() const
        {
            return static_cast<i32>(static_cast<u32>(static_cast<entt::entity>(m_Body)));
        }

        void SetPath(RenderingPath path)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            settings.ForwardPlusAutoSwitch = false;
            Renderer3D::ApplyRendererSettings();
        }

        static void ReadTexture(std::string_view name, GLenum format, GLenum type, void* out, sizet bytes)
        {
            const auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            const u32 texture = graph->ResolveTexture(graph->GetTextureHandle(name));
            ASSERT_NE(texture, 0u) << name;
            glGetTextureImage(texture, 0, format, type, static_cast<GLsizei>(bytes), out);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << name;
        }

        // A converged frame from `eye` at `target`, with the entity ids the
        // coat drew (read right after GroomPass, before any later pass reuses
        // the target), the depth the fog pass reads, and the depth before any
        // coat drew.
        void Capture(const std::string& name, const glm::vec3& eye, const glm::vec3& target, u32 frames,
                     std::vector<u8>& pixels)
        {
            struct RestoreClock
            {
                ~RestoreClock()
                {
                    Time::ClearMockTime();
                }
            } restoreClock;
            Time::SetMockTime(12.0f);
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            const glm::vec3 delta = eye - target;
            camera.SetPose(eye, -std::atan2(delta.x, delta.z), std::asin(delta.y / glm::length(delta)));
            Renderer3D::ResetFrameSequences();
            RunEditorFrames(camera, frames, 0.0f);

            auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            struct RemoveHook
            {
                Ref<RenderGraph> Graph;
                ~RemoveHook()
                {
                    Graph->RemovePostPassHook("GroomSceneLightingEvidence");
                }
            } removeHook{ graph };
            const sizet count = static_cast<sizet>(kWidth) * kHeight;
            m_Ids.assign(count, -1);
            m_FogDepth.clear();
            m_PreGroomDepth.clear();
            graph->AddPostPassHook("GroomSceneLightingEvidence", [&](std::string_view pass, RenderGraph&)
                                   {
                if (pass == "GroomSceneDepthSnapshotPass")
                {
                    m_PreGroomDepth.resize(count);
                    ReadTexture("GroomSceneDepthSnapshot", GL_DEPTH_COMPONENT, GL_FLOAT, m_PreGroomDepth.data(), count * sizeof(f32));
                }
                else if (pass == "GroomPass")
                {
                    ReadTexture("SceneEntityID", GL_RED_INTEGER, GL_INT, m_Ids.data(), count * sizeof(i32));
                }
                else if (pass == "FogPass")
                {
                    m_FogDepth.resize(count);
                    ReadTexture("SceneDepthAttachment", GL_DEPTH_COMPONENT, GL_FLOAT, m_FogDepth.data(), count * sizeof(f32));
                } });
            RunEditorFrames(camera, 1, 0.0f);
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
            const auto pass = Renderer3D::GetGroomRenderPass();
            ASSERT_TRUE(pass);
            EXPECT_GT(pass->GetStats().GroomsDrawn, 0u);
            EXPECT_EQ(pass->GetStats().GroomsBindingRefused, 0u);
            if (!name.empty())
            {
                const auto directory = kEditor.parent_path() / ".v-build/1618/visual";
                std::filesystem::create_directories(directory);
                stbi_flip_vertically_on_write(1);
                EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(), static_cast<i32>(width),
                                         static_cast<i32>(height), 4, pixels.data(), static_cast<i32>(width * 4u)),
                          0);
                stbi_flip_vertically_on_write(0);
            }
        }
    };

    // ── 1. Fog ───────────────────────────────────────────────────────────────
    //
    // THE MECHANISM FIRST: at every coat pixel, the depth the fog pass reads
    // must be the COAT's -- in front of what stood there before any coat drew
    // -- or the fog there is the fog of the ground behind the horse. Then the
    // picture: the coat's fog transmittance, fitted from the fogged and
    // unfogged frames in linear light, must sit with its body's, which stands
    // at the same distance.
    TEST_F(GroomSceneLightingEvidenceTest, FogReachesTheCoatAsItReachesItsBody)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const glm::vec3 target(0.0f, 1.1f, 0.0f);
        const std::array eyes{ glm::vec3(3.0f, 1.4f, 4.0f), glm::vec3(-3.5f, 1.8f, 3.0f) };
        for (const auto& [path, pathName] : kPaths)
        {
            SetPath(path);
            for (const bool volumetric : { false, true })
            {
                for (sizet angle = 0; angle < eyes.size(); ++angle)
                {
                    const std::string cell = std::string(pathName) + (volumetric ? "_Volumetric_" : "_Analytic_") +
                                             std::to_string(angle);
                    SCOPED_TRACE(cell);
                    auto& fog = Renderer3D::GetFogSettings();
                    fog = FogSettings{};
                    fog.Enabled = false;
                    std::vector<u8> clear;
                    Capture("GroomSceneFogOff_GL_" + cell, eyes[angle], target, 12u, clear);
                    fog.Enabled = true;
                    fog.Mode = FogMode::ExponentialSquared;
                    fog.Color = glm::vec3(0.55f, 0.6f, 0.68f);
                    fog.Density = 0.17f;
                    // Near-uniform: the froxel chain thins the medium with height,
                    // and both arms should fog the horse at its own distance.
                    fog.HeightFalloff = 0.01f;
                    fog.HeightOffset = 0.0f;
                    fog.EnableVolumetric = volumetric;
                    std::vector<u8> fogged;
                    Capture("GroomSceneFog_GL_" + cell, eyes[angle], target, volumetric ? 24u : 12u, fogged);
                    ASSERT_EQ(m_FogDepth.size(), m_Ids.size()) << "the fog pass did not run";
                    ASSERT_EQ(m_PreGroomDepth.size(), m_Ids.size()) << "no pre-groom depth snapshot";

                    // Each region's fog as one transmittance T and in-scatter I:
                    // final = scene T + I holds for the analytic fog (T = 1 - f,
                    // I = C f) and the froxel fog alike, and both are near
                    // constant over a region at one distance. A least-squares
                    // line through (scene, final) per channel, in linear light,
                    // gives them; coat and body must share them.
                    struct Fit
                    {
                        f64 Sx[3]{}, Sy[3]{}, Sxx[3]{}, Sxy[3]{};
                        u32 N = 0;
                        void Add(const u8* clearPx, const u8* fogPx)
                        {
                            for (u32 c = 0; c < 3u; ++c)
                            {
                                const f64 x = Linear(clearPx[c]);
                                const f64 y = Linear(fogPx[c]);
                                Sx[c] += x;
                                Sy[c] += y;
                                Sxx[c] += x * x;
                                Sxy[c] += x * y;
                            }
                            ++N;
                        }
                        [[nodiscard]] f64 Transmittance() const
                        {
                            f64 t = 0.0;
                            for (u32 c = 0; c < 3u; ++c)
                            {
                                const f64 n = static_cast<f64>(N);
                                const f64 det = n * Sxx[c] - Sx[c] * Sx[c];
                                t += det > 1.0e-12 ? (n * Sxy[c] - Sx[c] * Sy[c]) / det : 0.0;
                            }
                            return t / 3.0;
                        }
                    };
                    Fit coatFit;
                    Fit bodyFit;
                    u32 coatPixels = 0;
                    u32 coatInFogDepth = 0;
                    for (sizet i = 0; i < m_Ids.size(); ++i)
                    {
                        if (m_Ids[i] == CoatId())
                        {
                            ++coatPixels;
                            coatInFogDepth += m_FogDepth[i] < m_PreGroomDepth[i] ? 1u : 0u;
                            coatFit.Add(&clear[i * 4u], &fogged[i * 4u]);
                        }
                        else if (m_Ids[i] == BodyId())
                        {
                            bodyFit.Add(&clear[i * 4u], &fogged[i * 4u]);
                        }
                    }
                    const f32 depthShare = coatPixels > 0u ? static_cast<f32>(coatInFogDepth) / static_cast<f32>(coatPixels) : 0.0f;
                    const f64 coatT = coatFit.Transmittance();
                    const f64 bodyT = bodyFit.Transmittance();
                    std::printf("[groom-fog] %s coat px=%u in-fog-depth=%.3f transmittance coat=%.3f body=%.3f (body px=%u)\n",
                                cell.c_str(), coatPixels, depthShare, coatT, bodyT, bodyFit.N);
                    ASSERT_GT(coatPixels, 2000u);
                    ASSERT_GT(bodyFit.N, 2000u);
                    EXPECT_GT(depthShare, 0.95f) << "the fog pass reads a depth without the coat in it";
                    EXPECT_LT(bodyT, 0.85) << "the fog is too thin to tell anything";
                    EXPECT_NEAR(coatT, bodyT, 0.1) << "the coat is fogged differently from its body";
                }
            }
        }
    }

    // ── 2. DDGI interior ─────────────────────────────────────────────────────
    //
    // A closed room around the horse, a red wall beside it lit by a red point
    // light, the open sky shut out, and a Realtime probe volume over the floor.
    // Toggling the volume changes what the ambient ladder hands the BODY: the
    // red bounce. The coat must move the same way -- before #1618 it read the
    // sky's cube either way and did not move at all.
    TEST_F(GroomSceneLightingEvidenceTest, CoatAmbientFollowsTheProbeVolumeLikeItsBody)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene& scene = GetScene();
        scene.FindEntityByName("Ground").GetComponent<TransformComponent>().Translation.y = -10.0f;
        if (auto sun = scene.FindEntityByName("Sun"))
            sun.GetComponent<DirectionalLightComponent>().m_Intensity = 0.05f;
        const auto addBox = [&scene](const char* name, const glm::vec3& position, const glm::vec3& scale,
                                     const glm::vec3& albedo)
        {
            Entity e = scene.CreateEntity(name);
            auto& transform = e.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.Scale = scale;
            auto& mesh = e.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                mesh.m_MeshSource = cube->GetMeshSource();
            auto& material = e.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            material.m_Material.SetMetallicFactor(0.0f);
            material.m_Material.SetRoughnessFactor(0.9f);
        };
        addBox("Floor", { 0.0f, -0.05f, 0.0f }, { 12.0f, 0.1f, 12.0f }, { 0.5f, 0.5f, 0.5f });
        addBox("RedWall", { -3.0f, 2.5f, 0.0f }, { 0.2f, 5.0f, 10.0f }, { 0.9f, 0.15f, 0.1f });
        addBox("WhiteWall", { 3.5f, 2.5f, 0.0f }, { 0.2f, 5.0f, 10.0f }, { 0.85f, 0.85f, 0.85f });
        addBox("BackWall", { 0.0f, 2.5f, -5.0f }, { 7.0f, 5.0f, 0.2f }, { 0.85f, 0.85f, 0.85f });
        addBox("FrontWall", { 0.0f, 2.5f, 5.0f }, { 7.0f, 5.0f, 0.2f }, { 0.85f, 0.85f, 0.85f });
        addBox("Ceiling", { 0.0f, 5.0f, 0.0f }, { 7.0f, 0.2f, 10.0f }, { 0.85f, 0.85f, 0.85f });
        {
            Entity light = scene.CreateEntity("RedWallLight");
            light.GetComponent<TransformComponent>().Translation = { -1.5f, 3.5f, 1.5f };
            auto& point = light.AddComponent<PointLightComponent>();
            point.m_Color = { 1.0f, 0.95f, 0.9f };
            point.m_Intensity = 6.0f;
            point.m_Range = 9.0f;
            point.m_Attenuation = 1.0f;
            point.m_CastShadows = true;
        }
        Entity volume = scene.CreateEntity("ProbeVolume");
        {
            auto& probes = volume.AddComponent<LightProbeVolumeComponent>();
            probes.m_BoundsMin = { -2.7f, 0.3f, -4.6f };
            probes.m_BoundsMax = { 3.2f, 4.7f, 4.6f };
            probes.m_Resolution = { 4, 3, 5 };
            probes.m_Active = true;
            probes.m_Mode = LightProbeVolumeComponent::Mode::Realtime;
            probes.m_RaysPerProbe = 256;
            probes.m_Hysteresis = 0.5f;
            probes.m_ProbeCaptureBudget = 16;
            probes.m_RelightBudget = 0;
        }
        const glm::vec3 target(0.0f, 1.2f, 0.0f);
        const std::array eyes{ glm::vec3(2.6f, 1.6f, 4.2f), glm::vec3(-0.5f, 2.2f, 4.4f) };
        for (const auto& [path, pathName] : kPaths)
        {
            SetPath(path);
            for (sizet angle = 0; angle < eyes.size(); ++angle)
            {
                const std::string cell = std::string(pathName) + "_" + std::to_string(angle);
                SCOPED_TRACE(cell);
                volume.GetComponent<LightProbeVolumeComponent>().m_Active = false;
                volume.GetComponent<LightProbeVolumeComponent>().m_Dirty = true;
                std::vector<u8> sky;
                Capture("GroomSceneDdgiOff_GL_" + cell, eyes[angle], target, 12u, sky);
                const RegionMean coatSky = MeanOf(sky, m_Ids, CoatId());
                const RegionMean bodySky = MeanOf(sky, m_Ids, BodyId());
                volume.GetComponent<LightProbeVolumeComponent>().m_Active = true;
                volume.GetComponent<LightProbeVolumeComponent>().m_Dirty = true;
                std::vector<u8> probed;
                Capture("GroomSceneDdgi_GL_" + cell, eyes[angle], target, 90u, probed);
                const RegionMean coatProbe = MeanOf(probed, m_Ids, CoatId());
                const RegionMean bodyProbe = MeanOf(probed, m_Ids, BodyId());
                ASSERT_GT(coatSky.Pixels, 2000u);
                ASSERT_GT(bodySky.Pixels, 2000u);
                // Red over green, the bounce's signature, each side's change.
                const auto redness = [](const glm::vec3& c)
                { return c.r / std::max(c.g, 1.0e-4f); };
                const f32 coatShift = std::log(redness(coatProbe.Linear) / redness(coatSky.Linear));
                const f32 bodyShift = std::log(redness(bodyProbe.Linear) / redness(bodySky.Linear));
                std::printf("[groom-ddgi] %s coat sky=(%.4f %.4f %.4f) probe=(%.4f %.4f %.4f) shift=%.3f | body sky=(%.4f %.4f %.4f) probe=(%.4f %.4f %.4f) shift=%.3f\n",
                            cell.c_str(), coatSky.Linear.r, coatSky.Linear.g, coatSky.Linear.b, coatProbe.Linear.r,
                            coatProbe.Linear.g, coatProbe.Linear.b, coatShift, bodySky.Linear.r, bodySky.Linear.g,
                            bodySky.Linear.b, bodyProbe.Linear.r, bodyProbe.Linear.g, bodyProbe.Linear.b, bodyShift);
                EXPECT_GT(bodyShift, 0.05f) << "the probe volume did not redden the body; the rig proves nothing";
                EXPECT_GT(coatShift, 0.25f * bodyShift) << "the coat's ambient did not follow its body's into the probes";
            }
        }
    }

    // ── 3. Cluster lists ─────────────────────────────────────────────────────
    //
    // 32 coloured point lights in a ring round the horse. Forward walks the
    // whole buffer; Forward+ walks each fragment's cluster. The coat's pixels
    // must agree within the repeat floor, the cull must have run without its
    // depth-aware mask (a strand in front of open sky is in no occupied cell),
    // and the strands must actually have taken the cluster branch.
    TEST_F(GroomSceneLightingEvidenceTest, ClusterListsLightTheCoatAsTheWholeBufferDoes)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene& scene = GetScene();
        for (u32 i = 0; i < 32u; ++i)
        {
            const f32 angle = static_cast<f32>(i) * (6.28318530718f / 32.0f);
            Entity light = scene.CreateEntity("RingLight" + std::to_string(i));
            light.GetComponent<TransformComponent>().Translation = { 2.3f * std::cos(angle), 0.8f + 0.9f * static_cast<f32>(i % 3u),
                                                                     2.3f * std::sin(angle) };
            auto& point = light.AddComponent<PointLightComponent>();
            point.m_Color = glm::vec3(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle), 0.6f);
            point.m_Intensity = 1.5f;
            point.m_Range = 2.6f;
            point.m_Attenuation = 1.0f;
            point.m_CastShadows = false;
        }
        const glm::vec3 target(0.0f, 1.1f, 0.0f);
        const std::array eyes{ glm::vec3(3.0f, 1.4f, 4.0f), glm::vec3(-3.5f, 2.4f, -2.5f) };
        // Depth-aware culling ARMED, so "a coat in view stands it down" below is
        // a statement about the gate and not about a lever that was off.
        const bool prepassWas = Renderer3D::IsDepthPrepassEnabled();
        const bool depthAwareWas = Renderer3D::IsDepthAwareClusterCullingEnabled();
        Renderer3D::EnableDepthPrepass(true);
        Renderer3D::EnableDepthAwareClusterCulling(true);
        for (sizet angle = 0; angle < eyes.size(); ++angle)
        {
            const std::string cell = std::to_string(angle);
            SetPath(RenderingPath::Forward);
            std::vector<u8> whole, repeat, clustered;
            Capture("GroomSceneClusterWholeBuffer_GL_Forward_" + cell, eyes[angle], target, 12u, whole);
            EXPECT_FALSE(Renderer3D::GetGroomRenderPass()->GetStats().ClusteredLightLists);
            const std::vector<i32> ids = m_Ids;
            Capture("", eyes[angle], target, 12u, repeat);
            SetPath(RenderingPath::ForwardPlus);
            Capture("GroomSceneCluster_GL_ForwardPlus_" + cell, eyes[angle], target, 12u, clustered);
            EXPECT_TRUE(Renderer3D::GetGroomRenderPass()->GetStats().ClusteredLightLists);
            EXPECT_FALSE(Renderer3D::GetForwardPlus().WasLastCullingDepthAware())
                << "a frame with a coat culled lights against a depth the coat is not in";
            u32 coatPixels = 0;
            u32 noise = 0;
            u32 changed = 0;
            f32 maxDelta = 0.0f;
            for (sizet i = 0; i < ids.size(); ++i)
            {
                if (ids[i] != CoatId())
                    continue;
                ++coatPixels;
                bool repeatDiffers = false;
                bool clusterDiffers = false;
                for (u32 c = 0; c < 3u; ++c)
                {
                    const i32 a = whole[i * 4u + c];
                    repeatDiffers |= std::abs(a - repeat[i * 4u + c]) > 2;
                    const i32 d = std::abs(a - clustered[i * 4u + c]);
                    clusterDiffers |= d > 2;
                    maxDelta = std::max(maxDelta, static_cast<f32>(d));
                }
                noise += repeatDiffers ? 1u : 0u;
                changed += clusterDiffers ? 1u : 0u;
            }
            std::printf("[groom-clusters] %s coat px=%u differ(cluster)=%u differ(repeat)=%u max=%.0f\n", cell.c_str(),
                        coatPixels, changed, noise, maxDelta);
            ASSERT_GT(coatPixels, 2000u);
            EXPECT_LE(changed, noise + coatPixels / 200u) << "the cluster walk lights the coat differently";
        }

        // THE CONTROL: the same room on Forward+, the camera turned away from
        // the coat, and a shadow-casting lamp at the coat whose reach is in
        // view, so the coat is still submitted -- for its shadow. It is marked
        // out of view, and the cull goes depth-aware again: only a coat on
        // screen stands the cull down.
        {
            Entity lamp = GetScene().CreateEntity("ShadowLampAtTheCoat");
            lamp.GetComponent<TransformComponent>().Translation = target + glm::vec3(0.0f, 0.6f, 0.0f);
            auto& shadowLamp = lamp.AddComponent<PointLightComponent>();
            shadowLamp.m_Intensity = 1.0f;
            shadowLamp.m_Range = 6.0f;
            shadowLamp.m_CastShadows = true;
            const bool hadSceneShadow = m_Coat.HasComponent<GroomSceneShadowComponent>();
            auto& sceneShadow = hadSceneShadow ? m_Coat.GetComponent<GroomSceneShadowComponent>()
                                               : m_Coat.AddComponent<GroomSceneShadowComponent>();
            const bool castWas = sceneShadow.m_CastShadows;
            sceneShadow.m_CastShadows = true;
            std::vector<u8> away;
            Capture("", glm::vec3(0.0f, 1.4f, 3.0f), glm::vec3(0.0f, 1.4f, 6.0f), 12u, away);
            const i64 submitted = Renderer3D::GetGroomStrandRequests().Num();
            u32 inView = 0;
            for (const GroomStrandRequest& request : Renderer3D::GetGroomStrandRequests())
            {
                inView += request.InView ? 1u : 0u;
            }
            std::printf("[groom-clusters] turned away: %lld coat(s) submitted, %u in view, depth-aware=%d\n",
                        static_cast<long long>(submitted), inView,
                        Renderer3D::GetForwardPlus().WasLastCullingDepthAware() ? 1 : 0);
            EXPECT_GE(submitted, 1) << "the coat was dropped, so this frame says nothing about a shadow-only coat";
            EXPECT_EQ(inView, 0u) << "a coat behind the camera was counted as in view";
            EXPECT_TRUE(Renderer3D::GetForwardPlus().WasLastCullingDepthAware())
                << "with no coat on screen the cull should be depth-aware again";
            GetScene().DestroyEntity(lamp);
            if (hadSceneShadow)
            {
                m_Coat.GetComponent<GroomSceneShadowComponent>().m_CastShadows = castWas;
            }
            else
            {
                m_Coat.RemoveComponent<GroomSceneShadowComponent>();
            }
        }
        Renderer3D::EnableDepthPrepass(prepassWas);
        Renderer3D::EnableDepthAwareClusterCulling(depthAwareWas);
    }

    // ── 4. Cost ──────────────────────────────────────────────────────────────
    //
    // GroomPass GPU ms at 1, 32 and 256 local lights, on the whole-buffer walk
    // (Forward) and the cluster walk (Forward+). Recorded, not gated: the
    // adaptive clocks of a workstation GPU make a fixed threshold a coin toss
    // (the #1622 perf evidence, same reason). The base commit's shader run
    // against this binary is the "before" (see the PR).
    TEST_F(GroomSceneLightingEvidenceTest, GroomPassCostAtOneThirtyTwoAndTwoHundredFiftySixLights)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene& scene = GetScene();
        std::vector<Entity> lights;
        for (u32 i = 0; i < 256u; ++i)
        {
            const f32 angle = static_cast<f32>(i) * 2.39996323f; // golden angle
            const f32 radius = 1.2f + 6.0f * std::sqrt(static_cast<f32>(i) / 256.0f);
            Entity light = scene.CreateEntity("CostLight" + std::to_string(i));
            light.GetComponent<TransformComponent>().Translation = { radius * std::cos(angle),
                                                                     0.5f + 0.6f * static_cast<f32>(i % 4u),
                                                                     radius * std::sin(angle) };
            auto& point = light.AddComponent<PointLightComponent>();
            point.m_Intensity = 0.4f;
            point.m_Range = 2.5f;
            point.m_Attenuation = 1.0f;
            point.m_CastShadows = false;
            lights.push_back(light);
        }
        EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 200.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(0.0f, 1.6f, 5.5f), 0.0f, 0.12f);
        for (const auto& [path, pathName] : { std::pair{ RenderingPath::Forward, "Forward" },
                                              std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" } })
        {
            SetPath(path);
            for (const u32 count : { 1u, 32u, 256u })
            {
                // Removed rather than dimmed: a light at zero intensity still
                // occupies its slot and its clusters, and the count that
                // matters is how many the walk visits.
                for (sizet i = 0; i < lights.size(); ++i)
                {
                    if (i >= count && lights[i].HasComponent<PointLightComponent>())
                        lights[i].RemoveComponent<PointLightComponent>();
                    else if (i < count && !lights[i].HasComponent<PointLightComponent>())
                    {
                        auto& point = lights[i].AddComponent<PointLightComponent>();
                        point.m_Intensity = 0.4f;
                        point.m_Range = 2.5f;
                        point.m_Attenuation = 1.0f;
                        point.m_CastShadows = false;
                    }
                }
                Time::SetMockTime(12.0f);
                RunEditorFrames(camera, 30u, 0.0f);
                auto& timer = GPUPassTimerPool::GetInstance();
                u64 previous = timer.GetCurrentFrameNumber();
                std::vector<f64> costs;
                for (u32 frame = 0; frame < 120u && costs.size() < 30u; ++frame)
                {
                    RunEditorFrames(camera, 1u, 0.0f);
                    const auto report = timer.GetLastFrameTimings();
                    if (report.FrameNumber <= previous || report.IsStale())
                        continue;
                    previous = report.FrameNumber;
                    for (const auto& timing : report.Passes)
                        if (timing.Name == "GroomPass" && timing.IsValid() && std::isfinite(timing.Sample.GpuMs) &&
                            timing.Sample.GpuMs > 0.0)
                        {
                            costs.push_back(timing.Sample.GpuMs);
                            break;
                        }
                }
                Time::ClearMockTime();
                ASSERT_GE(costs.size(), 10u) << pathName << " " << count;
                std::ranges::sort(costs);
                const f64 median = costs[costs.size() / 2u];
                const auto& stats = Renderer3D::GetGroomRenderPass()->GetStats();
                std::printf("[groom-light-cost] %s lights=%u GroomPass median=%.3f ms min=%.3f ms n=%zu clustered=%d strands=%u\n",
                            pathName, count, median, costs.front(), costs.size(), stats.ClusteredLightLists ? 1 : 0,
                            stats.StrandsDrawn);
                ::testing::Test::RecordProperty(std::string("groom_pass_ms_") + pathName + "_" + std::to_string(count),
                                                std::to_string(median));
            }
        }
    }
} // namespace OloEngine::Tests
