// OLO_TEST_LAYER: integration

// =============================================================================
// Renderer state-machine harness, the device half (issue #1349).
//
// Generated and regression traces over the real renderer: after every
// operation each Required GL pair of the manifest renders the same state two
// ways and must agree, and at the end of every trace the state the sequence
// reached must render like the same configuration reached directly. See
// RendererStateMachineManifest.h for the pairs and their criteria, and
// RendererStateMachineHarness.h for what the harness substitutes.
//
//   * GeneratedSequencesHoldEveryPair: the smoke seeds and the regression
//     corpus (or --olo-state-machine-seeds / --olo-state-machine-length for an
//     unattended long run). Proves its own premises first: the scene renders,
//     the 40-mesh model contributes pixels, and each lever has work to do.
//   * ReloadsAreIdentityOperations: a shader reload and a scene reload leave
//     the frame bit-identical, on every path.
//   * TemporalBeautyMatchesInDistribution: with TAA on, the sequence-reached
//     and directly-reached beauty frames converge to the same distribution.
//     The stochastic half of the exact/distribution split.
//   * ReplayTraceFromCommandLine: --olo-state-machine-replay=<file>.
//
// Evidence PNGs: StateMachine_GL_<Path>.png, the canonical scene on each path.
// The backend is in the name on purpose: the Vulkan rows of the manifest are
// live-only and are not produced here.
//
// Every test SKIPs without a GL 4.6 context, and the run-end
// [ STATE MACHINE ] report says so.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererStateMachineHarness.h"
#include "RendererStateMachineManifest.h"
#include "StateMachineCoverage.h"
#include "TestOptions.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cstring>
#include <filesystem>
#include <iostream>

