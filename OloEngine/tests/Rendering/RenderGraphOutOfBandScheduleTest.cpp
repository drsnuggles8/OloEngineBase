// OLO_TEST_LAYER: integration

// =============================================================================
// The production out-of-band schedule on the real pipeline (issue #1331).
//
// RenderGraphOutOfBandTest.cpp pins the mechanism on stub graphs. This file
// runs the REAL renderer through Scene::OnUpdateRuntime and checks what the
// production passes declare against what they do:
//
//   * every path renders with a clean out-of-band ledger — every access a pass,
//     the prologue or the epilogue made was declared — also with the
//     tie-break reversed;
//   * the exposed schedule names a reason for every side effect and resolves
//     the frame-epilogue read of the final HZB rebuild;
//   * negative controls: drop one production declaration while its access
//     still runs, and the ledger reports it. For the froxel fog publication the
//     frame changes too: VolumetricFogPass, no longer consumed, is culled, and
//     FogPass must fall back rather than composite a stale volume.
//
// The reversed-schedule EQUIVALENCE over generated state-machine sequences is
// the schedule-reversed.gl pair of the #1349 harness.
//
// Every test SKIPs without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"
#include "PropertyTests/RendererAttachedTest.h"
#include "StateMachine/StateMachineCoverage.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphOutOfBand.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kSize = 256;

        [[nodiscard]] RenderGraph* ActiveGraph()
        {
            return const_cast<RenderGraph*>(RenderGraphDebugRuntime::GetActiveGraph().Raw());
        }

        [[nodiscard]] std::string Describe(const TArray64<RenderGraph::Hazard>& hazards)
        {
            std::string out;
            for (const auto& hazard : hazards)
                out += "  " + hazard.Message.ToStdString() + "\n";
            return out.empty() ? "  (none)\n" : out;
        }

        [[nodiscard]] bool HasHazard(const TArray64<RenderGraph::Hazard>& hazards, RenderGraph::HazardKind kind,
                                     std::string_view boundary)
        {
            return std::ranges::any_of(hazards, [&](const RenderGraph::Hazard& hazard)
                                       { return hazard.Kind == kind && hazard.Resource.ToView() == boundary; });
        }

        [[nodiscard]] bool LedgerSaw(const RenderGraph& graph, std::string_view boundary, RGOutOfBandAccess access,
                                     RGLedgerPhase phase = RGLedgerPhase::Graph)
        {
            const auto entries = const_cast<RenderGraph&>(graph).GetOutOfBandLedger().GetEntries();
            return std::ranges::any_of(entries, [&](const RGOutOfBandLedger::Entry& entry)
                                       { return entry.Boundary.ToView() == boundary && entry.Access == access && entry.Phase == phase; });
        }

        // Fraction of pixels whose largest channel delta exceeds `threshold`.
        [[nodiscard]] f32 FractionChanged(const std::vector<u8>& a, const std::vector<u8>& b, u8 threshold)
        {
            const std::size_t n = std::min(a.size(), b.size());
            if (n == 0)
                return 1.0f;
            std::size_t changed = 0;
            std::size_t pixels = 0;
            for (std::size_t i = 0; i + 3 < n; i += 4)
            {
                for (std::size_t c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])) > threshold)
                    {
                        ++changed;
                        break;
                    }
                }
                ++pixels;
            }
            return pixels ? static_cast<f32>(changed) / static_cast<f32>(pixels) : 1.0f;
        }

        class ScopedOmission
        {
          public:
            explicit ScopedOmission(std::string spec)
            {
                RGOutOfBand::SetOmittedDeclarationFault(std::move(spec));
                // The fault acts in Setup: make the next frame compile.
                if (RenderGraph* graph = ActiveGraph())
                    graph->InvalidateBuildFrameGraphCache();
            }
            ~ScopedOmission()
            {
                RGOutOfBand::SetOmittedDeclarationFault(std::nullopt);
                if (RenderGraph* graph = ActiveGraph())
                    graph->InvalidateBuildFrameGraphCache();
            }
            ScopedOmission(const ScopedOmission&) = delete;
            ScopedOmission& operator=(const ScopedOmission&) = delete;
        };

        class ScopedReverseTieBreak
        {
          public:
            ScopedReverseTieBreak() : m_Previous(Levers::RenderGraphReverseTieBreak())
            {
                Levers::SetRenderGraphReverseTieBreak(true);
            }
            ~ScopedReverseTieBreak()
            {
                Levers::SetRenderGraphReverseTieBreak(m_Previous);
            }
            ScopedReverseTieBreak(const ScopedReverseTieBreak&) = delete;
            ScopedReverseTieBreak& operator=(const ScopedReverseTieBreak&) = delete;

          private:
            bool m_Previous;
        };
    } // namespace

    // -------------------------------------------------------------------------
    // A lit, foggy scene with a GPU-culled instanced field behind an occluder,
    // so one frame exercises the froxel fog publication, the Forward+ cluster
    // publication, the scene bucket, the DDGI republish and the occlusion
    // pyramid in every phase.
    // -------------------------------------------------------------------------
    class RenderGraphOutOfBandSchedule : public RendererAttachedTest
    {
      protected:
        void SetUp() override
        {
            RendererAttachedTest::SetUp();
            if (::testing::Test::IsSkipped())
                return;
            // The fixture's snapshot covers neither of these.
            m_SavedFog = Renderer3D::GetFogSettings();
            m_SavedHZB = Renderer3D::IsHZBOcclusionCullingEnabled();
            m_SavedPost = Renderer3D::GetPostProcessSettings();
        }

        void TearDown() override
        {
            if (!::testing::Test::IsSkipped())
            {
                Renderer3D::GetFogSettings() = m_SavedFog;
                Renderer3D::EnableHZBOcclusionCulling(m_SavedHZB);
                Renderer3D::GetPostProcessSettings() = m_SavedPost;
                Renderer3D::ApplyRendererSettings();
            }
            RendererAttachedTest::TearDown();
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();

            Entity camera = scene.CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 10.0f };
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            const std::array<std::pair<glm::vec3, glm::vec3>, 3> lights = { {
                { { -4.0f, 2.0f, 4.0f }, { 1.0f, 0.2f, 0.2f } },
                { { 0.0f, 2.0f, 4.0f }, { 0.2f, 1.0f, 0.2f } },
                { { 4.0f, 2.0f, 4.0f }, { 0.2f, 0.2f, 1.0f } },
            } };
            for (const auto& [position, color] : lights)
            {
                Entity light = scene.CreateEntity("Light");
                light.GetComponent<TransformComponent>().Translation = position;
                auto& point = light.AddComponent<PointLightComponent>();
                point.m_Color = color;
                point.m_Intensity = 40.0f;
                point.m_Range = 14.0f;
            }

            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            Entity wall = scene.CreateEntity("Occluder");
            wall.AddComponent<MeshComponent>(cube->GetMeshSource());
            auto& wallTransform = wall.GetComponent<TransformComponent>();
            wallTransform.Translation = { 0.0f, 0.0f, 2.0f };
            wallTransform.Scale = { 5.0f, 5.0f, 0.5f };

            // Over the GPU-cull threshold (1024), so the field routes through
            // SubmitGPUCulledInstanced and the two-phase occlusion passes.
            Entity field = scene.CreateEntity("InstancedField");
            auto& instanced = field.AddComponent<InstancedMeshComponent>();
            instanced.MeshSource = cube->GetMeshSource();
            instanced.CastShadows = false;
            constexpr i32 kGrid = 34;
            instanced.Instances.Reserve(static_cast<sizet>(kGrid) * kGrid);
            for (i32 gy = 0; gy < kGrid; ++gy)
            {
                for (i32 gx = 0; gx < kGrid; ++gx)
                {
                    InstanceData instance;
                    instance.Transform = glm::scale(
                        glm::translate(glm::mat4(1.0f),
                                       glm::vec3((static_cast<f32>(gx) / (kGrid - 1) - 0.5f) * 11.0f,
                                                 (static_cast<f32>(gy) / (kGrid - 1) - 0.5f) * 11.0f, -20.0f)),
                        glm::vec3(0.15f));
                    instance.PrevTransform = instance.Transform;
                    instanced.Instances.Add(instance);
                }
            }

            EnableRendering(kSize, kSize);
        }

        // Global renderer state is configured per test, NOT in BuildScene:
        // SetUp snapshots it after BuildScene has run, so anything set there
        // would be "restored" into every later test in the process.
        void UsePath(RenderingPath path, AOTechnique ao = AOTechnique::GTAO, bool aoDebugView = false)
        {
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.EnableVolumetric = true;
            fog.Density = 0.05f;
            fog.Start = 0.0f;
            fog.End = 60.0f;
            fog.HeightFalloff = 0.0f;
            fog.MaxOpacity = 0.95f;
            fog.Color = { 0.35f, 0.38f, 0.42f };
            fog.EnableScattering = false;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;

            // `aoDebugView`: AOApplyPass, the prepared consumer of the SSAO
            // parameter block, runs only for the AO debug view since #1336
            // (the ambient term applies AO inside the lighting shaders).
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            // ApplyRendererSettings re-applies this flag, so it is set here.
            settings.HZBOcclusionCullingEnabled = m_HZBOcclusion;
            settings.ShowGrid = false;
            settings.ShowLightGizmos = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowCameraFrustums = false;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.ActiveAOTechnique = ao;
            post.SSAOEnabled = ao == AOTechnique::SSAO;
            post.GTAOEnabled = ao == AOTechnique::GTAO;
            post.SSAODebugView = aoDebugView && ao == AOTechnique::SSAO;
            Renderer3D::ApplyRendererSettings();
        }

        void SetHZBOcclusion(bool enabled)
        {
            m_HZBOcclusion = enabled;
            Renderer3D::GetRendererSettings().HZBOcclusionCullingEnabled = enabled;
            Renderer3D::EnableHZBOcclusionCulling(enabled);
        }

        FogSettings m_SavedFog{};
        PostProcessSettings m_SavedPost{};
        bool m_SavedHZB = false;
        bool m_HZBOcclusion = true;
    };

    TEST_F(RenderGraphOutOfBandSchedule, EveryPathRendersWithACleanLedgerInEitherOrder)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(std::string("path ") + std::to_string(static_cast<int>(path)));
            UsePath(path);
            RunFrames(4);
            RenderGraph* graph = ActiveGraph();
            ASSERT_NE(graph, nullptr);

            const auto hazards = graph->ValidateOutOfBandLedger();
            EXPECT_TRUE(hazards.IsEmpty()) << Describe(hazards);
            const auto compiled = graph->ValidateCompiledResourceHazards();
            EXPECT_TRUE(compiled.IsEmpty()) << Describe(compiled);

            // The frame really exercised the boundaries this scene is built
            // for, so "clean" is not "nothing happened".
            EXPECT_TRUE(LedgerSaw(*graph, RGOutOfBandBoundaries::FroxelFogVolume, RGOutOfBandAccess::Write));
            EXPECT_TRUE(LedgerSaw(*graph, RGOutOfBandBoundaries::FroxelFogVolume, RGOutOfBandAccess::Read));
            EXPECT_TRUE(LedgerSaw(*graph, RGOutOfBandBoundaries::SceneOpaqueCommandBucket, RGOutOfBandAccess::Write));
            EXPECT_TRUE(LedgerSaw(*graph, RGOutOfBandBoundaries::DDGIProbeVolume, RGOutOfBandAccess::Write));
            EXPECT_TRUE(LedgerSaw(*graph, RGOutOfBandBoundaries::OcclusionHZB, RGOutOfBandAccess::Write, RGLedgerPhase::Epilogue))
                << "the final pyramid rebuild did not run from the frame-epilogue read";
            EXPECT_TRUE(LedgerSaw(*graph, RGOutOfBandBoundaries::OcclusionHZB, RGOutOfBandAccess::ReadPreviousFrame, RGLedgerPhase::Prologue))
                << "the instance cull did not read the retained pyramid";

            {
                const ScopedReverseTieBreak reverse;
                RunFrames(2);
                const auto reversed = graph->ValidateOutOfBandLedger();
                EXPECT_TRUE(reversed.IsEmpty()) << "with the tie-break reversed:\n"
                                                << Describe(reversed);
            }
            RunFrames(1);
        }
    }

    TEST_F(RenderGraphOutOfBandSchedule, ScheduleNamesEverySideEffectAndTheEpilogueRead)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(std::string("path ") + std::to_string(static_cast<int>(path)));
            UsePath(path);
            RunFrames(3);
            RenderGraph* graph = ActiveGraph();
            ASSERT_NE(graph, nullptr);
            const auto schedule = nlohmann::json::parse(graph->ExportOutOfBandScheduleJson());

            for (const auto& pass : schedule["passes"])
            {
                if (!pass["sideEffects"].empty())
                    EXPECT_NE(pass["sideEffectReason"], "UNDOCUMENTED")
                        << pass["name"] << " has a side effect nobody documented in RenderGraphOutOfBand.cpp";
                for (const auto& declaration : pass["outOfBand"])
                    EXPECT_NE(graph->FindOutOfBandBoundary(declaration["boundary"].get<std::string>()), nullptr)
                        << pass["name"] << " declares an unregistered boundary " << declaration["boundary"];
            }

            const std::string expectedDepth(path == RenderingPath::Deferred ? ResourceNames::GBufferResolved
                                                                            : ResourceNames::SceneColor);
            ASSERT_EQ(schedule["frameEpilogueReads"].size(), 1u);
            EXPECT_EQ(schedule["frameEpilogueReads"][0]["resource"], expectedDepth);
            EXPECT_EQ(schedule["frameEpilogueReads"][0]["consumer"], "Renderer3D::GenerateOcclusionHZB");
        }

        // With occlusion off nothing reads the depth after the graph.
        SetHZBOcclusion(false);
        RunFrames(1);
        EXPECT_TRUE(ActiveGraph()->GetFrameEpilogueReads().empty());
    }

    // NEGATIVE CONTROL — a CPU publication consumed after its producer. FogPass
    // stops declaring the froxel volume it reads. The ledger must report the
    // undeclared read, and VolumetricFogPass, whose only consumer no longer
    // declares it, is culled: FogPass must see "did not run this frame"
    // rather than the last volume it integrated, so the frame changes.
    TEST_F(RenderGraphOutOfBandSchedule, OmittedFroxelFogPublicationIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        UsePath(RenderingPath::ForwardPlus);
        RunFrames(4);
        std::vector<u8> clean;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(clean, width, height));
        ASSERT_TRUE(ActiveGraph()->ValidateOutOfBandLedger().IsEmpty());

        std::vector<u8> faulty;
        TArray64<RenderGraph::Hazard> hazards;
        {
            const ScopedOmission fault("FogPass/FroxelFogVolume");
            RunFrames(4);
            hazards = ActiveGraph()->ValidateOutOfBandLedger();
            ASSERT_TRUE(ReadbackComposite(faulty, width, height));
            const auto culled = ActiveGraph()->GetCulledPasses();
            EXPECT_TRUE(std::ranges::any_of(culled, [](const FString& name)
                                            { return name.ToView() == "VolumetricFogPass"; }))
                << "VolumetricFogPass should have no consumer left";
        }

        const bool caught = HasHazard(hazards, RenderGraph::HazardKind::UndeclaredOutOfBandAccess,
                                      RGOutOfBandBoundaries::FroxelFogVolume);
        EXPECT_TRUE(caught) << Describe(hazards);
        // A stale volume would render the same as the clean frame.
        EXPECT_GT(FractionChanged(clean, faulty, 8u), 0.05f)
            << "the frame did not change: FogPass composited a volume VolumetricFogPass never produced this frame";
        if (caught)
            StateMachine::Coverage::RecordComparison("omitted-out-of-band-publication");

        RunFrames(2);
        EXPECT_TRUE(ActiveGraph()->ValidateOutOfBandLedger().IsEmpty()) << "the fault leaked past its scope";
    }

    // NEGATIVE CONTROL — a prepared consumer. AOApplyPass (whole-pass
    // recording) binds the SSAO parameter block SSAOPass publishes after its
    // own recording. Drop AOApply's declaration and the ledger must report the
    // read its preparation makes.
    TEST_F(RenderGraphOutOfBandSchedule, OmittedPreparedConsumerDeclarationIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        UsePath(RenderingPath::Deferred, AOTechnique::SSAO, /*aoDebugView*/ true);
        RunFrames(3);
        ASSERT_TRUE(LedgerSaw(*ActiveGraph(), RGOutOfBandBoundaries::SSAOParameters, RGOutOfBandAccess::Read))
            << "AOApplyPass did not run: the control would be vacuous";
        ASSERT_TRUE(ActiveGraph()->ValidateOutOfBandLedger().IsEmpty());

        TArray64<RenderGraph::Hazard> hazards;
        {
            const ScopedOmission fault("AOApplyPass/SSAOParameters");
            RunFrames(2);
            hazards = ActiveGraph()->ValidateOutOfBandLedger();
        }
        const bool caught = HasHazard(hazards, RenderGraph::HazardKind::UndeclaredOutOfBandAccess,
                                      RGOutOfBandBoundaries::SSAOParameters);
        EXPECT_TRUE(caught) << Describe(hazards);
        if (caught)
            StateMachine::Coverage::RecordComparison("omitted-prepared-consumer-declaration");
    }

    // NEGATIVE CONTROL — an in-place occlusion-pyramid rebuild nobody declared.
    // The two-phase occlusion pass rebuilds the retained pyramid from partial
    // depth: GPUDrivenOcclusionPass, or its forward-prepass share when an AO
    // prepass runs. With the rebuilding pass's declaration dropped, the ledger
    // must report the write.
    TEST_F(RenderGraphOutOfBandSchedule, OmittedOcclusionPyramidRebuildIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        UsePath(RenderingPath::Forward);
        RunFrames(4);
        std::string rebuilder;
        for (const auto& entry : ActiveGraph()->GetOutOfBandLedger().GetEntries())
        {
            if (entry.Boundary.ToView() == RGOutOfBandBoundaries::OcclusionHZB && entry.Access == RGOutOfBandAccess::Write &&
                entry.Phase == RGLedgerPhase::Graph)
            {
                rebuilder = entry.Pass.ToStdString();
            }
        }
        ASSERT_FALSE(rebuilder.empty()) << "no pass rebuilt the pyramid mid-frame: the control would be vacuous";
        ASSERT_TRUE(ActiveGraph()->ValidateOutOfBandLedger().IsEmpty());

        TArray64<RenderGraph::Hazard> hazards;
        {
            const ScopedOmission fault(rebuilder + "/OcclusionHZB");
            RunFrames(2);
            hazards = ActiveGraph()->ValidateOutOfBandLedger();
        }
        const bool caught = HasHazard(hazards, RenderGraph::HazardKind::UndeclaredOutOfBandAccess,
                                      RGOutOfBandBoundaries::OcclusionHZB);
        EXPECT_TRUE(caught) << Describe(hazards);
        if (caught)
            StateMachine::Coverage::RecordComparison("omitted-occlusion-pyramid-rebuild");
    }
} // namespace OloEngine::Tests
