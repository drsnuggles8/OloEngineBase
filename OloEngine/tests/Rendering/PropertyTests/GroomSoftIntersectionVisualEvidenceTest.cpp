// OLO_TEST_LAYER: L8
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "AttachmentFeedbackEvidence.h"
#include "../../TestOptions.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace OloEngine::Tests
{
    namespace
    {
        const std::filesystem::path kEditor{ OLO_TEST_EDITOR_ROOT };
        constexpr std::array kPaths{
            std::pair{ RenderingPath::Forward, "Forward" },
            std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
            std::pair{ RenderingPath::Deferred, "Deferred" }
        };

        u32 DifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size())
                return 0;
            u32 count = 0;
            for (sizet i = 0; i < a.size(); i += 4)
                count += std::abs(static_cast<i32>(a[i]) - b[i]) > 2 ||
                         std::abs(static_cast<i32>(a[i + 1]) - b[i + 1]) > 2 ||
                         std::abs(static_cast<i32>(a[i + 2]) - b[i + 2]) > 2;
            return count;
        }
    } // namespace

    // Uses shipped cooked coats and their real bound bodies. The task-owned
    // scenes freeze animation/simulation for matched A/B histories and retain
    // actual terrain and the displaced, refracting water renderer.
    class GroomSoftIntersectionVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Coat;
        bool m_Dog = true;
        u32 m_Width = 1280;
        u32 m_Height = 720;
        u32 m_SettleFrames = 16;
        f32 m_WorldScale = 1.0f;
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssets;
        bool m_StateCaptured = false;

        void BuildScene() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            m_StateCaptured = true;
            ASSERT_TRUE(Project::Load(kEditor / "SandboxProject/Sandbox.oloproj"));
            auto assets = Ref<EditorAssetManager>::Create();
            assets->Initialize(false);
            Project::SetAssetManager(assets);
            EnableRendering(m_Width, m_Height);
            auto& settings = Renderer3D::GetRendererSettings();
            settings.EditorDebugDrawsEnabled = false;
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowComponentGizmos = false;
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            if (!m_StateCaptured)
                return;
            Project::Unload();
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            if (m_PreviousAssets)
                Project::SetAssetManager(m_PreviousAssets);
        }

        void LoadSubject(bool dog)
        {
            m_Dog = dog;
            std::vector<UUID> previous;
            const auto view = GetScene().GetAllEntitiesWith<IDComponent>();
            for (auto entity : view)
                previous.push_back(view.get<IDComponent>(entity).ID);
            for (const UUID id : previous)
                if (auto entity = GetScene().TryGetEntityWithUUID(id))
                    GetScene().DestroyEntity(*entity);
            const auto fixture = kEditor / "SandboxProject/Assets/Scenes/Tests" /
                                 (dog ? "GroomSoftIntersectionDog.olo" : "GroomSoftIntersectionHorse.olo");
            ASSERT_TRUE(SceneSerializer(GetSceneRef()).Deserialize(fixture.string()));
            EnableRendering(m_Width, m_Height);
            m_Coat = GetScene().FindEntityByName(dog ? "DogCoat" : "LongCoatHorseCoat");
            ASSERT_TRUE(m_Coat && m_Coat.HasComponent<GroomSoftIntersectionComponent>());
        }

        void Capture(const std::string& name, bool enabled, bool cards, i32 angle, std::vector<u8>& pixels)
        {
            struct RestoreClock
            {
                ~RestoreClock()
                {
                    Time::ClearMockTime();
                }
            } restoreClock;
            Time::SetMockTime(12.0f);
            // THE TIER IS FORCED, NOT HANDED OVER TO. This case compares the
            // contact fade on and off on one representation; a #1612 cross-fade
            // from the previous capture's tier would leave the outgoing tier in
            // the stochastic coat's TAA history for the whole settle, and the
            // first capture of a cell would differ from its own repeat by that
            // tail. Held off for this capture, and restored after it.
            struct RestoreCrossFade
            {
                u32 Frames = Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames;
                ~RestoreCrossFade()
                {
                    Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames = Frames;
                }
            } restoreCrossFade;
            Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames = 0u;
            m_Coat.GetComponent<GroomSoftIntersectionComponent>().m_Enabled = enabled;
            auto& lod = m_Coat.GetComponent<GroomLodComponent>();
            lod.m_Enabled = cards;
            lod.m_CardPixelSize = 16384.0f;
            lod.m_MeshPixelSize = 0.5f;
            lod.m_HoldFrames = 0;
            lod.m_VisibilitySteps = 0;
            lod.m_SimulationSteps = 0;
            lod.m_ShadowSteps = 0;
            EditorCamera camera(45.0f, static_cast<f32>(m_Width) / static_cast<f32>(m_Height), 0.02f * m_WorldScale, 100.0f * m_WorldScale);
            camera.SetViewportSize(static_cast<f32>(m_Width), static_cast<f32>(m_Height));
            const glm::vec3 position = m_Dog ? (angle == 0 ? glm::vec3(0.65f, 0.35f, 1.4f) : glm::vec3(-1.1f, 0.55f, 1.35f))
                                             : (angle == 0 ? glm::vec3(3.0f, 1.2f, 4.0f) : glm::vec3(-3.0f, 1.5f, 4.0f));
            const glm::vec3 target = m_Dog ? glm::vec3(-0.3f, 0.12f, 0.04f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 delta = position - target;
            const f32 yaw = -std::atan2(delta.x, delta.z);
            const f32 pitch = std::asin(delta.y / glm::length(delta));
            camera.SetPose(position * m_WorldScale, yaw, pitch);
            Renderer3D::ResetFrameSequences();
            RunEditorFrames(camera, m_SettleFrames, 0.0f);
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            ASSERT_EQ(width, m_Width);
            ASSERT_EQ(height, m_Height);
            if (const auto graph = RenderGraphDebugRuntime::GetActiveGraph(); graph && graph->GetRenderScale() < 1.0f)
            {
                const u32 renderWidth = graph->GetRenderWidth();
                const u32 renderHeight = graph->GetRenderHeight();
                ASSERT_GT(renderWidth, 0u);
                ASSERT_GT(renderHeight, 0u);
                ASSERT_LE(renderWidth, width);
                ASSERT_LE(renderHeight, height);
                // Display-sized targets contain the frame in their [0, render)
                // corner. The unused margin is not part of this capture (#1526).
                std::vector<u8> cropped(static_cast<sizet>(renderWidth) * renderHeight * 4u);
                for (u32 y = 0; y < renderHeight; ++y)
                    std::copy_n(pixels.data() + static_cast<sizet>(y) * width * 4u, renderWidth * 4u,
                                cropped.data() + static_cast<sizet>(y) * renderWidth * 4u);
                pixels = std::move(cropped);
                width = renderWidth;
                height = renderHeight;
            }
            const auto pass = Renderer3D::GetGroomRenderPass();
            ASSERT_TRUE(pass);
            EXPECT_GT(pass->GetStats().GroomsDrawn, 0u);
            EXPECT_EQ(pass->GetStats().GroomsBindingRefused, 0u);
            EXPECT_GT(pass->GetStats().Lod.ByRepresentation[static_cast<sizet>(cards ? GroomRepresentation::Card
                                                                                     : GroomRepresentation::Strand)],
                      0u);
            if (!name.empty() && name.find("Rejected1537") == std::string::npos)
            {
                const auto directory = kEditor.parent_path() / ".v-build/1614/visual";
                std::filesystem::create_directories(directory);
                stbi_flip_vertically_on_write(1);
                EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(),
                                         static_cast<i32>(width), static_cast<i32>(height), 4, pixels.data(),
                                         static_cast<i32>(width * 4u)),
                          0);
                stbi_flip_vertically_on_write(0);
            }
        }

        void ReadIntersectionDepth(std::vector<f32>& depth, std::string_view name = "GroomSceneDepthSnapshot")
        {
            const auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            const u32 texture = graph->ResolveTexture(graph->GetTextureHandle(name));
            ASSERT_NE(texture, 0u);
            depth.resize(static_cast<sizet>(m_Width) * m_Height);
            glGetTextureImage(texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                              static_cast<GLsizei>(depth.size() * sizeof(f32)), depth.data());
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
        }

        void CheckPair(const std::string& cell, bool cards, i32 angle)
        {
            std::vector<u8> off, repeat, on;
            Capture("GroomSoftIntersectionOff_GL_" + cell, false, cards, angle, off);
            Capture("", false, cards, angle, repeat);
            if (const auto graph = RenderGraphDebugRuntime::GetActiveGraph())
            {
                const auto culled = graph->GetCulledPasses();
                for (const std::string_view name : { "GroomSceneDepthSnapshotPass", "GroomSceneEntitySnapshotPass", "GroomWaterDepthPass" })
                    EXPECT_TRUE(std::ranges::any_of(culled, [name](const FString& pass)
                                                    { return pass.ToView() == name; }))
                        << "disabled coats must cull " << name;
            }
            Capture("GroomSoftIntersection_GL_" + cell, true, cards, angle, on);
            ASSERT_FALSE(off.empty());
            const u32 noise = DifferingPixels(off, repeat);
            const u32 changed = DifferingPixels(off, on);
            std::printf("[soft-intersection] %s changed=%u repeat=%u viewport=%ux%u\n",
                        cell.c_str(), changed, noise, m_Width, m_Height);
            EXPECT_GT(changed, noise + 8u) << "contact fade did not exceed the matched repeat floor";
            CheckEnabledGraph();
        }

        // What the frame the last capture rendered, with the fade on, must
        // hold in its graph: no attachment read while written, snapshots that
        // sample like their sources, an integer snapshot filtered nearest and
        // a depth snapshot that is its own texture. No frames of its own.
        void CheckEnabledGraph()
        {
            ExpectAttachmentFeedbackGraphClean("GroomPass");
            ExpectAttachmentFeedbackGraphClean("GroomSceneDepthSnapshotPass");
            ExpectAttachmentFeedbackGraphClean("GroomSceneEntitySnapshotPass");
            ExpectAttachmentSnapshotSamplingMatchesSource("SceneDepthAttachment", "GroomSceneDepthSnapshot");
            ExpectAttachmentSnapshotSamplingMatchesSource("SceneEntityID", "GroomSceneEntitySnapshot");
            const auto integerSnapshot = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(integerSnapshot);
            const u32 idTexture = integerSnapshot->ResolveTexture(integerSnapshot->GetTextureHandle("GroomSceneEntitySnapshot"));
            for (const GLenum filter : { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER })
            {
                GLint value = 0;
                glGetTextureParameteriv(idTexture, filter, &value);
                EXPECT_EQ(value, GL_NEAREST);
            }
            const auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            EXPECT_NE(graph->ResolveTexture(graph->GetTextureHandle("SceneDepthAttachment")),
                      graph->ResolveTexture(graph->GetTextureHandle("GroomSceneDepthSnapshot")));
            if (!m_Dog)
                ExpectAttachmentFeedbackGraphClean("GroomWaterDepthPass");
        }
    };

    TEST_F(GroomSoftIntersectionVisualEvidenceTest, RealDogTerrainAndHorseWaterAcrossPathsAndRepresentations)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const bool dog : { true, false })
        {
            LoadSubject(dog);
            for (const auto& [path, name] : kPaths)
            {
                Renderer3D::GetRendererSettings().Path = path;
                Renderer3D::ApplyRendererSettings();
                for (const bool cards : { false, true })
                    for (i32 angle = 0; angle < 2; ++angle)
                        CheckPair(std::string(name) + (dog ? "_DogTerrain" : "_HorseWater") +
                                      (cards ? "_Cards_" : "_Strands_") + std::to_string(angle),
                                  cards, angle);
            }
        }
    }

    TEST_F(GroomSoftIntersectionVisualEvidenceTest, DepthInputsSurviveMsaaUpscaleAndViewportChanges)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const u32 width : { 1280u, 1920u })
        {
            m_Width = width;
            m_Height = width == 1280u ? 720u : 1080u;
            ResizeRenderTarget(m_Width, m_Height);
            for (const bool dog : { true, false })
            {
                LoadSubject(dog);
                for (const auto& [path, name] : kPaths)
                {
                    ForEachAttachmentFeedbackCell(path, name, [&](const std::string& cell, u32 settle)
                                                  {
                        m_SettleFrames = std::max(8u, settle);
                        if (cell.find("Rejected1537") != std::string::npos)
                        {
                            std::vector<u8> unused;
                            Capture("", true, false, 0, unused);
                            std::printf("[soft-intersection-rejected] %s display=%ux%u (#1537; both representations)\n",
                                        cell.c_str(), m_Width, m_Height);
                            return;
                        }
                        // A forward path asked for MSAA renders at 1x: the cell's
                        // pairs would repeat the 1x cell's frames exactly. One
                        // frame latches the settings for the helper's checks that
                        // the request resolved to one sample, and its graph is
                        // checked as a pair's would be (#1630).
                        if (cell.find("MSAARequest") != std::string::npos)
                        {
                            std::vector<u8> unused;
                            Capture("", true, false, 0, unused);
                            CheckEnabledGraph();
                            std::printf("[soft-intersection-msaa-request] %s display=%ux%u renders the 1x cell's frames\n",
                                        cell.c_str(), m_Width, m_Height);
                            return;
                        }
                        for (const bool cards : { false, true })
                            CheckPair(cell + (dog ? "_DogTerrain" : "_HorseWater") +
                                      (cards ? "_Cards_" : "_Strands_") + std::to_string(width), cards, 0); });
                }
            }
        }
    }

    TEST_F(GroomSoftIntersectionVisualEvidenceTest, AwayReferenceAndTwoViewControlsStayStable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const bool dog : { true, false })
        {
            LoadSubject(dog);
            if (dog)
                GetScene().FindEntityByName("Lawn").GetComponent<TransformComponent>().Translation.y = -10.0f;
            else
            {
                GetScene().FindEntityByName("Ground").GetComponent<TransformComponent>().Translation.y = -10.0f;
                GetScene().FindEntityByName("ShallowWater").GetComponent<WaterComponent>().m_Enabled = false;
            }
            for (const bool fine : { false, true })
            {
                m_WorldScale = fine ? 0.1f : 1.0f;
                if (fine)
                {
                    auto body = GetScene().FindEntityByName(dog ? "Dog" : "LongCoatHorse");
                    ASSERT_TRUE(body);
                    auto& transform = body.GetComponent<TransformComponent>();
                    transform.Translation *= m_WorldScale;
                    transform.Scale *= m_WorldScale;
                    if (!dog)
                    {
                        // The horse coat is a separate root entity; the dog's
                        // coat inherits its body's scale through its parent.
                        auto& coatTransform = m_Coat.GetComponent<TransformComponent>();
                        coatTransform.Translation *= m_WorldScale;
                        coatTransform.Scale *= m_WorldScale;
                    }
                    auto& coat = m_Coat.GetComponent<GroomCoatComponent>();
                    coat.m_UndercoatLength = coat.m_UndercoatWidth = 0.1f;
                    coat.m_GuardLength = coat.m_GuardWidth = 0.1f;
                }
                for (const bool cards : { false, true })
                {
                    std::vector<u8> off, repeat, on, otherView, returned;
                    Capture("", false, cards, 0, off);
                    Capture("", false, cards, 0, repeat);
                    Capture(std::string("GroomSoftIntersectionReference_GL_") + (dog ? "Dog" : "Horse") +
                                (cards ? "_Cards" : "_Strands") + (fine ? "_TinyFine" : ""),
                            true, cards, 0, on);
                    Capture("", true, cards, 1, otherView);
                    Capture("", true, cards, 0, returned);
                    const u32 noise = DifferingPixels(off, repeat);
                    std::printf("[soft-intersection-reference] %s %s changed=%u repeat=%u returned=%u\n", dog ? "Dog" : "Horse",
                                cards ? "cards" : "strands", DifferingPixels(off, on), noise, DifferingPixels(on, returned));
                    EXPECT_LE(DifferingPixels(off, on), noise + 8u);
                    EXPECT_LE(DifferingPixels(on, returned), noise + 8u);
                    EXPECT_GT(DifferingPixels(on, otherView), 100u);
                }
            }
            m_WorldScale = 1.0f;
        }
    }

    TEST_F(GroomSoftIntersectionVisualEvidenceTest, PrepassAndRootProtectionControlsReachTheRealDraws)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct RestorePrepass
        {
            bool Saved = Levers::GroomNoDepthPrepass();
            ~RestorePrepass()
            {
                Levers::SetGroomNoDepthPrepass(Saved);
            }
        } restore;
        LoadSubject(false);
        std::vector<u8> prepass, shaded, unprotected, fullyProtected, off;
        // Settle the freshly loaded body/binding before comparing render modes.
        Capture("", true, false, 0, prepass);
        auto graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph);
        struct RemoveHook
        {
            Ref<RenderGraph> Graph;
            ~RemoveHook()
            {
                Graph->RemovePostPassHook("GroomPrepassCoverage");
            }
        } removeHook{ graph };
        std::vector<f32> capturedDepth;
        graph->AddPostPassHook("GroomPrepassCoverage", [&](std::string_view pass, RenderGraph&)
                               {
                                  if (pass == "GroomPass")
                                      ReadIntersectionDepth(capturedDepth, "SceneDepthAttachment"); });
        Capture("GroomSoftIntersectionPrepass_GL_Horse", true, false, 0, prepass);
        const auto prepassDepth = capturedDepth;
        ASSERT_FALSE(prepassDepth.empty());
        Levers::SetGroomNoDepthPrepass(true);
        Capture("GroomSoftIntersectionNoPrepass_GL_Horse", true, false, 0, shaded);
        EXPECT_EQ(prepassDepth, capturedDepth) << "LESS/EQUAL and shaded-only draws disagree about covered depth";
        std::printf("[soft-intersection-prepass] beauty difference=%u pixels\n", DifferingPixels(prepass, shaded));
        // The two modes have different fragment invocation/derivative work.
        // Pin contact coverage against each mode's own disabled control.
        CheckPair("NoPrepass_HorseWater_Strands_0", false, 0);
        Levers::SetGroomNoDepthPrepass(false);
        m_Coat.GetComponent<GroomSoftIntersectionComponent>().m_ProtectedRoot = 0.0f;
        Capture("GroomSoftIntersectionUnprotected_GL_Horse", true, false, 0, unprotected);
        EXPECT_GT(DifferingPixels(prepass, unprotected), 8u);
        m_Coat.GetComponent<GroomSoftIntersectionComponent>().m_ProtectedRoot = 1.0f;
        Capture("GroomSoftIntersectionFullyProtected_GL_Horse", true, false, 0, fullyProtected);
        Capture("", false, false, 0, off);
        EXPECT_LE(DifferingPixels(fullyProtected, off), 8u);
    }

    TEST_F(GroomSoftIntersectionVisualEvidenceTest, OverlappingCoatsAndMovingBodyUseFreshIndependentInputs)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadSubject(false);
        auto second = GetScene().DuplicateEntity(m_Coat);
        ASSERT_TRUE(second);
        second.GetComponent<GroomSoftIntersectionComponent>().m_ProtectedRoot = 1.0f;
        auto& overlay = second.GetComponent<GroomCoatComponent>();
        overlay.m_UndercoatDensity = 0.12f;
        overlay.m_UndercoatLength = 1.2f;
        overlay.m_GuardDensity = 0.12f;
        overlay.m_GuardLength = 1.2f;
        second.GetComponent<GroomComponent>().m_MaxRenderStrands = 1000;
        // A sparse, longer protected coat writes depth without covering every
        // contact pixel of the target coat. Its depth must not contaminate the
        // shared intersection input.
        std::vector<u8> off, repeat, contact, moved, returned;
        Capture("", false, false, 0, off);
        auto graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph);
        constexpr auto hookKey = "GroomIntersectionInputContents";
        struct RemoveHook
        {
            Ref<RenderGraph> Graph;
            ~RemoveHook()
            {
                Graph->RemovePostPassHook("GroomIntersectionInputContents");
            }
        } removeHook{ graph };
        std::vector<f32> beforeGroom, opaqueDepth, afterGroom;
        u32 checkedFrames = 0;
        bool observedReuse = false;
        graph->AddPostPassHook(hookKey, [&](std::string_view pass, RenderGraph&)
                               {
            if (pass == "GroomSceneDepthSnapshotPass")
            {
                ReadIntersectionDepth(beforeGroom);
                ReadIntersectionDepth(opaqueDepth, "SceneDepthAttachment");
                EXPECT_EQ(beforeGroom, opaqueDepth) << "snapshot differs from this frame's pre-groom depth";
            }
            else if (pass == "GroomPass")
            {
                ReadIntersectionDepth(afterGroom);
                EXPECT_EQ(beforeGroom, afterGroom) << "groom writes contaminated their shared depth input";
                ++checkedFrames;
            }
            else if (!observedReuse && (pass == "SceneDepthSnapshotPass" || pass == "DiffusionDepthSnapshotPass"))
            {
                const auto laterName = pass == "SceneDepthSnapshotPass" ? "SceneDepthSnapshot" : "DiffusionDepthSnapshot";
                const auto earlyTexture = graph->ResolveTexture(graph->GetTextureHandle("GroomSceneDepthSnapshot"));
                const auto laterTexture = graph->ResolveTexture(graph->GetTextureHandle(laterName));
                if (earlyTexture != 0u && earlyTexture == laterTexture)
                {
                    std::printf("[soft-intersection-lifetime] snapshot backing reused by %s texture=%u\n", laterName, earlyTexture);
                    observedReuse = true;
                }
            } });
        Capture("GroomSoftIntersectionOverlap_GL_Off", false, false, 0, off);
        Capture("", false, false, 0, repeat);
        std::vector<f32> offDepth, contactDepth, movedDepth, returnedDepth;
        // Copy at the snapshot hook: its backing may be reused by later graph
        // passes once GroomPass has consumed it. End-of-frame readback is late.
        offDepth = beforeGroom;
        Capture("", true, false, 0, contact);
        Capture("GroomSoftIntersectionOverlap_GL_Contact", true, false, 0, contact);
        ASSERT_EQ(Renderer3D::GetGroomRenderPass()->GetStats().GroomsDrawn, 2u);
        EXPECT_GT(DifferingPixels(off, contact), DifferingPixels(off, repeat) + 8u);
        contactDepth = beforeGroom;
        u32 changedDepth = 0;
        f32 maxDepthDifference = 0.0f;
        for (sizet i = 0; i < offDepth.size(); ++i)
        {
            changedDepth += offDepth[i] != contactDepth[i];
            maxDepthDifference = std::max(maxDepthDifference, std::abs(offDepth[i] - contactDepth[i]));
        }
        std::printf("[soft-intersection-overlap] depth changed=%u max=%g\n", changedDepth, maxDepthDifference);
        EXPECT_EQ(offDepth, contactDepth) << "coat coverage changed its own intersection depth input";
        auto body = GetScene().FindEntityByName("LongCoatHorse");
        ASSERT_TRUE(body);
        auto& transform = body.GetComponent<TransformComponent>();
        const auto saved = transform.Translation;
        transform.Translation.y += 0.3f;
        Capture("GroomSoftIntersectionOverlap_GL_Moved", true, false, 0, moved);
        movedDepth = beforeGroom;
        EXPECT_NE(contactDepth, movedDepth) << "body motion left stale intersection depth";
        ExpectAttachmentFeedbackGraphClean("GroomPass");
        ExpectAttachmentSnapshotSamplingMatchesSource("SceneDepthAttachment", "GroomSceneDepthSnapshot");
        transform.Translation = saved;
        Capture("GroomSoftIntersectionOverlap_GL_Returned", true, false, 0, returned);
        returnedDepth = beforeGroom;
        EXPECT_EQ(contactDepth, returnedDepth);
        EXPECT_LE(DifferingPixels(contact, returned), 8u);
        EXPECT_GT(DifferingPixels(contact, moved), 100u);
        EXPECT_GT(checkedFrames, 0u);
    }

    TEST_F(GroomSoftIntersectionVisualEvidenceTest, RealCoatOverridesSurviveColdAssetPackDelivery)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto sandbox = kEditor / "SandboxProject";
        for (const bool dog : { true, false })
        {
            LoadSubject(dog);
            auto& component = m_Coat.GetComponent<GroomSoftIntersectionComponent>();
            component.m_Automatic = false;
            component.m_FadeDistance = dog ? 0.003f : 0.006f;
            component.m_ProtectedRoot = 0.35f;
            const auto expected = component;
            std::vector<u8> pixels;
            Capture("", true, false, 0, pixels);
            auto sourceAssets = Project::GetAssetManager().As<EditorAssetManager>();
            ASSERT_TRUE(sourceAssets);
            struct RestoreProject
            {
                Ref<Project> Previous = Project::GetActive();
                Ref<AssetManagerBase> Assets = Project::GetAssetManager();
                std::filesystem::path Directory = std::filesystem::current_path();
                ~RestoreProject()
                {
                    std::filesystem::current_path(Directory);
                    Project::Unload();
                    Project::NewInMemory(Previous->GetDirectory(), Previous->GetConfig());
                    Project::SetAssetManager(Assets);
                }
            } restore;
            const std::string subject = dog ? "Dog" : "Horse";
            const auto source = TempDir(subject);
            const auto relativeScene = std::filesystem::path("Assets/Scenes") / (subject + ".olo");
            std::filesystem::create_directories((source / relativeScene).parent_path());
            EXPECT_TRUE(SceneSerializer(GetSceneRef()).Serialize(source / relativeScene));
            sizet copied = 0;
            std::string error;
            std::vector<std::string> unresolved;
            ASSERT_TRUE(StageSceneReferencedContent({ source / relativeScene }, sandbox, "Assets", kEditor,
                                                    source, copied, unresolved, error))
                << error;
            ASSERT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());
            AssetRegistry selected;
            YAML::Emitter registeredReferences;
            registeredReferences << YAML::BeginSeq;
            // The real scene has already resolved its handle-based dependencies.
            // Retain their metadata/handles and ship their exact authored files.
            for (const auto& [handle, asset] : sourceAssets->GetLoadedAssets())
            {
                (void)asset;
                const auto metadata = sourceAssets->GetMetadata(handle);
                if (metadata.FilePath.empty() || !std::filesystem::is_regular_file(sandbox / metadata.FilePath))
                    continue;
                selected.AddAsset(metadata);
                registeredReferences << metadata.FilePath.generic_string();
                std::filesystem::create_directories((source / metadata.FilePath).parent_path());
                std::filesystem::copy_file(sandbox / metadata.FilePath, source / metadata.FilePath,
                                           std::filesystem::copy_options::overwrite_existing);
            }
            registeredReferences << YAML::EndSeq;
            // Meshes referenced by handle can have external glTF buffers and
            // images. Use the production dependency walker for their files too.
            const auto referenceList = source / "RegisteredAssets.yaml";
            {
                std::ofstream references(referenceList);
                ASSERT_TRUE(references);
                references << registeredReferences.c_str();
            }
            unresolved.clear();
            ASSERT_TRUE(StageSceneReferencedContent({ referenceList }, sandbox, "Assets", kEditor,
                                                    source, copied, unresolved, error))
                << error;
            ASSERT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());
            ASSERT_TRUE(selected.Serialize(source / "AssetRegistry.oar"));
            ProjectConfig config;
            config.Name = "GroomSoftIntersection" + subject;
            config.AssetDirectory = "Assets";
            config.StartScene = "Scenes/" + subject + ".olo";
            ASSERT_TRUE(Project::NewInMemory(source, config));
            auto editorAssets = Ref<EditorAssetManager>::Create();
            editorAssets->Initialize(false);
            Project::SetAssetManager(editorAssets);
            const auto sceneHandle = editorAssets->ImportAsset(source / relativeScene);
            ASSERT_NE(static_cast<u64>(sceneHandle), 0u);
            ASSERT_TRUE(editorAssets->SerializeAssetRegistry());
            const auto package = Options().CaptureOutDir.empty() ? source / "Package" : std::filesystem::absolute(Options().CaptureOutDir) / config.Name;
            std::atomic<f32> progress{ 0.0f };
            if (Options().CaptureOutDir.empty())
            {
                AssetPackBuilder::BuildSettings settings;
                settings.m_OutputPath = package / "Assets/AssetPack.olopack";
                settings.m_IncludeScriptModule = false;
                const auto built = AssetPackBuilder::BuildFromActiveProject(settings, progress);
                ASSERT_TRUE(built.m_Success) << built.m_ErrorMessage;
                ASSERT_EQ(built.m_FailedAssetCount, 0u);
            }
            else
            {
                GameBuildSettings settings;
                settings.GameName = config.Name;
                settings.OutputDirectory = std::filesystem::absolute(Options().CaptureOutDir);
                settings.StartScene = config.StartScene;
                settings.IncludeScriptModule = false;
                std::filesystem::current_path(kEditor);
                const auto built = GameBuildPipeline::Build(settings, progress);
                ASSERT_TRUE(built.Success) << built.ErrorMessage;
            }
            auto runtime = Ref<RuntimeAssetManager>::Create();
            ASSERT_TRUE(runtime->LoadAssetPack(package / "Assets/AssetPack.olopack"));
            Project::SetAssetManager(runtime);
            const auto loaded = runtime->GetAsset(sceneHandle).As<Scene>();
            ASSERT_TRUE(loaded);
            const auto coat = loaded->FindEntityByName(dog ? "DogCoat" : "LongCoatHorseCoat");
            ASSERT_TRUE(coat && coat.HasComponent<GroomSoftIntersectionComponent>());
            EXPECT_TRUE(coat.GetComponent<GroomSoftIntersectionComponent>() == expected);
            EXPECT_TRUE(runtime->GetAsset(coat.GetComponent<GroomComponent>().m_Groom));
            EXPECT_TRUE(runtime->GetAsset(coat.GetComponent<GroomBindingComponent>().m_Binding));
            std::printf("[soft-intersection-delivery] %s scene=%llu package=%s\n", subject.c_str(),
                        static_cast<unsigned long long>(sceneHandle), package.string().c_str());
        }
    }
} // namespace OloEngine::Tests