namespace OloEngine::Tests::StateMachine
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] const char* PathName(u32 path)
        {
            switch (path)
            {
                case 0:
                    return "Forward";
                case 1:
                    return "ForwardPlus";
                default:
                    return "Deferred";
            }
        }

        void WritePng(const std::string& fileName, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            ASSERT_EQ(rgba.size(), static_cast<sizet>(width) * height * 4u) << fileName;
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            std::vector<u8> flipped(rgba.size());
            for (u32 y = 0; y < height; ++y)
                std::memcpy(flipped.data() + (static_cast<sizet>(y) * rowBytes),
                            rgba.data() + (static_cast<sizet>(height - 1u - y) * rowBytes), rowBytes);
            const fs::path dir = fs::path(OLO_TEST_EDITOR_ROOT) / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / fileName).string();
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, flipped.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "failed to write evidence PNG " << path;
        }

        [[nodiscard]] u32 DifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size())
                return ~0u;
            u32 count = 0;
            for (sizet i = 0; i + 3u < a.size(); i += 4u)
            {
                if (std::memcmp(&a[i], &b[i], 3u) != 0)
                    ++count;
            }
            return count;
        }

        // Every Required row this test owns must have compared something real:
        // a row that ran only vacuously has not been tested.
        void ExpectOwnedRowsExercised(std::string_view owner)
        {
            for (const PairRow& pair : kPairs)
            {
                if (pair.Status == RowStatus::Required && pair.Owner == owner)
                    EXPECT_GT(Coverage::ComparisonCount(pair.Id), 0u)
                        << pair.Id << " is owned by " << owner << " but never compared anything";
            }
        }
    } // namespace

    class RendererStateMachineEvidence : public RendererStateMachineFixture
    {
      protected:
        // The premises every pair leans on, checked on the canonical scene of
        // one path: the frame is not empty, the 40-mesh model draws (a blank
        // ModelComponent would make serial-vs-parallel compare two frames with
        // no parallel-submitted pixels), and each lever has work to do.
        void ExpectScenePremises(u32 path)
        {
            SCOPED_TRACE(PathName(path));
            ModelConfig config;
            config.Path = path;
            ConfigureDirectly(config);
            RenderFrames(3);
            const FrameCapture withField = CaptureFrame();
            ASSERT_FALSE(withField.Composite.empty());

            // Luminance spread: a flat frame is not a scene.
            u32 lo = 255;
            u32 hi = 0;
            for (sizet i = 0; i + 3u < withField.Composite.size(); i += 4u)
            {
                const u32 l = (withField.Composite[i] + withField.Composite[i + 1u] + withField.Composite[i + 2u]) / 3u;
                lo = std::min(lo, l);
                hi = std::max(hi, l);
            }
            EXPECT_GT(hi - lo, 60u) << "the canonical frame is nearly flat: nothing to compare";

            u32 parallelMeshes = Renderer3D::GetStats().ParallelSubmittedMeshes;
            EXPECT_GE(parallelMeshes, 32u) << "the 40-mesh model did not reach SubmitMeshesParallel's worker branch";

            u32 batched = 0;
            u32 bucketCommands = 0;
            if (auto* geometry = Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry))
            {
                batched = geometry->GetCommandBucket().GetStatistics().BatchedCommands;
                bucketCommands = geometry->GetCommandBucket().GetStatistics().TotalCommands;
                u32 meshPackets = 0;
                u32 instancedPackets = 0;
                u32 otherPackets = 0;
                for (const CommandPacket* packet : geometry->GetCommandBucket().GetSortedCommands())
                {
                    if (packet == nullptr)
                        continue;
                    if (packet->GetCommandType() == CommandType::DrawMesh)
                        ++meshPackets;
                    else if (packet->GetCommandType() == CommandType::DrawMeshInstanced)
                        ++instancedPackets;
                    else
                        ++otherPackets;
                }
                std::cout << "[StateMachine] geometry bucket replays " << meshPackets << " DrawMesh, " << instancedPackets
                          << " DrawMeshInstanced, " << otherPackets << " other\n";
            }
            EXPECT_GT(batched, 0u) << "no DrawMesh packets were auto-batched (the twin MeshField model supplies them): batch-vs-nobatch would be vacuous";

            // The model's own contribution: hide it, re-render, count pixels.
            for (const auto entity : GetScene().GetAllEntitiesWith<ModelComponent>())
                Entity(entity, &GetScene()).GetComponent<ModelComponent>().m_Visible = false;
            RenderFrames(2);
            const FrameCapture withoutField = CaptureFrame();
            for (const auto entity : GetScene().GetAllEntitiesWith<ModelComponent>())
                Entity(entity, &GetScene()).GetComponent<ModelComponent>().m_Visible = true;
            // DifferingPixels reads a size mismatch as "everything differs", so
            // a failed second capture must not pass as a visible model.
            ASSERT_EQ(withoutField.Composite.size(), withField.Composite.size())
                << "the model-hidden capture failed, so the model's contribution cannot be measured";
            const u32 fieldPixels = DifferingPixels(withField.Composite, withoutField.Composite);
            const u32 frame = withField.Width * withField.Height;
            EXPECT_GT(fieldPixels, frame / 100u) << "the model covers " << fieldPixels << " of " << frame
                                                 << " pixels; a pair over it would compare nothing";
            std::cout << "[StateMachine] " << PathName(path) << ": luminance " << lo << ".." << hi << ", model "
                      << fieldPixels << "/" << frame << " px, " << parallelMeshes << " parallel-submitted, " << batched
                      << " batched of " << bucketCommands << " geometry commands, " << withField.Targets.size()
                      << " targets captured; skipped: " << withField.Skipped << "\n";

            WritePng(std::string("StateMachine_GL_") + PathName(path) + ".png", withField.Composite, withField.Width,
                     withField.Height);
        }
    };

    TEST_F(RendererStateMachineEvidence, GeneratedSequencesHoldEveryPair)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (u32 path = 0; path < kPathCount; ++path)
            ExpectScenePremises(path);
        if (::testing::Test::HasFailure())
            return; // every pair below would be measuring an empty premise

        const auto& options = Options();
        std::vector<u64> seeds(kSmokeSeeds.begin(), kSmokeSeeds.end());
        if (!options.StateMachineSeeds.empty())
            seeds = options.StateMachineSeeds;
        u32 passed = 0;
        u32 traces = 0;
        for (const u64 seed : seeds)
        {
            const Trace trace = GenerateTrace(seed, options.StateMachineLength);
            ++traces;
            if (RunTraceAndReport(trace, "seed-" + std::to_string(seed)))
                ++passed;
        }
        if (options.StateMachineSeeds.empty())
        {
            for (const RegressionRow& row : kRegressions)
            {
                std::string error;
                const std::optional<Trace> trace = LoadTraceFile((fs::path(CorpusDirectory()) / row.File).string(), &error);
                ASSERT_TRUE(trace.has_value()) << row.File << ": " << error;
                ++traces;
                if (RunTraceAndReport(*trace, std::string(row.File)))
                    ++passed;
            }
        }
        std::cout << "[StateMachine] " << passed << "/" << traces << " traces held every pair\n";
        ExpectOwnedRowsExercised("RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair");
    }

    TEST_F(RendererStateMachineEvidence, ReloadsAreIdentityOperations)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (u32 path = 0; path < kPathCount; ++path)
        {
            SCOPED_TRACE(PathName(path));
            ModelConfig config;
            config.Path = path;
            config.Features = (1u << static_cast<u32>(FeatureId::Bloom)) | (1u << static_cast<u32>(FeatureId::FXAA));
            config.BlendedMesh = true;
            ConfigureDirectly(config);
            RenderFrames(3);
            const FrameCapture first = CaptureFrame();
            const FrameCapture before = CaptureFrame();
            const std::vector<ControlFloor> controls = MeasureControls(first, before);
            // Without the final image a comparison holds on whatever
            // intermediates were read, and the row would count as executed.
            ASSERT_FALSE(before.Composite.empty()) << "the pre-reload capture has no composite";

            for (const OpKind reload : { OpKind::ShaderReload, OpKind::SceneReload })
            {
                const Op op{ reload, 0u };
                ApplyOp(op);
                RenderFrames(3);
                const FrameCapture after = CaptureFrame();
                ASSERT_FALSE(after.Composite.empty()) << ToString(op) << ": the post-reload capture has no composite";
                const Comparison comparison = CompareCaptures(before, after, controls);
                Coverage::RecordComparison("fresh-vs-reloaded.gl", comparison.AnyDistributionFallback);
                EXPECT_TRUE(comparison.Held) << ToString(op) << " changed the frame:\n"
                                             << comparison.Describe();
            }
        }
        ExpectOwnedRowsExercised("RendererStateMachineEvidence.ReloadsAreIdentityOperations");
    }

    // TAA accumulates, so the beauty of two equivalent executions converges by
    // different paths and is never bit-identical. The claim is distributional:
    // after convergence, the sequence-reached frame and the directly reached
    // one have the same channel means and luminance histogram, within twice
    // what two converged frames of ONE execution differ by.
    TEST_F(RendererStateMachineEvidence, TemporalBeautyMatchesInDistribution)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        constexpr u32 kConverge = 24u;
        m_BaselinePost.TAAEnabled = true;

        const Trace trace = GenerateTrace(kSmokeSeeds[0], 10u);
        ConfigureDirectly(trace.Initial);
        RenderFrames(kConverge);
        for (const Op& op : trace.Ops)
        {
            ApplyOp(op);
            RenderFrames(3);
        }
        RenderFrames(kConverge);

        // Premise: TAA is actually accumulating, with a valid surface history.
        bool taaHistoryValid = false;
        if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph())
        {
            for (const auto& history : graph->GetTemporalHistoryRegistry().Snapshot())
                taaHistoryValid = taaHistoryValid || (history.Key.Effect == TemporalHistoryEffect::TAA && history.Valid);
        }
        ASSERT_TRUE(taaHistoryValid) << "TAA has no valid history: the distributional comparison would be of two "
                                        "un-accumulated frames";

        const FrameCapture sequenceFirst = CaptureFrame();
        const FrameCapture sequence = CaptureFrame();
        const ModelConfig reached = CurrentConfig();
        ConfigureDirectly(reached);
        RenderFrames(kConverge);
        const FrameCapture freshFirst = CaptureFrame();
        const FrameCapture fresh = CaptureFrame();

        // Every target at distribution level, whatever its controls say: the
        // claim here is the stochastic one.
        std::vector<ControlFloor> controls = MeasureControls(sequenceFirst, sequence);
        const std::vector<ControlFloor> freshControls = MeasureControls(freshFirst, fresh);
        for (ControlFloor& control : controls)
        {
            control.Exact = false;
            if (const auto it = std::ranges::find(freshControls, control.Name, &ControlFloor::Name); it != freshControls.end())
            {
                control.MeanShift = std::max(control.MeanShift, it->MeanShift);
                control.HistogramL1 = std::max(control.HistogramL1, it->HistogramL1);
            }
        }
        ASSERT_FALSE(sequence.Composite.empty()) << "the sequence-reached capture has no composite";
        ASSERT_FALSE(fresh.Composite.empty()) << "the directly configured capture has no composite";
        const Comparison comparison = CompareCaptures(sequence, fresh, controls);
        Coverage::RecordComparison("temporal-fresh-vs-sequence.gl", true);
        EXPECT_TRUE(comparison.Held) << "TAA beauty after the sequence and after a direct configure differ in "
                                        "distribution ("
                                     << DescribeConfig(reached) << "):\n"
                                     << comparison.Describe();
        ExpectOwnedRowsExercised("RendererStateMachineEvidence.TemporalBeautyMatchesInDistribution");
    }

    // serial-vs-parallel.gl compares images, and none of the tracked targets
    // holds velocity, so a branch that dropped a moving model's motion history
    // held there. Model::DrawParallel supplies no history: DrawMesh (the serial
    // branch) looked it up, the parallel workers aliased prev = current, and a
    // moving model had velocity below the 32-mesh threshold and none above it.
    // This pins the packets themselves, on both branches.
    TEST_F(RendererStateMachineEvidence, MovingModelKeepsItsMotionHistoryOnBothSubmissionBranches)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ConfigureDirectly(ModelConfig{});

        Entity field;
        for (const auto entity : GetScene().GetAllEntitiesWith<ModelComponent>())
        {
            if (Entity candidate(entity, &GetScene()); candidate.GetComponent<TagComponent>().Tag == "MeshField")
                field = candidate;
        }
        ASSERT_TRUE(field) << "the canonical scene has no MeshField model";
        const i32 fieldId = static_cast<i32>(std::to_underlying(static_cast<entt::entity>(field)));
        auto& translation = field.GetComponent<TransformComponent>().Translation;
        const glm::vec3 home = translation;
        constexpr f32 kStep = 0.25f;

        // The bucket's own batching would fold these DrawMesh packets into
        // instanced draws with the twin model; the history lives on DrawMesh.
        CommandBucket* bucket = nullptr;
        if (auto* geometry = Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry))
            bucket = &geometry->GetCommandBucket();
        ASSERT_NE(bucket, nullptr);
        auto config = bucket->GetConfig();
        config.EnableBatching = false;
        bucket->SetConfig(config);

        const bool savedSerial = Levers::SerialMeshSubmission();
        for (const bool serial : { false, true })
        {
            SCOPED_TRACE(serial ? "serial branch" : "parallel branch");
            Levers::SetSerialMeshSubmission(serial);
            translation = home;
            RenderFrames(2);
            translation = home + glm::vec3(kStep, 0.0f, 0.0f);
            RenderFrames(1);
            if (serial)
                EXPECT_EQ(Renderer3D::GetStats().ParallelSubmittedMeshes, 0u) << "the lever did not force the serial branch";
            else
                EXPECT_GE(Renderer3D::GetStats().ParallelSubmittedMeshes, 32u) << "the model did not reach the worker branch";

            u32 packets = 0;
            u32 moving = 0;
            for (const CommandPacket* packet : bucket->GetSortedCommands())
            {
                if (packet == nullptr || packet->GetCommandType() != CommandType::DrawMesh)
                    continue;
                const auto* cmd = packet->GetCommandData<DrawMeshCommand>();
                if (cmd->entityID != fieldId)
                    continue;
                ++packets;
                // Scale 1.2 does not touch the translation column, so the
                // frame-to-frame step shows up there unscaled.
                const f32 step = cmd->transform[3].x - cmd->prevTransform[3].x;
                if (std::abs(step - kStep) < 1e-4f)
                    ++moving;
            }
            EXPECT_GE(packets, 32u) << "fewer MeshField packets than the parallel threshold";
            EXPECT_EQ(moving, packets) << moving << " of " << packets
                                       << " MeshField packets carry the model's step in their previous transform";
        }
        Levers::SetSerialMeshSubmission(savedSerial);
        translation = home;
        config.EnableBatching = true;
        bucket->SetConfig(config);
    }

    TEST_F(RendererStateMachineEvidence, ReplayTraceFromCommandLine)
    {
        const std::string& path = Options().StateMachineReplay;
        if (path.empty())
            GTEST_SKIP() << "no --olo-state-machine-replay=<trace> given; this test only replays on request";
        OLO_ENSURE_GPU_OR_SKIP();
        std::string error;
        const std::optional<Trace> trace = LoadTraceFile(path, &error);
        ASSERT_TRUE(trace.has_value()) << error;
        std::cout << "[StateMachine] replaying " << path << ":\n"
                  << Serialize(*trace);
        EXPECT_TRUE(RunTraceAndReport(*trace, fs::path(path).stem().string()));
    }
} // namespace OloEngine::Tests::StateMachine
