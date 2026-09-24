// OLO_TEST_LAYER: L5

// =============================================================================
// Renderer state-machine harness, the CPU half (issue #1349). Runs in every
// job, including hosted runners with no GPU.
//
//   * The harness itself: the trace language, the generator's portability,
//     trace files, the minimiser, the capture comparison and the coverage
//     banner. A harness that is itself wrong reports green for the wrong
//     reason, so each piece is pinned against an answer known in advance.
//   * The manifest: every Required row names a registered test, every
//     Prerequisite names an issue, the corpus parses.
//   * Policy that needs no device: the render-graph declaration cache over
//     generated gate sequences (cached vs forced rebuild), transient alias
//     plans against an independent lifetime model, and blended draw order under
//     batching and parallel submission. Each has a negative control that must
//     be caught.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderingTestUtils.h"
#include "RendererStateMachineHarness.h"
#include "RendererStateMachineManifest.h"
#include "StateMachineCoverage.h"
#include "StateMachineTrace.h"
#include "TraceMinimizer.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphDeclarationKey.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RenderGraphTransientPlanner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace OloEngine::Tests::StateMachine
{
    namespace
    {
        namespace fs = std::filesystem;

        // RAII for a boolean lever (the fault levers in particular).
        class ScopedFault
        {
          public:
            ScopedFault(bool (*get)(), void (*set)(bool)) : m_Set(set), m_Previous(get())
            {
                m_Set(true);
            }
            ~ScopedFault()
            {
                m_Set(m_Previous);
            }
            ScopedFault(const ScopedFault&) = delete;
            ScopedFault& operator=(const ScopedFault&) = delete;

          private:
            void (*m_Set)(bool);
            bool m_Previous;
        };

        [[nodiscard]] std::set<std::string> RegisteredTests()
        {
            std::set<std::string> names;
            const auto* unit = ::testing::UnitTest::GetInstance();
            for (int s = 0; s < unit->total_test_suite_count(); ++s)
            {
                const auto* suite = unit->GetTestSuite(s);
                for (int t = 0; t < suite->total_test_count(); ++t)
                    names.insert(std::string(suite->name()) + "." + suite->GetTestInfo(t)->name());
            }
            return names;
        }

        [[nodiscard]] bool IsIssueReference(std::string_view text)
        {
            if (text.size() < 2u || text.front() != '#')
                return false;
            return std::ranges::all_of(text.substr(1), [](char c)
                                       { return c >= '0' && c <= '9'; });
        }

        [[nodiscard]] TargetCapture MakeTarget(std::string name, u32 width, u32 height, f32 value)
        {
            TargetCapture target;
            target.Name = std::move(name);
            target.PinnedPass = "Pass";
            target.Width = width;
            target.Height = height;
            target.Texels.assign(static_cast<sizet>(width) * height * 4u, value);
            return target;
        }
    } // namespace

    // =========================================================================
    // The trace language
    // =========================================================================

    // Seed 1349's first twelve operations, computed by an independent Python
    // port of GenerateTrace (SplitMix64, Lemire's bounded draw, the weight
    // table). A seed printed in a failure report must name the same trace on
    // MSVC, clang-cl and libstdc++; a std distribution would not.
    TEST(RendererStateMachineTrace, GenerationIsDeterministicAndPortable)
    {
        const std::vector<std::string> expected{ "path deferred",
                                                 "entity-churn add",
                                                 "history-advance",
                                                 "msaa 4",
                                                 "fence-drain",
                                                 "msaa 4",
                                                 "feature vignette-grading off",
                                                 "feature bloom off",
                                                 "entity-churn remove",
                                                 "feature gtao-denoise off",
                                                 "feature gtao-denoise on",
                                                 "resize 320x180" };
        const Trace trace = GenerateTrace(1349u, 12u);
        ASSERT_EQ(trace.Ops.size(), expected.size());
        for (sizet i = 0; i < expected.size(); ++i)
            EXPECT_EQ(ToString(trace.Ops[i]), expected[i]) << "op " << i;

        EXPECT_EQ(GenerateTrace(1349u, 12u).Ops, trace.Ops) << "the same seed must give the same trace";
        EXPECT_NE(GenerateTrace(1350u, 12u).Ops, trace.Ops) << "neighbouring seeds should differ";
        const Trace longer = GenerateTrace(1349u, 20u);
        EXPECT_TRUE(std::equal(trace.Ops.begin(), trace.Ops.end(), longer.Ops.begin()))
            << "a longer trace from the same seed must extend, not reshuffle, the shorter one";
    }

    TEST(RendererStateMachineTrace, SerializeThenParseIsTheIdentity)
    {
        for (u64 seed = 0; seed < 64u; ++seed)
        {
            Trace trace = GenerateTrace(seed, 16u);
            trace.Initial = Trace{}.Initial;
            trace.Initial.Path = static_cast<u32>(seed % kPathCount);
            trace.Initial.Features = static_cast<u32>(seed) & 0x2Du;
            trace.Initial.BlendedMesh = (seed % 2u) == 0u;
            trace.Initial.DoubleBuffering = (seed % 3u) != 0u;
            const std::string text = Serialize(trace);
            std::string error;
            const std::optional<Trace> parsed = ParseTrace(text, &error);
            ASSERT_TRUE(parsed.has_value()) << error << "\n"
                                            << text;
            EXPECT_EQ(parsed->Seed, trace.Seed);
            EXPECT_EQ(parsed->Initial, trace.Initial) << text;
            EXPECT_EQ(parsed->Ops, trace.Ops) << text;
            EXPECT_EQ(Serialize(*parsed), text);
        }
    }

    TEST(RendererStateMachineTrace, MalformedTracesAreRejectedWithALineNumber)
    {
        const std::vector<std::pair<std::string, std::string>> cases{
            { "", "empty" },
            { "olo-state-machine-trace 2\n", "line 1" },
            { "olo-state-machine-trace 1\nop resize 1x1\n", "line 2" },
            { "olo-state-machine-trace 1\nop path sideways\n", "line 2" },
            { "olo-state-machine-trace 1\nop feature bloom maybe\n", "line 2" },
            { "olo-state-machine-trace 1\nop shader-reload now\n", "line 2" },
            { "olo-state-machine-trace 1\nseed x\n", "line 2" },
            { "olo-state-machine-trace 1\ninitial path=forward size=7x7\n", "line 2" },
            { "olo-state-machine-trace 1\n# fine\n\nop camera-move 9\n", "line 4" },
            { "olo-state-machine-trace 1\nwobble\n", "line 2" },
        };
        for (const auto& [text, where] : cases)
        {
            std::string error;
            EXPECT_FALSE(ParseTrace(text, &error).has_value()) << "accepted:\n"
                                                               << text;
            EXPECT_NE(error.find(where), std::string::npos) << "error '" << error << "' should name " << where;
        }
    }

    // Totality is what makes every subsequence of a trace a valid trace, and
    // so what makes ddmin sound. Every operation, in every configuration the
    // generator can reach, must land on an in-range configuration, and the
    // hidden-state operations must leave the configuration alone.
    TEST(RendererStateMachineTrace, EveryOperationIsTotal)
    {
        std::vector<Op> everyOp;
        for (u32 kind = 0; kind < static_cast<u32>(OpKind::Count); ++kind)
        {
            for (u32 arg = 0; arg < 12u; ++arg)
            {
                const Op op{ static_cast<OpKind>(kind), arg };
                if (ParseOp(ToString(op)) == op)
                    everyOp.push_back(op);
            }
        }
        ASSERT_GE(everyOp.size(), static_cast<sizet>(OpKind::Count));

        for (u64 seed = 0; seed < 32u; ++seed)
        {
            ModelConfig config = GenerateTrace(seed, 24u).Final();
            for (const Op& op : everyOp)
            {
                const ModelConfig next = Apply(config, op);
                EXPECT_LT(next.Path, kPathCount);
                EXPECT_LT(next.SizeIndex, kSizes.size());
                EXPECT_LT(next.MsaaIndex, kMsaaSamples.size());
                EXPECT_LT(next.UpscaleIndex, kUpscaleModes.size());
                EXPECT_LT(next.Pose, kPoseCount);
                EXPECT_EQ(next.Features >> static_cast<u32>(FeatureId::Count), 0u);
                switch (op.Kind)
                {
                    case OpKind::ShaderReload:
                    case OpKind::SceneReload:
                    case OpKind::FenceDrain:
                    case OpKind::SceneSwap:
                    case OpKind::HistoryAdvance:
                    case OpKind::PoolTrim:
                        EXPECT_EQ(next, config) << ToString(op) << " must only move hidden state";
                        break;
                    default:
                        break;
                }
                EXPECT_EQ(Apply(next, op), next) << ToString(op) << " applied twice must equal applied once";
            }
        }
    }

    // =========================================================================
    // The manifest
    // =========================================================================

    TEST(RendererStateMachineManifest, EveryOperationKindHasExactlyOneRow)
    {
        std::set<std::string> fromLanguage;
        for (u32 kind = 0; kind < static_cast<u32>(OpKind::Count); ++kind)
        {
            const std::string text = ToString(Op{ static_cast<OpKind>(kind), 0u });
            fromLanguage.insert(text.substr(0, text.find(' ')));
        }
        std::set<std::string> fromManifest;
        for (const OperationRow& row : kOperations)
        {
            EXPECT_TRUE(fromManifest.insert(std::string(row.Id)).second) << "duplicate operation row " << row.Id;
            EXPECT_EQ(row.Status, RowStatus::Required) << row.Id;
        }
        EXPECT_EQ(fromLanguage, fromManifest)
            << "the manifest's operation rows and the trace language's operations must be the same set";
    }

    TEST(RendererStateMachineManifest, EveryRowIsExecutedBlockedOrExplained)
    {
        const std::set<std::string> registered = RegisteredTests();
        std::set<std::string> ids;
        const auto checkRow = [&registered, &ids](std::string_view id, RowStatus status, std::string_view owner,
                                                  std::string_view note)
        {
            EXPECT_TRUE(ids.insert(std::string(id)).second) << "duplicate row id " << id;
            switch (status)
            {
                case RowStatus::Required:
                    EXPECT_TRUE(registered.contains(std::string(owner)))
                        << id << ": Required, but its owner '" << owner << "' is not a registered test";
                    break;
                case RowStatus::Prerequisite:
                    EXPECT_TRUE(IsIssueReference(owner)) << id << ": a Prerequisite must name the issue that owns it";
                    EXPECT_FALSE(note.empty()) << id << ": say what is missing";
                    break;
                case RowStatus::LiveOnly:
                    EXPECT_FALSE(note.empty()) << id << ": a LiveOnly row must say why no fixture can run it";
                    break;
            }
        };
        for (const PairRow& pair : kPairs)
            checkRow(pair.Id, pair.Status, pair.Owner, pair.Note);
        for (const NegativeControlRow& control : kNegativeControls)
        {
            checkRow(control.Id, RowStatus::Required, control.Owner, control.CaughtBy);
            // The checks a control names must be real pairs.
            bool namesAPair = false;
            for (const PairRow& pair : kPairs)
                namesAPair = namesAPair || control.CaughtBy.find(pair.Id) != std::string_view::npos;
            EXPECT_TRUE(namesAPair) << control.Id << ": CaughtBy names no pair in the manifest";
        }
    }

    TEST(RendererStateMachineManifest, TheRegressionCorpusParsesAndIsReplayable)
    {
        std::set<std::string> listed;
        for (const RegressionRow& row : kRegressions)
        {
            const fs::path path = fs::path(CorpusDirectory()) / row.File;
            std::string error;
            const std::optional<Trace> trace = LoadTraceFile(path.string(), &error);
            ASSERT_TRUE(trace.has_value()) << row.File << ": " << error;
            EXPECT_FALSE(trace->Ops.empty()) << row.File;
            EXPECT_EQ(trace->Seed, 0u) << row.File << ": a corpus trace is hand-written, not seeded";
            EXPECT_TRUE(IsIssueReference(row.Origin.substr(0, row.Origin.find(' ')))) << row.File;
            listed.insert(std::string(row.File));
        }
        // A trace file nobody lists would never run.
        for (const auto& entry : fs::directory_iterator(CorpusDirectory()))
        {
            if (entry.path().extension() == ".trace")
                EXPECT_TRUE(listed.contains(entry.path().filename().string()))
                    << entry.path().filename().string() << " is in corpus/ but not in kRegressions";
        }
        const std::set<u64> seeds(kSmokeSeeds.begin(), kSmokeSeeds.end());
        EXPECT_EQ(seeds.size(), kSmokeSeeds.size()) << "duplicate smoke seed";
    }

    // =========================================================================
    // The minimiser
    // =========================================================================

    TEST(RendererStateMachineMinimizer, FindsTheTwoElementsThatMatter)
    {
        std::vector<u32> input(40);
        for (u32 i = 0; i < input.size(); ++i)
            input[i] = i;
        const std::function<bool(const std::vector<u32>&)> fails = [](const std::vector<u32>& ops)
        {
            // Fails only when 7 comes before 31, like "enable X, then resize".
            const auto seven = std::ranges::find(ops, 7u);
            const auto thirtyOne = std::ranges::find(ops, 31u);
            return seven != ops.end() && thirtyOne != ops.end() && seven < thirtyOne;
        };
        const auto result = MinimizeTrace<u32>(input, fails, 400u);
        EXPECT_FALSE(result.InputDidNotFail);
        EXPECT_TRUE(result.OneMinimal);
        EXPECT_EQ(result.Trace, (std::vector<u32>{ 7u, 31u }));
        EXPECT_LT(result.Evaluations, 120u) << "ddmin should need far fewer replays than there are subsets";
    }

    TEST(RendererStateMachineMinimizer, IsOneMinimalOnAConjunction)
    {
        const std::vector<u32> input{ 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        const std::function<bool(const std::vector<u32>&)> fails = [](const std::vector<u32>& ops)
        {
            const auto has = [&ops](u32 v)
            { return std::ranges::find(ops, v) != ops.end(); };
            return has(2u) && has(5u) && has(9u);
        };
        const auto result = MinimizeTrace<u32>(input, fails, 400u);
        ASSERT_TRUE(result.OneMinimal);
        EXPECT_EQ(result.Trace, (std::vector<u32>{ 2u, 5u, 9u }));
        for (sizet drop = 0; drop < result.Trace.size(); ++drop)
        {
            std::vector<u32> smaller = result.Trace;
            smaller.erase(smaller.begin() + static_cast<std::ptrdiff_t>(drop));
            EXPECT_FALSE(fails(smaller)) << "dropping element " << drop << " still fails: not 1-minimal";
        }
    }

    TEST(RendererStateMachineMinimizer, ReportsAnExhaustedBudgetAndANonFailingInput)
    {
        std::vector<u32> input(64);
        for (u32 i = 0; i < input.size(); ++i)
            input[i] = i;
        const std::function<bool(const std::vector<u32>&)> fails = [](const std::vector<u32>& ops)
        { return std::ranges::find(ops, 40u) != ops.end() && std::ranges::find(ops, 3u) != ops.end(); };
        const auto starved = MinimizeTrace<u32>(input, fails, 3u);
        EXPECT_FALSE(starved.OneMinimal) << "three replays cannot reach a 1-minimal trace";
        EXPECT_LE(starved.Evaluations, 3u);
        EXPECT_TRUE(fails(starved.Trace)) << "whatever is returned must still fail";

        const std::function<bool(const std::vector<u32>&)> never = [](const std::vector<u32>&)
        { return false; };
        const auto clean = MinimizeTrace<u32>(input, never, 10u);
        EXPECT_TRUE(clean.InputDidNotFail);
        EXPECT_EQ(clean.Trace, input);
        EXPECT_EQ(clean.Evaluations, 1u);
    }

    // =========================================================================
    // Capture comparison (the criteria in the manifest, as code)
    // =========================================================================

    TEST(RendererStateMachineComparison, AnExactTargetCatchesOneTexel)
    {
        FrameCapture a;
        a.Targets.push_back(MakeTarget("SceneColorTexture", 8u, 8u, 0.25f));
        FrameCapture b = a;
        const std::vector<ControlFloor> exact = MeasureControls(a, a);
        ASSERT_EQ(exact.size(), 1u);
        EXPECT_TRUE(exact[0].Exact);
        EXPECT_TRUE(CompareCaptures(a, b, exact).Held);

        b.Targets[0].Texels[17] = 0.2500001f;
        const Comparison comparison = CompareCaptures(a, b, exact);
        EXPECT_FALSE(comparison.Held) << "one texel one ulp off must fail an exact target";
        EXPECT_FALSE(comparison.AnyDistributionFallback);
        ASSERT_EQ(comparison.Targets.size(), 1u);
        EXPECT_EQ(comparison.Targets[0].DifferingTexels, 1u);
    }

    TEST(RendererStateMachineComparison, ANoisyControlFallsBackToDistributionAndSaysSo)
    {
        FrameCapture first;
        // 0.3, not 0.5: 0.5 is exactly a log-luminance bin edge, where any
        // downward nudge changes bin and no upward one does.
        first.Targets.push_back(MakeTarget("AOBuffer", 16u, 16u, 0.3f));
        FrameCapture second = first;
        // Frame-index noise: a few texels move a little, both ways, between
        // two frames of one state.
        for (sizet i = 0; i < second.Targets[0].Texels.size(); i += 37u)
            second.Targets[0].Texels[i] += (i / 37u) % 2u == 0u ? 0.01f : -0.01f;
        const std::vector<ControlFloor> controls = MeasureControls(first, second);
        ASSERT_FALSE(controls[0].Exact);

        FrameCapture candidate = second;
        for (sizet i = 3; i < candidate.Targets[0].Texels.size(); i += 41u)
            candidate.Targets[0].Texels[i] -= 0.01f;
        const Comparison noiseLike = CompareCaptures(second, candidate, controls);
        EXPECT_TRUE(noiseLike.Held) << noiseLike.Describe();
        EXPECT_TRUE(noiseLike.AnyDistributionFallback) << "a fallback must be reported, never hidden";

        // A real difference (a black frame where there was grey) is far
        // outside any noise floor and must still fail at distribution level.
        FrameCapture broken = second;
        std::ranges::fill(broken.Targets[0].Texels, 0.0f);
        EXPECT_FALSE(CompareCaptures(second, broken, controls).Held);
    }

    TEST(RendererStateMachineComparison, ATargetCapturedOnOneSideOnlyFails)
    {
        FrameCapture a;
        a.Targets.push_back(MakeTarget("UIComposite", 4u, 4u, 1.0f));
        a.Targets.push_back(MakeTarget("SSRColorTexture", 4u, 4u, 1.0f));
        FrameCapture b;
        b.Targets.push_back(MakeTarget("UIComposite", 4u, 4u, 1.0f));
        const Comparison comparison = CompareCaptures(a, b, MeasureControls(a, a));
        EXPECT_FALSE(comparison.Held) << "a pass that ran in one execution only is a difference";
        EXPECT_NE(comparison.Describe().find("SSRColorTexture"), std::string::npos);
    }

    // =========================================================================
    // The run-end coverage banner
    // =========================================================================

    TEST(RendererStateMachineCoverage, TheBannerIsQuietUnlessAnOwnerRan)
    {
        std::vector<Coverage::RowReport> rows(2);
        rows[0].Id = "a";
        rows[0].Outcome = Coverage::RowOutcome::NotSelected;
        rows[1].Id = "b";
        rows[1].Outcome = Coverage::RowOutcome::Prerequisite;
        EXPECT_TRUE(Coverage::FormatBanner(rows).empty());
    }

    TEST(RendererStateMachineCoverage, TheBannerNamesSkipsAndUnexercisedRows)
    {
        std::vector<Coverage::RowReport> rows(4);
        rows[0] = { "alias-vs-noalias.gl", Coverage::RowOutcome::Executed, 12u, 3u, 2u, {} };
        rows[1] = { "batch-vs-nobatch.gl", Coverage::RowOutcome::Skipped, 0u, 0u, 0u, "no GL 4.6 context" };
        rows[2] = { "serial-vs-parallel.gl", Coverage::RowOutcome::NotExercised, 0u, 0u, 0u, {} };
        rows[3] = { "isolated-vs-shared-view", Coverage::RowOutcome::Prerequisite, 0u, 0u, 0u, "blocked by #1352" };
        const std::string banner = Coverage::FormatBanner(rows);
        EXPECT_NE(banner.find("[ STATE MACHINE ]"), std::string::npos);
        EXPECT_NE(banner.find("12 comparison(s), 3 at distribution level, 2 vacuous"), std::string::npos) << banner;
        EXPECT_NE(banner.find("SKIPPED"), std::string::npos) << banner;
        EXPECT_NE(banner.find("no GL 4.6 context"), std::string::npos) << banner;
        EXPECT_NE(banner.find("NOT EXERCISED"), std::string::npos) << banner;
        EXPECT_NE(banner.find("blocked by #1352"), std::string::npos) << banner;
        EXPECT_NE(banner.find("1 executed, 1 skipped, 1 not exercised"), std::string::npos) << banner;
    }

    // =========================================================================
    // Policy: the declaration cache over generated gate sequences
    // =========================================================================

    namespace
    {
        // A pass that declares only while its gate is open, the way the
        // bucket-gated passes of #1315 do, and reports that gate to the key the
        // way AppendDeclarationInputs does -- unless told not to.
        class GatedNode : public RenderGraphNode
        {
          public:
            GatedNode(std::string name, std::vector<RGTextureHandle>* handles, u32 index, i32 reads, bool reportGate)
                : m_Handles(handles), m_Index(index), m_Reads(reads), m_ReportGate(reportGate)
            {
                SetName(name);
            }

            void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
            {
                RenderGraphNode::Setup(builder, blackboard);
                if (!Open)
                    return;
                if (m_Reads >= 0)
                {
                    [[maybe_unused]] const auto read = builder.Read((*m_Handles)[static_cast<sizet>(m_Reads)]);
                }
                builder.Write((*m_Handles)[m_Index]);
            }
            void Execute(RGCommandContext& /*context*/) override {}

            [[nodiscard]] bool IsEnabled() const noexcept override
            {
                return Enabled;
            }
            void AppendDeclarationInputs(RGDeclarationKey& key) const override
            {
                if (m_ReportGate)
                    key.Add(Open);
            }

            bool Open = true;
            bool Enabled = true;

          private:
            std::vector<RGTextureHandle>* m_Handles;
            u32 m_Index;
            i32 m_Reads;
            bool m_ReportGate;
        };

        struct GatedGraph
        {
            RenderGraph Graph;
            std::vector<Ref<GatedNode>> Nodes;
            std::vector<RGTextureHandle> Handles;
        };

        // Six passes, each reading some earlier pass's output (chosen by the
        // seed) and writing its own; the last one presents.
        void BuildGatedGraph(GatedGraph& out, u64 seed, i32 unreportedNode)
        {
            constexpr u32 kPasses = 6u;
            SplitMix64 rng(seed);
            out.Handles.resize(kPasses);
            for (u32 i = 0; i < kPasses; ++i)
            {
                const i32 reads = i == 0u ? -1 : static_cast<i32>(rng.Below(i));
                auto node = Ref<GatedNode>::Create("Gated" + std::to_string(i), &out.Handles, i, reads,
                                                   static_cast<i32>(i) != unreportedNode);
                out.Graph.AddNode(node);
                out.Nodes.push_back(node);
            }
            out.Nodes.back()->SetSideEffects(RenderGraphNode::SideEffect::Present);
            out.Graph.SetFinalPass(std::string(out.Nodes.back()->GetName()));
            for (u32 i = 0; i < kPasses; ++i)
            {
                auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "T" + std::to_string(i));
                desc.Format = RGResourceFormat::RGBA16Float;
                desc.Width = 64u;
                desc.Height = 64u;
                out.Handles[i] = out.Graph.DeclareTransientTexture("T" + std::to_string(i), desc);
            }
        }

        // The key the pipeline builds: every pass's enable, readiness and own
        // declaration inputs (RenderPipeline::CaptureDeclarationConfig).
        [[nodiscard]] u64 DeclarationKey(const GatedGraph& graph)
        {
            RGDeclarationKey passStates;
            for (const auto& node : graph.Nodes)
            {
                RGDeclarationKey passKey;
                if (node->IsEnableADeclarationInput())
                    passKey.Add(node->IsEnabled());
                passKey.Add(node->IsReadyForExecution());
                node->AppendDeclarationInputs(passKey);
                passStates.Add(passKey.Get());
            }
            // Never 0: BuildFrameGraph treats a zero key as "do not cache".
            return passStates.Get() | 1u;
        }

        // Walks `steps` random gate/enable flips and, after each, builds the
        // graph from the cache and again from scratch. Returns how many steps
        // the cached plan differed from the rebuilt one, and describes the first.
        u32 CountStaleSteps(u64 seed, u32 steps, i32 unreportedNode, std::string& firstDifference)
        {
            GatedGraph graph;
            BuildGatedGraph(graph, seed, unreportedNode);
            SplitMix64 rng(seed ^ 0xC0FFEEull);
            u32 stale = 0;
            for (u32 step = 0; step < steps; ++step)
            {
                GatedNode& node = *graph.Nodes[rng.Below(static_cast<u32>(graph.Nodes.size()))];
                if (rng.Below(3u) == 0u)
                    node.Enabled = !node.Enabled;
                else
                    node.Open = !node.Open;

                const u64 key = DeclarationKey(graph);
                graph.Graph.BuildFrameGraph(key);
                std::vector<RenderGraph::PlanDigestEntry> cachedEntries;
                const u64 cached = graph.Graph.ComputeCompiledPlanDigest(&cachedEntries);
                graph.Graph.InvalidateBuildFrameGraphCache();
                graph.Graph.BuildFrameGraph(key);
                std::vector<RenderGraph::PlanDigestEntry> rebuiltEntries;
                const u64 rebuilt = graph.Graph.ComputeCompiledPlanDigest(&rebuiltEntries);
                if (cached != rebuilt)
                {
                    if (stale++ == 0u)
                        firstDifference = "step " + std::to_string(step) + ": " + std::string(node.GetName()) +
                                          " open=" + std::to_string(node.Open) + " enabled=" +
                                          std::to_string(node.Enabled);
                }
            }
            return stale;
        }
    } // namespace

    TEST(RendererStateMachinePolicy, CachedGraphMatchesARebuildOverGeneratedSequences)
    {
        u32 comparisons = 0;
        for (u64 seed = 1; seed <= 24u; ++seed)
        {
            std::string first;
            const u32 stale = CountStaleSteps(seed, 40u, -1, first);
            EXPECT_EQ(stale, 0u) << "seed " << seed << ": a cached build differed from a forced rebuild at " << first;
            comparisons += 40u;
        }
        Coverage::RecordComparison("cached-vs-rebuild.cpu");
        EXPECT_EQ(comparisons, 960u);
    }

    // The negative control: one pass branches in Setup() on a gate it does not
    // report. The generated sequences must find a step where only that gate
    // moved and the cache served the stale plan.
    TEST(RendererStateMachinePolicy, UnreportedSetupGateIsCaught)
    {
        u32 caughtSeeds = 0;
        std::string example;
        for (u64 seed = 1; seed <= 24u; ++seed)
        {
            std::string first;
            if (CountStaleSteps(seed, 40u, 2, first) > 0u)
            {
                ++caughtSeeds;
                if (example.empty())
                    example = "seed " + std::to_string(seed) + " " + first;
            }
        }
        EXPECT_GT(caughtSeeds, 12u) << "an unreported Setup() gate went unnoticed in most generated sequences: the "
                                       "cached-vs-rebuild comparison is blind to the #1315 class";
        if (caughtSeeds > 0u)
            Coverage::RecordComparison("stale-declaration-key.cpu");
        std::cout << "[StateMachine] unreported gate caught in " << caughtSeeds << "/24 sequences, first: " << example
                  << "\n";
    }

    // =========================================================================
    // Policy: transient alias plans against an independent lifetime model
    // =========================================================================

    namespace
    {
        struct RandomPlan
        {
            RGTransparentStringMap<RGResourceDesc> Descs;
            std::vector<FString> Order;
            RGTransparentStringMap<TArray64<RGAccessDeclaration>> Accesses;
            RGTransparentStringMap<TArray64<FString>> Extensions;
            RGTransparentStringMap<FString> Versions;
            std::set<std::string> Extracted;
        };

        // Passes touch resources from a few same-descriptor groups, so slots
        // are genuinely contended; some resources are versioned (renames of a
        // base) and some are extracted after the last pass (history sources).
        [[nodiscard]] RandomPlan MakeRandomPlan(u64 seed)
        {
            RandomPlan plan;
            SplitMix64 rng(seed);
            const u32 passes = 6u + rng.Below(10u);
            const u32 resources = 6u + rng.Below(14u);
            for (u32 p = 0; p < passes; ++p)
                plan.Order.emplace_back("P" + std::to_string(p));
            for (u32 r = 0; r < resources; ++r)
            {
                const std::string name = "R" + std::to_string(r);
                auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name);
                desc.Format = rng.Below(2u) == 0u ? RGResourceFormat::RGBA16Float : RGResourceFormat::RGBA8UNorm;
                desc.Width = 64u << rng.Below(2u);
                desc.Height = desc.Width;
                plan.Descs.emplace(name, desc);
                // Each resource is written by one pass and read by up to three later ones.
                const u32 writer = rng.Below(passes);
                plan.Accesses[plan.Order[writer].ToStdString()].Add(RGAccessDeclaration{ FString(name), true });
                const u32 readers = rng.Below(4u);
                for (u32 k = 0; k < readers && writer + 1u < passes; ++k)
                {
                    const u32 reader = writer + 1u + rng.Below(passes - writer - 1u);
                    plan.Accesses[plan.Order[reader].ToStdString()].Add(RGAccessDeclaration{ FString(name), false });
                }
                if (rng.Below(6u) == 0u)
                    plan.Extracted.insert(name);
            }
            // A version: "R0@Pk" renames R0, read by a later pass.
            const std::string version = "R0@V";
            plan.Descs.emplace(version, plan.Descs.at("R0"));
            plan.Versions.emplace(version, FString("R0"));
            plan.Accesses[plan.Order[passes - 1u].ToStdString()].Add(RGAccessDeclaration{ FString(version), false });
            return plan;
        }

        [[nodiscard]] TArray64<RenderGraph::TransientPlanEntry> PlanFor(const RandomPlan& plan)
        {
            const RenderGraphTransientPlanner::PlanInput input{
                .TransientResourceDescs = plan.Descs,
                .ExecutionOrder = std::span<const FString>(plan.Order.data(), plan.Order.size()),
                .PassAccessDeclarations = plan.Accesses,
                .PassLifetimeExtensions = plan.Extensions,
                .VersionAliasTargets = plan.Versions,
                .IsPassReachable = [](std::string_view)
                { return true; },
                .IsExternallyBackedTransientResource = [](std::string_view)
                { return false; },
                .IsExtractedAfterExecution = [&plan](std::string_view name)
                { return plan.Extracted.contains(std::string(name)); },
            };
            return RenderGraphTransientPlanner::ComputePlan(input);
        }

        // The independent model: lifetimes recomputed from the declarations
        // alone (versions folded into their base, extraction extending to the
        // last pass), then every pair of resources sharing an alias slot must
        // have disjoint lifetimes. Returns the overlaps it finds.
        [[nodiscard]] std::vector<std::string> SlotOverlaps(const RandomPlan& plan,
                                                            const TArray64<RenderGraph::TransientPlanEntry>& entries)
        {
            std::map<std::string, std::pair<u32, u32>> lifetime;
            for (u32 p = 0; p < plan.Order.size(); ++p)
            {
                const auto it = plan.Accesses.find(plan.Order[p].ToStdString());
                if (it == plan.Accesses.end())
                    continue;
                for (const auto& access : it->second)
                {
                    std::string name = access.ResourceName.ToStdString();
                    while (plan.Versions.contains(name))
                        name = plan.Versions.at(name).ToStdString();
                    auto [entry, inserted] = lifetime.try_emplace(name, p, p);
                    entry->second.first = std::min(entry->second.first, p);
                    entry->second.second = std::max(entry->second.second, p);
                }
            }
            for (auto& [name, range] : lifetime)
            {
                if (plan.Extracted.contains(name))
                    range.second = static_cast<u32>(plan.Order.size() - 1u);
            }

            std::map<std::pair<std::string, u32>, std::vector<std::string>> slots;
            for (const auto& entry : entries)
            {
                if (entry.WillAllocate)
                    slots[{ entry.AliasGroup.ToStdString(), entry.AliasSlot }].push_back(entry.Resource.ToStdString());
            }
            std::vector<std::string> overlaps;
            for (const auto& [slot, members] : slots)
            {
                for (sizet i = 0; i < members.size(); ++i)
                {
                    for (sizet j = i + 1u; j < members.size(); ++j)
                    {
                        const auto a = lifetime.at(members[i]);
                        const auto b = lifetime.at(members[j]);
                        if (a.first <= b.second && b.first <= a.second)
                            overlaps.push_back(members[i] + "[" + std::to_string(a.first) + "," + std::to_string(a.second) +
                                               "] and " + members[j] + "[" + std::to_string(b.first) + "," +
                                               std::to_string(b.second) + "] share slot " + std::to_string(slot.second));
                    }
                }
            }
            return overlaps;
        }
    } // namespace

    TEST(RendererStateMachinePolicy, AliasPlansNeverShareASlotAcrossOverlappingLifetimes)
    {
        u32 sharedSlots = 0;
        for (u64 seed = 1; seed <= 200u; ++seed)
        {
            const RandomPlan plan = MakeRandomPlan(seed);
            const auto entries = PlanFor(plan);
            const auto overlaps = SlotOverlaps(plan, entries);
            EXPECT_TRUE(overlaps.empty()) << "seed " << seed << ": " << overlaps.front();
            std::map<std::pair<std::string, u32>, u32> users;
            for (const auto& entry : entries)
                if (entry.WillAllocate)
                    ++users[{ entry.AliasGroup.ToStdString(), entry.AliasSlot }];
            sharedSlots += static_cast<u32>(std::ranges::count_if(users, [](const auto& kv)
                                                                  { return kv.second > 1u; }));
        }
        // The property is vacuous unless the planner actually shared slots.
        EXPECT_GT(sharedSlots, 100u) << "the random plans barely alias; the check would pass on anything";
        Coverage::RecordComparison("alias-plan.cpu");
    }

    TEST(RendererStateMachinePolicy, ShortenedLifetimesAreCaughtByTheIndependentModel)
    {
        u32 caught = 0;
        {
            const ScopedFault fault(&Levers::FaultShortenTransientLifetimes, &Levers::SetFaultShortenTransientLifetimes);
            for (u64 seed = 1; seed <= 200u; ++seed)
            {
                const RandomPlan plan = MakeRandomPlan(seed);
                if (!SlotOverlaps(plan, PlanFor(plan)).empty())
                    ++caught;
            }
        }
        EXPECT_GT(caught, 20u) << "the planner fault ended lifetimes a pass early on 200 random plans and the "
                                  "independent model caught it on only "
                               << caught;
        if (caught > 0u)
            Coverage::RecordComparison("alias-lifetime.cpu");
        std::cout << "[StateMachine] shortened lifetimes caught on " << caught << "/200 random plans\n";

        // And with the fault off again, the same plans are clean.
        for (u64 seed = 1; seed <= 200u; ++seed)
        {
            const RandomPlan plan = MakeRandomPlan(seed);
            ASSERT_TRUE(SlotOverlaps(plan, PlanFor(plan)).empty()) << "fault did not restore, seed " << seed;
        }
    }

    // =========================================================================
    // Policy: blended draw order under batching and parallel submission
    // =========================================================================

    namespace
    {
        struct Draw
        {
            u32 Shader = 1;
            u32 Material = 1;
            u32 Depth = 0; // larger is farther
            bool Blended = false;
            i32 Entity = 0;
        };

        [[nodiscard]] std::vector<Draw> MakeDraws(u64 seed)
        {
            SplitMix64 rng(seed);
            std::vector<Draw> draws;
            const u32 count = 24u + rng.Below(40u);
            for (u32 i = 0; i < count; ++i)
            {
                Draw draw;
                draw.Blended = rng.Below(2u) == 0u;
                // Few shaders and materials, so batching has candidates; a
                // repeated depth now and then, so ties exist.
                draw.Shader = 1u + rng.Below(3u);
                draw.Material = 1u + rng.Below(3u);
                draw.Depth = 100u * (1u + rng.Below(12u));
                draw.Entity = static_cast<i32>(i);
                draws.push_back(draw);
            }
            return draws;
        }

        [[nodiscard]] PacketMetadata MetadataFor(const Draw& draw)
        {
            PacketMetadata meta;
            meta.m_SortKey = draw.Blended
                                 ? DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, draw.Shader, draw.Material, draw.Depth)
                                 : MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, draw.Shader, draw.Material, draw.Depth);
            return meta;
        }

        [[nodiscard]] DrawMeshCommand CommandFor(const Draw& draw)
        {
            DrawMeshCommand cmd = MakeSyntheticDrawMeshCommand(draw.Shader, draw.Material, 0.5f, draw.Entity);
            cmd.color = glm::vec4(1.0f, 1.0f, 1.0f, draw.Blended ? 0.5f : 1.0f);
            return cmd;
        }

        // The entity order a bucket replays, instanced draws expanded in
        // instance order, which is the order the GPU blends them.
        [[nodiscard]] std::vector<i32> ReplayOrder(const CommandBucket& bucket)
        {
            std::vector<i32> order;
            for (const CommandPacket* packet : bucket.GetSortedCommands())
            {
                if (packet == nullptr)
                    continue;
                if (packet->GetCommandType() == CommandType::DrawMeshInstanced)
                {
                    const auto* instanced = packet->GetCommandData<DrawMeshInstancedCommand>();
                    for (u32 i = 0; i < instanced->instanceCount; ++i)
                    {
                        const i32* id = instanced->entityIDBufferOffset == UINT32_MAX
                                            ? nullptr
                                            : FrameDataBufferManager::Get().GetEntityIDPtr(instanced->entityIDBufferOffset + i);
                        order.push_back(id != nullptr ? *id : -1);
                    }
                    continue;
                }
                order.push_back(packet->GetCommandData<DrawMeshCommand>()->entityID);
            }
            return order;
        }

        // The legality check: every blended draw is back to front (a draw is
        // never nearer than one blended after it). Returns the first violation.
        [[nodiscard]] std::string FirstOrderViolation(const std::vector<i32>& order, const std::vector<Draw>& draws)
        {
            u32 previousDepth = std::numeric_limits<u32>::max();
            i32 previousEntity = -1;
            for (const i32 entity : order)
            {
                if (entity < 0 || static_cast<sizet>(entity) >= draws.size())
                    return "unknown entity " + std::to_string(entity) + " in the replay";
                const Draw& draw = draws[static_cast<sizet>(entity)];
                if (!draw.Blended)
                    continue;
                if (draw.Depth > previousDepth)
                    return "blended entity " + std::to_string(entity) + " (depth " + std::to_string(draw.Depth) +
                           ") after entity " + std::to_string(previousEntity) + " (depth " +
                           std::to_string(previousDepth) + "): not back to front";
                previousDepth = draw.Depth;
                previousEntity = entity;
            }
            return {};
        }

        // Blended draws with an identical complete key are an unordered tie
        // run; canonicalise each run so two legal orders compare equal.
        [[nodiscard]] std::vector<i32> CanonicalBlendedOrder(const std::vector<i32>& order, const std::vector<Draw>& draws)
        {
            std::vector<i32> blended;
            for (const i32 entity : order)
            {
                if (entity >= 0 && static_cast<sizet>(entity) < draws.size() && draws[static_cast<sizet>(entity)].Blended)
                    blended.push_back(entity);
            }
            const auto sameKey = [&draws](i32 a, i32 b)
            {
                const Draw& x = draws[static_cast<sizet>(a)];
                const Draw& y = draws[static_cast<sizet>(b)];
                return x.Shader == y.Shader && x.Material == y.Material && x.Depth == y.Depth;
            };
            for (sizet begin = 0; begin < blended.size();)
            {
                sizet end = begin + 1u;
                while (end < blended.size() && sameKey(blended[begin], blended[end]))
                    ++end;
                std::sort(blended.begin() + static_cast<std::ptrdiff_t>(begin), blended.begin() + static_cast<std::ptrdiff_t>(end));
                begin = end;
            }
            return blended;
        }

    } // namespace

    class RendererStateMachineDrawOrder : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_OwnsFrameData = !FrameDataBufferManager::IsInitialized();
            if (m_OwnsFrameData)
                FrameDataBufferManager::Init();
        }
        void TearDown() override
        {
            if (m_OwnsFrameData)
                FrameDataBufferManager::Shutdown();
        }

        // Submit, sort and (optionally) batch the draws as the geometry
        // stream does, from one thread or from `workers` real threads.
        [[nodiscard]] std::vector<i32> Execute(const std::vector<Draw>& draws, bool batching, u32 workers, u64 seed)
        {
            FrameDataBufferManager::Get().Reset();
            CommandBucketConfig config;
            config.EnableSorting = true;
            config.EnableBatching = batching;
            config.InitialCapacity = 256;
            CommandBucket bucket(config);
            std::vector<std::unique_ptr<CommandAllocator>> allocators;
            for (u32 w = 0; w < std::max(workers, 1u); ++w)
                allocators.push_back(std::make_unique<CommandAllocator>());
            bucket.SetAllocator(allocators[0].get());

            if (workers <= 1u)
            {
                for (const Draw& draw : draws)
                    bucket.Submit(CommandFor(draw), MetadataFor(draw), allocators[0].get());
            }
            else
            {
                // A seeded, uneven partition: the merge must not depend on
                // which worker a draw landed on.
                SplitMix64 rng(seed);
                std::vector<std::vector<const Draw*>> shares(workers);
                for (const Draw& draw : draws)
                    shares[rng.Below(workers)].push_back(&draw);
                bucket.PrepareForParallelSubmission(static_cast<u32>(draws.size()));
                std::vector<std::thread> threads;
                for (u32 w = 0; w < workers; ++w)
                {
                    threads.emplace_back([&bucket, &allocators, &shares, w]
                                         {
                                                 for (const Draw* draw : shares[w])
                                                 {
                                                     auto* packet = allocators[w]->CreateCommandPacket(CommandFor(*draw), MetadataFor(*draw));
                                                     bucket.SubmitPacketParallel(packet, w);
                                                 } });
                }
                for (auto& thread : threads)
                    thread.join();
                bucket.MergeThreadLocalCommands();
            }

            if (batching)
            {
                bucket.BatchCommands(*allocators[0]);
                if (!bucket.IsSorted())
                    bucket.SortCommands();
            }
            else
            {
                bucket.SortCommands();
            }
            return ReplayOrder(bucket);
        }

      private:
        bool m_OwnsFrameData = false;
    };

    TEST_F(RendererStateMachineDrawOrder, BatchingAndParallelSubmissionKeepBlendedDrawOrder)
    {
        u32 executions = 0;
        for (u64 seed = 1; seed <= 16u; ++seed)
        {
            const std::vector<Draw> draws = MakeDraws(seed);
            const std::vector<i32> reference = Execute(draws, false, 1u, seed);
            ASSERT_TRUE(FirstOrderViolation(reference, draws).empty()) << "seed " << seed << ", serial unbatched: "
                                                                       << FirstOrderViolation(reference, draws);
            const std::vector<i32> canonical = CanonicalBlendedOrder(reference, draws);
            for (const bool batching : { false, true })
            {
                for (const u32 workers : { 1u, 3u, 4u })
                {
                    SCOPED_TRACE("seed " + std::to_string(seed) + (batching ? " batched" : " unbatched") + ", " +
                                 std::to_string(workers) + " worker(s)");
                    const std::vector<i32> order = Execute(draws, batching, workers, seed * 31u + workers);
                    std::vector<i32> sortedIds = order;
                    std::ranges::sort(sortedIds);
                    std::vector<i32> expectedIds(draws.size());
                    for (sizet i = 0; i < draws.size(); ++i)
                        expectedIds[i] = static_cast<i32>(i);
                    EXPECT_EQ(sortedIds, expectedIds) << "a draw was lost or duplicated";
                    EXPECT_EQ(FirstOrderViolation(order, draws), "");
                    EXPECT_EQ(CanonicalBlendedOrder(order, draws), canonical)
                        << "the blended order differs from the serial unbatched execution beyond tie runs";
                    ++executions;
                    Coverage::RecordComparison(workers > 1u ? "serial-vs-parallel.cpu-submit" : "batch-order.cpu");
                }
            }
        }
        EXPECT_EQ(executions, 96u);
    }

    // The negative control for the order check: a batcher that groups blended
    // draws on shader and material only (the #1327 mistake rule 4 forbids)
    // collapses a group to its first member's position. The legality check must
    // reject the order that produces.
    TEST_F(RendererStateMachineDrawOrder, PartialKeyBatchingIsCaught)
    {
        u32 caught = 0;
        for (u64 seed = 1; seed <= 16u; ++seed)
        {
            const std::vector<Draw> draws = MakeDraws(seed);
            const std::vector<i32> order = Execute(draws, false, 1u, seed);
            // Re-group: every blended draw moves up to the first blended draw
            // that shares its shader and material.
            std::vector<i32> grouped;
            std::vector<bool> placed(draws.size(), false);
            for (const i32 entity : order)
            {
                if (placed[static_cast<sizet>(entity)])
                    continue;
                grouped.push_back(entity);
                placed[static_cast<sizet>(entity)] = true;
                const Draw& lead = draws[static_cast<sizet>(entity)];
                if (!lead.Blended)
                    continue;
                for (const i32 other : order)
                {
                    const Draw& candidate = draws[static_cast<sizet>(other)];
                    if (!placed[static_cast<sizet>(other)] && candidate.Blended && candidate.Shader == lead.Shader &&
                        candidate.Material == lead.Material)
                    {
                        grouped.push_back(other);
                        placed[static_cast<sizet>(other)] = true;
                    }
                }
            }
            if (!FirstOrderViolation(grouped, draws).empty())
                ++caught;
        }
        EXPECT_GE(caught, 14u) << "partial-key grouping reordered blended draws and the legality check missed it";
        if (caught > 0u)
            Coverage::RecordComparison("draw-order.cpu");
    }
} // namespace OloEngine::Tests::StateMachine
