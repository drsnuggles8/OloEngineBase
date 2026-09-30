// OLO_TEST_LAYER: L5

// =============================================================================
// Out-of-band boundaries in the render graph (issue #1331).
//
// Work the resource graph cannot see — a TLAS, a retained occlusion pyramid, a
// command bucket one pass hands another — is declared as a named out-of-band
// boundary (RenderGraphOutOfBand.h). These tests pin the contract on stub
// graphs, CPU only:
//
//   * edges: every writer precedes a current-frame reader and every
//     previous-frame reader precedes a writer, whatever the registration
//     order; a write after a read is ordered too;
//   * reachability: a current-frame reader keeps its producers, an
//     ordering-only edge keeps nothing, a frame-epilogue read roots its
//     writers and outlives the last pass (the side-effect roots and the single
//     fixpoint are RenderGraphReachability.* in RenderGraphTest.cpp);
//   * the declarations never reach a physical planner;
//   * the runtime ledger reports an access nobody declared and a read on the
//     wrong side of a write, and is clean when the declarations match;
//   * negative controls: drop a declaration (the omission fault) and reverse
//     the tie-break, and the ledger or the validator must fail.
//
// The production declarations and the live ledger are exercised on the real
// pipeline by RenderGraphOutOfBandScheduleTest.cpp.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Passes/RayTracingScenePass.h"
#include "OloEngine/Renderer/Passes/SkeletalDeformPass.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphOutOfBand.h"

#include "StateMachine/StateMachineCoverage.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr std::string_view kBoundary = "TestBoundary";
        constexpr std::string_view kPublication = "TestPublication";

        void RegisterTestBoundaries(RenderGraph& graph, RGFramePhaseUse prologue = RGFramePhaseUse::None,
                                    RGFramePhaseUse epilogue = RGFramePhaseUse::None)
        {
            graph.RegisterOutOfBandBoundary(RGOutOfBandBoundary{
                .Name = kBoundary,
                .Kind = RGOutOfBandKind::GpuResource,
                .Prologue = prologue,
                .Epilogue = epilogue,
                .Owner = "test",
                .Reason = "test",
            });
            graph.RegisterOutOfBandBoundary(RGOutOfBandBoundary{
                .Name = kPublication,
                .Kind = RGOutOfBandKind::CpuPublication,
                .Owner = "test",
                .Reason = "test",
            });
        }

        // A node whose Setup and Execute are lambdas, so one test can declare an
        // out-of-band access and perform it through the same RGOutOfBand::Note
        // call production access sites make.
        class OutOfBandStubNode : public RenderGraphNode
        {
          public:
            using SetupFn = std::function<void(RGBuilder&)>;
            using ExecuteFn = std::function<void()>;

            OutOfBandStubNode(std::string name, SetupFn setup, ExecuteFn execute)
                : m_Setup(std::move(setup)), m_Execute(std::move(execute))
            {
                SetName(name);
            }

            void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
            {
                RenderGraphNode::Setup(builder, blackboard);
                if (m_Setup)
                    m_Setup(builder);
            }

            void Execute(RGCommandContext& /*context*/) override
            {
                ++m_ExecuteCount;
                if (m_Execute)
                    m_Execute();
            }

            [[nodiscard]] u32 GetExecuteCount() const
            {
                return m_ExecuteCount;
            }

          private:
            SetupFn m_Setup;
            ExecuteFn m_Execute;
            u32 m_ExecuteCount = 0;
        };

        Ref<OutOfBandStubNode> AddNode(RenderGraph& graph, std::string name, OutOfBandStubNode::SetupFn setup,
                                       OutOfBandStubNode::ExecuteFn execute = {})
        {
            auto node = Ref<OutOfBandStubNode>::Create(std::move(name), std::move(setup), std::move(execute));
            graph.AddNode(node);
            return node;
        }

        // The final pass writes an imported target so it has a real output.
        Ref<OutOfBandStubNode> AddFinal(RenderGraph& graph, std::function<void(RGBuilder&)> extra = {})
        {
            return AddNode(graph, "Final",
                           [extra = std::move(extra)](RGBuilder& builder)
                           {
                               const auto target = builder.ImportTexture(
                                   "FinalTarget", 7u, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "FinalTarget"));
                               builder.Write(target, RGWriteUsage::RenderTarget);
                               if (extra)
                                   extra(builder);
                           });
        }

        [[nodiscard]] i64 PositionOf(const RenderGraph& graph, std::string_view pass)
        {
            const auto order = graph.GetExecutionOrder();
            const auto it = std::ranges::find_if(order, [pass](const FString& name)
                                                 { return name.ToView() == pass; });
            return it == order.end() ? -1 : static_cast<i64>(it - order.begin());
        }

        [[nodiscard]] bool IsCulled(const RenderGraph& graph, std::string_view pass)
        {
            const auto culled = graph.GetCulledPasses();
            return std::ranges::any_of(culled, [pass](const FString& name)
                                       { return name.ToView() == pass; });
        }

        [[nodiscard]] bool HasHazard(const TArray64<RenderGraph::Hazard>& hazards, RenderGraph::HazardKind kind,
                                     std::string_view resource)
        {
            return std::ranges::any_of(hazards, [&](const RenderGraph::Hazard& hazard)
                                       { return hazard.Kind == kind && hazard.Resource.ToView() == resource; });
        }

        [[nodiscard]] std::string Describe(const TArray64<RenderGraph::Hazard>& hazards)
        {
            std::string out;
            for (const auto& hazard : hazards)
                out += "  " + hazard.Message.ToStdString() + "\n";
            return out.empty() ? "  (none)\n" : out;
        }

        // Scoped lever / fault state so a failing test cannot leak into the next.
        class ScopedReverseTieBreak
        {
          public:
            explicit ScopedReverseTieBreak(bool on) : m_Previous(Levers::RenderGraphReverseTieBreak())
            {
                Levers::SetRenderGraphReverseTieBreak(on);
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

        class ScopedOmission
        {
          public:
            explicit ScopedOmission(std::string spec)
            {
                RGOutOfBand::SetOmittedDeclarationFault(std::move(spec));
            }
            ~ScopedOmission()
            {
                RGOutOfBand::SetOmittedDeclarationFault(std::nullopt);
            }
            ScopedOmission(const ScopedOmission&) = delete;
            ScopedOmission& operator=(const ScopedOmission&) = delete;
        };

        // Runs one frame the way Renderer3D does: open the ledger, execute,
        // validate, close. Returns the ledger hazards.
        TArray64<RenderGraph::Hazard> RunLedgerFrame(RenderGraph& graph, const std::function<void()>& prologue = {},
                                                     const std::function<void()>& epilogue = {})
        {
            RGOutOfBand::SetActiveLedger(&graph.GetOutOfBandLedger());
            graph.GetOutOfBandLedger().BeginFrame();
            if (prologue)
                prologue();
            graph.BuildFrameGraph();
            graph.Execute();
            if (epilogue)
                epilogue();
            auto hazards = graph.ValidateOutOfBandLedger();
            graph.GetOutOfBandLedger().EndFrame();
            RGOutOfBand::SetActiveLedger(nullptr);
            return hazards;
        }

        // Consumer registered FIRST and producer second: registration order
        // says "consumer first", so only the declaration can put the producer
        // ahead. Both touch the boundary through RGOutOfBand::Note, the call
        // production access sites make.
        // `finalKeepsProducer` roots the producer independently of the
        // consumer's declaration, which a control that drops that declaration
        // needs: otherwise the producer is simply culled and nothing is caught.
        void BuildConsumerBeforeProducer(RenderGraph& graph, bool finalKeepsProducer = false)
        {
            RegisterTestBoundaries(graph);
            AddNode(
                graph, "Consumer",
                [](RGBuilder& builder)
                {
                    builder.ReadOutOfBand(kBoundary);
                    const auto out = builder.ImportTexture("ConsumerOut", 11u,
                                                           RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "ConsumerOut"));
                    builder.Write(out, RGWriteUsage::RenderTarget);
                },
                []
                { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Read); });
            AddNode(
                graph, "Producer", [](RGBuilder& builder)
                { builder.WriteOutOfBand(kBoundary); },
                []
                { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Write); });
            AddFinal(graph, [finalKeepsProducer](RGBuilder& builder)
                     {
                         const auto in = builder.ImportTexture("ConsumerOut", 11u,
                                                               RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "ConsumerOut"));
                         [[maybe_unused]] const auto read = builder.Read(in, RGReadUsage::ShaderSample);
                         if (finalKeepsProducer)
                             builder.DependsOnPass("Producer"); });
            graph.SetFinalPass("Final");
            graph.SetRuntimeBarrierExecutionEnabled(false);
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Edges
    // -------------------------------------------------------------------------

    TEST(RenderGraphOutOfBand, WriterPrecedesCurrentFrameReaderWhateverTheRegistrationOrder)
    {
        RenderGraph graph;
        BuildConsumerBeforeProducer(graph);
        graph.BuildFrameGraph();

        EXPECT_LT(PositionOf(graph, "Producer"), PositionOf(graph, "Consumer"));
        EXPECT_FALSE(IsCulled(graph, "Producer")) << "a current-frame reader keeps its producer reachable";
        EXPECT_TRUE(graph.ValidateCompiledResourceHazards().IsEmpty());
    }

    TEST(RenderGraphOutOfBand, PreviousFrameReaderPrecedesAnInPlaceRebuild)
    {
        // The retained-pyramid shape: Rebuilder (registered first) rebuilds the
        // boundary in place, RetainedReader must still see last frame's value.
        RenderGraph graph;
        RegisterTestBoundaries(graph);
        AddNode(graph, "Rebuilder", [](RGBuilder& builder)
                {
                    builder.WriteOutOfBand(kBoundary);
                    const auto out = builder.ImportTexture("RebuilderOut", 12u,
                                                           RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "RebuilderOut"));
                    builder.Write(out, RGWriteUsage::RenderTarget); });
        AddNode(graph, "RetainedReader", [](RGBuilder& builder)
                {
                    builder.ReadOutOfBand(kBoundary, RGOutOfBandEpoch::PreviousFrame);
                    const auto out = builder.ImportTexture("ReaderOut", 13u,
                                                           RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "ReaderOut"));
                    builder.Write(out, RGWriteUsage::RenderTarget); });
        AddFinal(graph, [](RGBuilder& builder)
                 {
                     const std::pair<const char*, u32> inputs[] = { { "RebuilderOut", 12u }, { "ReaderOut", 13u } };
                     for (const auto& [name, id] : inputs)
                     {
                         const auto in = builder.ImportTexture(name, id, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name));
                         [[maybe_unused]] const auto read = builder.Read(in, RGReadUsage::ShaderSample);
                     } });
        graph.SetFinalPass("Final");
        graph.BuildFrameGraph();

        EXPECT_LT(PositionOf(graph, "RetainedReader"), PositionOf(graph, "Rebuilder"))
            << "a previous-frame read must run before this frame's in-place rebuild";
        EXPECT_TRUE(graph.ValidateCompiledResourceHazards().IsEmpty());
    }

    // VirtualGeometryPass's shape: it reads the retained pyramid AND rebuilds
    // it, and here it is registered after another rebuilder. Chaining the
    // writers in registration order would put the other rebuild first and
    // drop the retained read's edge as a cycle.
    TEST(RenderGraphOutOfBand, ARetainedReaderThatAlsoRebuildsRunsBeforeEveryOtherRebuild)
    {
        RenderGraph graph;
        RegisterTestBoundaries(graph);
        AddNode(graph, "OtherRebuilder", [](RGBuilder& builder)
                {
                    builder.WriteOutOfBand(kBoundary);
                    builder.Write(builder.ImportTexture("OtherOut", 14u, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "OtherOut")),
                                  RGWriteUsage::RenderTarget); });
        AddNode(graph, "ReadThenRebuild", [](RGBuilder& builder)
                {
                    builder.ReadOutOfBand(kBoundary, RGOutOfBandEpoch::PreviousFrame);
                    builder.WriteOutOfBand(kBoundary);
                    builder.Write(builder.ImportTexture("VgOut", 15u, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "VgOut")),
                                  RGWriteUsage::RenderTarget); });
        AddFinal(graph, [](RGBuilder& builder)
                 {
                     const std::pair<const char*, u32> inputs[] = { { "OtherOut", 14u }, { "VgOut", 15u } };
                     for (const auto& [name, id] : inputs)
                     {
                         [[maybe_unused]] const auto read = builder.Read(
                             builder.ImportTexture(name, id, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name)),
                             RGReadUsage::ShaderSample);
                     } });
        graph.SetFinalPass("Final");
        graph.BuildFrameGraph();

        EXPECT_LT(PositionOf(graph, "ReadThenRebuild"), PositionOf(graph, "OtherRebuilder"));
        const auto hazards = graph.ValidateCompiledResourceHazards();
        EXPECT_TRUE(hazards.IsEmpty()) << Describe(hazards);
    }

    // A resource edge derived in registration order can contradict a
    // previous-frame read: both passes write the same export, and the
    // rebuilder is registered first. The resource edge wins the cycle check,
    // so the retained read would see this frame's rebuild. That must be LOUD,
    // not a silently dropped edge.
    TEST(RenderGraphOutOfBand, AResourceEdgeContradictingARetainedReadIsReported)
    {
        RenderGraph graph;
        RegisterTestBoundaries(graph);
        const auto exportDesc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "DepthExport");
        AddNode(graph, "Rebuilder", [exportDesc](RGBuilder& builder)
                {
                    builder.WriteOutOfBand(kBoundary);
                    builder.Write(builder.ImportTexture("DepthExport", 16u, exportDesc), RGWriteUsage::TransferDest); });
        AddNode(graph, "RetainedReader", [exportDesc](RGBuilder& builder)
                {
                    builder.ReadOutOfBand(kBoundary, RGOutOfBandEpoch::PreviousFrame);
                    builder.Write(builder.ImportTexture("DepthExport", 16u, exportDesc), RGWriteUsage::TransferDest); });
        AddFinal(graph, [exportDesc](RGBuilder& builder)
                 { [[maybe_unused]] const auto read = builder.Read(builder.ImportTexture("DepthExport", 16u, exportDesc), RGReadUsage::ShaderSample); });
        graph.SetFinalPass("Final");
        graph.BuildFrameGraph();

        ASSERT_LT(PositionOf(graph, "Rebuilder"), PositionOf(graph, "RetainedReader")) << "the resource edge ordered them";
        const auto hazards = graph.ValidateCompiledResourceHazards();
        EXPECT_TRUE(HasHazard(hazards, RenderGraph::HazardKind::OutOfBandOrdering, kBoundary))
            << "a retained read ordered after its rebuild went unreported:\n"
            << Describe(hazards);
    }

    TEST(RenderGraphOutOfBand, DestroyingTheActiveGraphDetachesItsLedger)
    {
        {
            RenderGraph graph;
            RGOutOfBand::SetActiveLedger(&graph.GetOutOfBandLedger());
            ASSERT_EQ(RGOutOfBand::GetActiveLedger(), &graph.GetOutOfBandLedger());
        }
        EXPECT_EQ(RGOutOfBand::GetActiveLedger(), nullptr) << "an access site would record into a freed ledger";
        RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Read); // must be a no-op, not a use-after-free
    }

    TEST(RenderGraphOutOfBand, AnOrderingOnlyEdgeKeepsNothingAlive)
    {
        // The final pass rebuilds the boundary in place, so it depends on the
        // retained reader through an edge that only orders them. Nothing else
        // needs the reader, and that edge must not keep it alive.
        RenderGraph graph;
        RegisterTestBoundaries(graph);
        AddNode(graph, "RetainedReader", [](RGBuilder& builder)
                { builder.ReadOutOfBand(kBoundary, RGOutOfBandEpoch::PreviousFrame); });
        AddFinal(graph, [](RGBuilder& builder)
                 { builder.WriteOutOfBand(kBoundary); });
        graph.SetFinalPass("Final");
        graph.BuildFrameGraph();

        EXPECT_TRUE(IsCulled(graph, "RetainedReader"));
    }

    TEST(RenderGraphOutOfBand, WriteAfterReadIsOrderedAndDoesNotRootTheReader)
    {
        // Writer -> Reader -> Overwriter on ONE resource name. Overwriter must
        // wait for Reader (write after read) even though nothing it reads comes
        // from Reader. Before #1331 that held by registration order alone.
        const auto build = [](RenderGraph& graph)
        {
            graph.SetRuntimeBarrierExecutionEnabled(false);
            const auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "Shared");
            AddNode(graph, "Writer", [desc](RGBuilder& builder)
                    { builder.Write(builder.ImportTexture("Shared", 21u, desc), RGWriteUsage::RenderTarget); });
            AddNode(graph, "Reader", [desc](RGBuilder& builder)
                    {
                        [[maybe_unused]] const auto read = builder.Read(builder.ImportTexture("Shared", 21u, desc), RGReadUsage::ShaderSample);
                        builder.Write(builder.ImportTexture("ReaderOut", 22u, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "ReaderOut")),
                                      RGWriteUsage::RenderTarget); });
            AddNode(graph, "Overwriter", [desc](RGBuilder& builder)
                    { builder.Write(builder.ImportTexture("Shared", 21u, desc), RGWriteUsage::RenderTarget); });
            AddFinal(graph, [desc](RGBuilder& builder)
                     {
                         [[maybe_unused]] const auto shared = builder.Read(builder.ImportTexture("Shared", 21u, desc), RGReadUsage::ShaderSample);
                         [[maybe_unused]] const auto readerOut = builder.Read(
                             builder.ImportTexture("ReaderOut", 22u, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "ReaderOut")),
                             RGReadUsage::ShaderSample); });
            graph.SetFinalPass("Final");
        };

        for (const bool reversed : { false, true })
        {
            const ScopedReverseTieBreak lever(reversed);
            RenderGraph graph;
            build(graph);
            graph.BuildFrameGraph();
            EXPECT_LT(PositionOf(graph, "Reader"), PositionOf(graph, "Overwriter")) << "reversed=" << reversed;
            EXPECT_TRUE(graph.ValidateCompiledResourceHazards().IsEmpty()) << "reversed=" << reversed;
        }

        // And the ordering edge alone does not keep a reader: drop Final's use
        // of ReaderOut and Reader is culled although Overwriter still runs.
        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(false);
        const auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "Shared");
        AddNode(graph, "Writer", [desc](RGBuilder& builder)
                { builder.Write(builder.ImportTexture("Shared", 21u, desc), RGWriteUsage::RenderTarget); });
        AddNode(graph, "Reader", [desc](RGBuilder& builder)
                { [[maybe_unused]] const auto read = builder.Read(builder.ImportTexture("Shared", 21u, desc), RGReadUsage::ShaderSample); });
        AddNode(graph, "Overwriter", [desc](RGBuilder& builder)
                { builder.Write(builder.ImportTexture("Shared", 21u, desc), RGWriteUsage::RenderTarget); });
        AddFinal(graph, [desc](RGBuilder& builder)
                 { [[maybe_unused]] const auto shared = builder.Read(builder.ImportTexture("Shared", 21u, desc), RGReadUsage::ShaderSample); });
        graph.SetFinalPass("Final");
        graph.BuildFrameGraph();
        EXPECT_FALSE(IsCulled(graph, "Overwriter"));
        EXPECT_TRUE(IsCulled(graph, "Reader"));
    }

    // -------------------------------------------------------------------------
    // Reachability
    // -------------------------------------------------------------------------

    TEST(RenderGraphOutOfBand, FrameEpilogueReadRootsItsWriterAndOutlivesTheLastPass)
    {
        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(false);
        AddNode(graph, "DepthWriter", [](RGBuilder& builder)
                {
                    RGResourceDesc desc;
                    desc.Kind = RGResourceHandle::Kind::Texture2D;
                    desc.Format = RGResourceFormat::RGBA16Float;
                    desc.Width = 64;
                    desc.Height = 64;
                    desc.DebugName = "EpilogueDepth";
                    builder.Write(builder.CreateTexture("EpilogueDepth", desc), RGWriteUsage::RenderTarget); });
        AddNode(graph, "Later", {});
        AddFinal(graph, [](RGBuilder& builder)
                 { builder.DependsOnPass("Later"); });
        graph.SetFinalPass("Final");

        graph.BuildFrameGraph();
        EXPECT_TRUE(IsCulled(graph, "DepthWriter")) << "control: nothing in the graph reads it";

        graph.DeclareFrameEpilogueRead("EpilogueDepth", "test epilogue");
        graph.BuildFrameGraph();
        EXPECT_FALSE(IsCulled(graph, "DepthWriter")) << "an epilogue read roots the writer";

        const auto plan = graph.GetTransientPlan();
        const auto entry = std::ranges::find_if(plan, [](const RenderGraph::TransientPlanEntry& e)
                                                { return e.Resource == "EpilogueDepth"; });
        ASSERT_NE(entry, plan.end());
        EXPECT_EQ(entry->LastPassIndex, static_cast<u32>(graph.GetExecutionOrder().size() - 1u))
            << "the backing must survive to the end of the frame, not to its last in-graph access";

        graph.RemoveFrameEpilogueRead("EpilogueDepth");
        graph.BuildFrameGraph();
        EXPECT_TRUE(IsCulled(graph, "DepthWriter")) << "removing the read must invalidate the cached plan";
    }

    // -------------------------------------------------------------------------
    // The declarations stay logical
    // -------------------------------------------------------------------------

    TEST(RenderGraphOutOfBand, DeclarationsNeverReachAPhysicalPlanner)
    {
        RenderGraph graph;
        BuildConsumerBeforeProducer(graph);
        graph.BuildFrameGraph();

        for (const auto& barrier : graph.GetPlannedBarriers())
            EXPECT_NE(barrier.Resource.ToView(), kBoundary) << "a boundary reached the barrier planner";
        for (const auto& entry : graph.GetTransientPlan())
            EXPECT_NE(entry.Resource.ToView(), kBoundary) << "a boundary reached the transient planner";
        for (const auto& resource : graph.GetRegisteredResources())
            EXPECT_NE(resource.Name.ToView(), kBoundary) << "a boundary reached the resource registry";
    }

    TEST(RenderGraphOutOfBand, AMisdeclaredBoundaryIsAHazard)
    {
        RenderGraph graph;
        RegisterTestBoundaries(graph);
        AddFinal(graph, [](RGBuilder& builder)
                 {
                     builder.Publish(kBoundary);          // a GPU resource declared as a publication
                     builder.ReadOutOfBand("NoSuchBoundary"); });
        graph.SetFinalPass("Final");
        graph.BuildFrameGraph();

        const auto hazards = graph.ValidateCompiledResourceHazards();
        EXPECT_TRUE(HasHazard(hazards, RenderGraph::HazardKind::UnregisteredOutOfBandBoundary, kBoundary)) << Describe(hazards);
        EXPECT_TRUE(HasHazard(hazards, RenderGraph::HazardKind::UnregisteredOutOfBandBoundary, "NoSuchBoundary")) << Describe(hazards);
    }

    TEST(RenderGraphOutOfBand, PlanDigestSeesOutOfBandDeclarations)
    {
        const auto digest = [](bool declare)
        {
            RenderGraph graph;
            RegisterTestBoundaries(graph);
            AddFinal(graph, [declare](RGBuilder& builder)
                     {
                         if (declare)
                             builder.ConsumePublication(kPublication); });
            graph.SetFinalPass("Final");
            graph.BuildFrameGraph();
            return graph.ComputeCompiledPlanDigest();
        };
        EXPECT_NE(digest(false), digest(true)) << "a cached plan would survive a changed out-of-band declaration";
    }

    // -------------------------------------------------------------------------
    // The runtime ledger
    // -------------------------------------------------------------------------

    TEST(RenderGraphOutOfBand, LedgerIsCleanWhenDeclarationsMatchTheAccesses)
    {
        for (const bool reversed : { false, true })
        {
            const ScopedReverseTieBreak lever(reversed);
            RenderGraph graph;
            BuildConsumerBeforeProducer(graph);
            const auto hazards = RunLedgerFrame(graph);
            EXPECT_TRUE(hazards.IsEmpty()) << "reversed=" << reversed << "\n"
                                           << Describe(hazards);
            EXPECT_EQ(graph.GetOutOfBandLedger().GetEntries().Num(), 2);
        }
    }

    // NEGATIVE CONTROL. The consumer's declaration is dropped while its access
    // still runs. Registration order happens to put the consumer first, the
    // way a missing DependsOnPass used to hide: the ledger must report both the
    // undeclared read and the read that ran before the write.
    TEST(RenderGraphOutOfBand, DroppedCurrentFrameDeclarationIsCaughtByTheLedger)
    {
        RenderGraph graph;
        const ScopedOmission fault("Consumer/TestBoundary");
        BuildConsumerBeforeProducer(graph, /*finalKeepsProducer*/ true);
        const auto hazards = RunLedgerFrame(graph);

        const bool undeclared = HasHazard(hazards, RenderGraph::HazardKind::UndeclaredOutOfBandAccess, kBoundary);
        const bool misordered = HasHazard(hazards, RenderGraph::HazardKind::OutOfBandOrdering, kBoundary);
        EXPECT_TRUE(undeclared) << Describe(hazards);
        EXPECT_TRUE(misordered) << "the consumer ran before the producer and the ledger did not see it:\n"
                                << Describe(hazards);
        if (undeclared && misordered)
            StateMachine::Coverage::RecordComparison("omitted-current-frame-read.cpu");
    }

    // NEGATIVE CONTROL for a previous-frame read: drop the retained reader's
    // declaration and reverse the tie-break so the rebuild runs first.
    TEST(RenderGraphOutOfBand, DroppedPreviousFrameDeclarationIsCaughtByTheLedger)
    {
        const auto build = [](RenderGraph& graph)
        {
            RegisterTestBoundaries(graph);
            graph.SetRuntimeBarrierExecutionEnabled(false);
            AddNode(
                graph, "RetainedReader", [](RGBuilder& builder)
                { builder.ReadOutOfBand(kBoundary, RGOutOfBandEpoch::PreviousFrame); },
                []
                { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::ReadPreviousFrame); });
            AddNode(
                graph, "Rebuilder", [](RGBuilder& builder)
                { builder.WriteOutOfBand(kBoundary); },
                []
                { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Write); });
            AddFinal(graph, [](RGBuilder& builder)
                     {
                         builder.DependsOnPass("RetainedReader");
                         builder.DependsOnPass("Rebuilder"); });
            graph.SetFinalPass("Final");
        };

        const ScopedReverseTieBreak lever(true);
        {
            RenderGraph graph;
            build(graph);
            const auto hazards = RunLedgerFrame(graph);
            EXPECT_TRUE(hazards.IsEmpty()) << "declared, the reversed order must still hold:\n"
                                           << Describe(hazards);
        }
        {
            const ScopedOmission fault("RetainedReader/TestBoundary");
            RenderGraph graph;
            build(graph);
            const auto hazards = RunLedgerFrame(graph);
            const bool undeclared = HasHazard(hazards, RenderGraph::HazardKind::UndeclaredOutOfBandAccess, kBoundary);
            const bool misordered = HasHazard(hazards, RenderGraph::HazardKind::OutOfBandOrdering, kBoundary);
            EXPECT_TRUE(undeclared) << Describe(hazards);
            EXPECT_TRUE(misordered) << "the retained read ran after the rebuild and the ledger did not see it:\n"
                                    << Describe(hazards);
            if (undeclared && misordered)
                StateMachine::Coverage::RecordComparison("omitted-previous-frame-read.cpu");
        }
    }

    TEST(RenderGraphOutOfBand, LedgerChecksFramePhasesAgainstTheBoundary)
    {
        RenderGraph graph;
        RegisterTestBoundaries(graph, RGFramePhaseUse::Read, RGFramePhaseUse::Write);
        AddFinal(graph);
        graph.SetFinalPass("Final");
        graph.SetRuntimeBarrierExecutionEnabled(false);

        const auto allowed = RunLedgerFrame(
            graph, []
            { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::ReadPreviousFrame); },
            []
            { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Write); });
        EXPECT_TRUE(allowed.IsEmpty()) << Describe(allowed);

        const auto forbidden = RunLedgerFrame(
            graph, []
            { RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Write); },
            []
            { RGOutOfBand::Note(kPublication, RGOutOfBandAccess::Read); });
        EXPECT_TRUE(HasHazard(forbidden, RenderGraph::HazardKind::UndeclaredOutOfBandAccess, kBoundary)) << Describe(forbidden);
        EXPECT_TRUE(HasHazard(forbidden, RenderGraph::HazardKind::UndeclaredOutOfBandAccess, kPublication)) << Describe(forbidden);
    }

    TEST(RenderGraphOutOfBand, LedgerIgnoresAccessesOutsideAFrame)
    {
        RenderGraph graph;
        RegisterTestBoundaries(graph);
        RGOutOfBand::SetActiveLedger(&graph.GetOutOfBandLedger());
        RGOutOfBand::Note(kBoundary, RGOutOfBandAccess::Write);
        RGOutOfBand::SetActiveLedger(nullptr);
        EXPECT_TRUE(graph.GetOutOfBandLedger().GetEntries().IsEmpty())
            << "an editor readback between frames is not a frame access";
    }

    // -------------------------------------------------------------------------
    // Production declarations that run on every backend
    // -------------------------------------------------------------------------

    // The AS build chain, with the REAL SkeletalDeformPass and
    // RayTracingScenePass declarations and a TLAS reader registered ahead of
    // both. The reader performs its access through the note
    // RayTracingScene::GetTlasDeviceAddressForTrace makes.
    TEST(RenderGraphOutOfBand, AccelerationStructureChainOrdersByDeclarationNotRegistration)
    {
        const auto build = [](RenderGraph& graph)
        {
            graph.SetRuntimeBarrierExecutionEnabled(false);
            AddNode(
                graph, "TlasReader",
                [](RGBuilder& builder)
                {
                    builder.ReadOutOfBand(RGOutOfBandBoundaries::SceneTLAS);
                    builder.Write(builder.ImportTexture("TraceOut", 51u,
                                                        RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "TraceOut")),
                                  RGWriteUsage::ShaderImage);
                },
                []
                { RGOutOfBand::Note(RGOutOfBandBoundaries::SceneTLAS, RGOutOfBandAccess::Read); });
            auto tlas = Ref<RayTracingScenePass>::Create();
            graph.AddNode(tlas);
            auto deform = Ref<SkeletalDeformPass>::Create();
            graph.AddNode(deform);
            AddFinal(graph, [](RGBuilder& builder)
                     { [[maybe_unused]] const auto trace = builder.Read(
                           builder.ImportTexture("TraceOut", 51u, RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "TraceOut")),
                           RGReadUsage::ShaderSample); });
            graph.SetFinalPass("Final");
        };

        {
            RenderGraph graph;
            build(graph);
            graph.BuildFrameGraph();
            EXPECT_LT(PositionOf(graph, "SkeletalDeformPass"), PositionOf(graph, "RayTracingScenePass"));
            EXPECT_LT(PositionOf(graph, "RayTracingScenePass"), PositionOf(graph, "TlasReader"));
            EXPECT_FALSE(IsCulled(graph, "SkeletalDeformPass"))
                << "without NeverCull the deform pass lives only through RayTracingScenePass's read";
            EXPECT_TRUE(graph.ValidateCompiledResourceHazards().IsEmpty());
        }

        // NEGATIVE CONTROL: without the reader's declaration the scheduler is
        // free to trace before the build, and registration order does.
        {
            const ScopedOmission fault("TlasReader/SceneTLAS");
            RenderGraph graph;
            build(graph);
            graph.BuildFrameGraph();
            const bool reordered = PositionOf(graph, "TlasReader") < PositionOf(graph, "RayTracingScenePass");
            EXPECT_TRUE(reordered) << "the control is vacuous: something other than the declaration ordered the reader";
            if (reordered)
                StateMachine::Coverage::RecordComparison("omitted-tlas-read.cpu");
        }
    }

    // -------------------------------------------------------------------------
    // The exposed schedule
    // -------------------------------------------------------------------------

    TEST(RenderGraphOutOfBand, ScheduleExportNamesPassesBoundariesPhasesAndTheLedger)
    {
        RenderGraph graph;
        BuildConsumerBeforeProducer(graph);
        auto kept = AddNode(graph, "UndocumentedKeeper", {});
        kept->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
        graph.DeclareFrameEpilogueRead("ConsumerOut", "test epilogue");
        (void)RunLedgerFrame(graph);

        const auto schedule = nlohmann::json::parse(graph.ExportOutOfBandScheduleJson());

        const auto findPass = [&](std::string_view name) -> const nlohmann::json*
        {
            for (const auto& pass : schedule["passes"])
                if (pass["name"].get<std::string>() == name)
                    return &pass;
            return nullptr;
        };
        for (const char* name : { "Consumer", "Producer", "Final", "UndocumentedKeeper" })
            EXPECT_NE(findPass(name), nullptr) << name;
        ASSERT_NE(findPass("Consumer"), nullptr);
        EXPECT_EQ((*findPass("Consumer"))["outOfBand"][0]["boundary"], std::string(kBoundary));
        EXPECT_EQ((*findPass("UndocumentedKeeper"))["sideEffectReason"], "UNDOCUMENTED");

        bool sawTestBoundary = false;
        u32 productionBoundaries = 0;
        for (const auto& boundary : schedule["boundaries"])
        {
            if (boundary["name"] == std::string(kBoundary))
            {
                sawTestBoundary = true;
                EXPECT_EQ(boundary["producers"][0], "Producer");
                EXPECT_EQ(boundary["consumers"][0], "Consumer");
            }
            for (const RGOutOfBandBoundary& production : GetProductionOutOfBandBoundaries())
                if (boundary["name"] == std::string(production.Name))
                    ++productionBoundaries;
        }
        EXPECT_TRUE(sawTestBoundary);
        EXPECT_EQ(productionBoundaries, GetProductionOutOfBandBoundaries().size());
        EXPECT_EQ(schedule["framePhaseWork"].size(), GetFramePhaseWork().size());
        EXPECT_EQ(schedule["frameEpilogueReads"][0]["resource"], "ConsumerOut");
        EXPECT_EQ(schedule["ledger"].size(), 2u);
        EXPECT_TRUE(schedule["ledgerHazards"].empty());
    }

    // Every production side effect has a documented reason. The table is the
    // source; a pass that gains NeverCull without an entry reads UNDOCUMENTED
    // in the schedule and fails the production-pipeline check.
    TEST(RenderGraphOutOfBand, SideEffectReasonTableHasNoDuplicatesOrBlanks)
    {
        std::vector<std::string_view> names;
        for (const auto& reason : GetSideEffectReasons())
        {
            EXPECT_FALSE(reason.Reason.empty()) << reason.PassName;
            names.push_back(reason.PassName);
        }
        std::ranges::sort(names);
        EXPECT_EQ(std::ranges::adjacent_find(names), names.end());

        for (const auto& boundary : GetProductionOutOfBandBoundaries())
        {
            EXPECT_FALSE(boundary.Owner.empty()) << boundary.Name;
            EXPECT_FALSE(boundary.Reason.empty()) << boundary.Name;
        }
        for (const auto& work : GetFramePhaseWork())
        {
            EXPECT_FALSE(work.Owner.empty()) << work.Name;
            EXPECT_FALSE(work.Reason.empty()) << work.Name;
        }
    }
} // namespace OloEngine::Tests
