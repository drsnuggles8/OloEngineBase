// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include "MockRendererAPI.h"
#include "OloEngine/Renderer/RenderGraphPlanExecutor.h"
#include "OloEngine/Renderer/RenderGraphSubmissionPlan.h"
#include "OloEngine/Renderer/RGBuilder.h"

#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <thread>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    class PreparedNode final : public RenderGraphNode
    {
      public:
        explicit PreparedNode(std::string_view name)
        {
            SetName(name);
        }
        bool SupportsWholePassRecording() const noexcept override
        {
            return Eligible;
        }
        bool IsEnabled() const noexcept override
        {
            return Enabled;
        }
        void Setup(RGBuilder& builder, FrameBlackboard&) override
        {
            if (OnSetup)
                OnSetup(builder);
        }
        RGPreparedPass PrepareParallelRecording(RGCommandContext& context) override
        {
            if (OnPrepare)
                OnPrepare(context);
            return { .Record = Record, .Publish = Publish, .Resources = Resources };
        }
        void Execute(RGCommandContext& context) override
        {
            ++InlineCalls;
            if (Record)
                Record(context);
            if (Publish)
                Publish();
        }
        bool Eligible = true;
        bool Enabled = true;
        u32 InlineCalls = 0;
        std::function<void(RGCommandContext&)> OnPrepare;
        std::function<void(RGBuilder&)> OnSetup;
        std::function<void(RGCommandContext&)> Record;
        std::function<void()> Publish;
        std::vector<RGRecordingResourceUse> Resources;
    };

    class ThreadedRecordingAPI final : public Testing::MockRendererAPI
    {
      public:
        bool SupportsParallelRecording() const override
        {
            return true;
        }
        void PushDebugGroup(u32, std::string_view) override {}
        void PopDebugGroup() override {}
        void RecordParallelOrdered(u32 count, const std::function<void(u32)>& body,
                                   const std::function<void(u32)>& before,
                                   const std::function<void(u32)>& after, u32, std::span<const std::string>) override
        {
            ++Groups;
            std::barrier start(static_cast<std::ptrdiff_t>(count));
            std::vector<std::jthread> workers;
            for (u32 lane = 0; lane < count; ++lane)
                workers.emplace_back([&, lane]
                                     {
                    start.arrive_and_wait();
                    body(lane); });
            workers.clear(); // join before caller-owned brackets/publication
            for (u32 lane = 0; lane < count; ++lane)
            {
                before(lane);
                ExecutionOrder.push_back(lane);
                after(lane);
            }
        }
        u32 Groups = 0;
        std::vector<u32> ExecutionOrder;
    };

    auto MakePlan(std::span<PreparedNode*> nodes,
                  const std::unordered_map<std::string, std::vector<std::string>>& dependencies = {},
                  std::span<const RenderGraph::PlannedBarrier> barriers = {},
                  std::span<const RenderGraph::AsyncComputeBatch> batches = {}, bool splitBarriers = false,
                  const std::function<bool(const std::string&)>& isPassReachable = {})
    {
        std::vector<std::string> order;
        for (const auto* node : nodes)
            order.push_back(node->GetName());
        return RenderGraphSubmissionPlan::BuildPlan({
            .ExecutionOrder = order,
            .Dependencies = dependencies,
            .PlannedBarriers = barriers,
            .Transitions = {},
            .Batches = batches,
            .EnableSplitBarriers = splitBarriers,
            .GetPassWorkType = [nodes](const std::string& name)
            {
                for (const auto* node : nodes)
                    if (node->GetName() == name)
                        return node->GetPassWorkType();
                return RenderGraphPassWorkType::Graphics; },
            .ResolveNodePointer = [nodes](const std::string& name) -> RenderGraphNode*
            {
                for (auto* node : nodes)
                    if (node->GetName() == name)
                        return node;
                return nullptr;
            },
            .IsPassReachable = isPassReachable,
        });
    }
} // namespace

TEST(RenderGraphParallelRecording, WorkersOwnContextsAndPublishBeforeDependentConsumer)
{
    PreparedNode first("First"), second("Second"), consumer("Consumer");
    const auto caller = std::this_thread::get_id();
    std::array<std::string, 2> labels;
    std::array<u32, 2> lanes{ UINT32_MAX, UINT32_MAX };
    std::array<std::thread::id, 2> threads;
    std::vector<std::string> published;
    for (auto [node, index] : { std::pair{ &first, 0u }, std::pair{ &second, 1u } })
    {
        node->Resources = { { RHI::ResourceHandle{ index + 10u, 1u }, true } };
        node->OnPrepare = [&](RGCommandContext&)
        { EXPECT_EQ(std::this_thread::get_id(), caller); };
        node->Record = [&, index](RGCommandContext& context)
        {
            labels[index] = context.GetActivePassName();
            lanes[index] = context.GetRecordingLane();
            threads[index] = std::this_thread::get_id();
        };
        node->Publish = [&, node]
        {
            EXPECT_EQ(std::this_thread::get_id(), caller);
            published.push_back(node->GetName());
        };
    }
    consumer.Record = [&](RGCommandContext&)
    {
        EXPECT_EQ(published, (std::vector<std::string>{ "First", "Second" }));
    };
    std::array nodes{ &first, &second, &consumer };
    const auto plan = MakePlan(nodes, { { "Consumer", { "First", "Second" } } });
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[0].RecordingGroup, plan[1].RecordingGroup);
    EXPECT_NE(plan[0].RecordingGroup, UINT32_MAX);
    EXPECT_EQ(plan[2].RecordingGroup, UINT32_MAX);
    ThreadedRecordingAPI api;
    RGCommandContext context;
    const auto timings = RenderGraphPlanExecutor::ExecutePlan({ .SubmissionPlan = plan, .Context = context, .RuntimeBarrierExecutionEnabled = false, .IsPassReachable = [](const std::string&)
                                                                                                                                                     { return true; },
                                                                .RecordingAPI = &api });
    ASSERT_EQ(timings.size(), 3u);
    EXPECT_EQ(labels, (std::array<std::string, 2>{ "First", "Second" }));
    EXPECT_EQ(lanes, (std::array<u32, 2>{ 0, 1 }));
    EXPECT_NE(threads[0], caller);
    EXPECT_NE(threads[1], caller);
    EXPECT_NE(threads[0], threads[1]);
    EXPECT_EQ(api.Groups, 1u);
    EXPECT_EQ(api.ExecutionOrder, (std::vector<u32>{ 0, 1 }));
    EXPECT_EQ(consumer.InlineCalls, 1u);
}

TEST(RenderGraphParallelRecording, PhysicalAliasDeclinesLogicalIndependence)
{
    PreparedNode writer("Writer"), reader("Reader");
    const RHI::ResourceHandle aliased{ 23, 1 };
    writer.Resources = { { aliased, true } };
    reader.Resources = { { aliased, false } };
    std::vector<std::string> order;
    writer.Record = [&](RGCommandContext&)
    { order.push_back("Writer"); };
    reader.Record = [&](RGCommandContext&)
    { order.push_back("Reader"); };
    std::array nodes{ &writer, &reader };
    const auto plan = MakePlan(nodes);
    ThreadedRecordingAPI api;
    RGCommandContext context;
    const auto timings = RenderGraphPlanExecutor::ExecutePlan({ .SubmissionPlan = plan, .Context = context, .RuntimeBarrierExecutionEnabled = false, .IsPassReachable = [](const std::string&)
                                                                                                                                                     { return true; },
                                                                .RecordingAPI = &api });
    EXPECT_EQ(api.Groups, 0u);
    EXPECT_EQ(order, (std::vector<std::string>{ "Writer", "Reader" }));
    EXPECT_EQ(timings.size(), 2u);
}

TEST(RenderGraphParallelRecording, CaptureHookRetainsEveryOriginalPassBoundary)
{
    PreparedNode first("First"), second("Second");
    std::vector<std::string> order;
    first.Record = [&](RGCommandContext&)
    { order.push_back("First"); };
    second.Record = [&](RGCommandContext&)
    { order.push_back("Second"); };
    std::array nodes{ &first, &second };
    const auto plan = MakePlan(nodes);
    ThreadedRecordingAPI api;
    RGCommandContext context;
    RenderGraph graph;
    const auto timings = RenderGraphPlanExecutor::ExecutePlan({ .SubmissionPlan = plan, .Context = context, .RuntimeBarrierExecutionEnabled = false, .IsPassReachable = [](const std::string&)
                                                                                                                                                     { return true; },
                                                                .PostPassHook = [&](const std::string& name, RenderGraph&)
                                                                { order.push_back("Capture " + name); },
                                                                .GraphForPostPassHook = &graph,
                                                                .RecordingAPI = &api });
    EXPECT_EQ(api.Groups, 0u);
    EXPECT_EQ(order, (std::vector<std::string>{ "First", "Capture First", "Second", "Capture Second" }));
    EXPECT_EQ(timings.size(), 2u);
}

TEST(RenderGraphParallelRecording, ConsumerBarrierRemainsAfterRecordingGroup)
{
    PreparedNode first("First"), second("Second"), consumer("Consumer");
    std::array nodes{ &first, &second, &consumer };
    std::array barriers{
        RenderGraph::PlannedBarrier{ .BeforePass = "First", .Resource = "Input", .Flags = MemoryBarrierFlags::TextureFetch },
        RenderGraph::PlannedBarrier{ .BeforePass = "Second", .Resource = "Input", .Flags = MemoryBarrierFlags::TextureFetch },
        RenderGraph::PlannedBarrier{ .BeforePass = "Consumer", .Resource = "Output", .Flags = MemoryBarrierFlags::TextureFetch }
    };
    const auto plan = MakePlan(nodes, { { "Consumer", { "First", "Second" } } }, barriers);
    ASSERT_EQ(plan.size(), 6u);
    for (sizet command = 0; command < 4; ++command)
        EXPECT_EQ(plan[command].RecordingGroup, 0u);
    EXPECT_EQ(plan[4].CommandKind, RenderGraph::SubmissionCommand::Kind::MemoryBarrier);
    EXPECT_EQ(plan[4].RecordingGroup, UINT32_MAX);
    EXPECT_EQ(plan[5].RecordingGroup, UINT32_MAX);
}

TEST(RenderGraphParallelRecording, RecordingGroupsStopAtBatchBoundariesAndCapacity)
{
    std::vector<std::unique_ptr<PreparedNode>> owners;
    std::vector<PreparedNode*> nodes;
    for (u32 index = 0; index < 20; ++index)
    {
        owners.push_back(std::make_unique<PreparedNode>("Pass" + std::to_string(index)));
        nodes.push_back(owners.back().get());
    }
    const std::array batches{ RenderGraph::AsyncComputeBatch{ .ComputeNodes = { "Pass0", "Pass1" } } };
    const auto plan = MakePlan(nodes, {}, {}, batches);
    std::vector<std::pair<u32, u32>> groupsAndLanes;
    for (const auto& command : plan)
    {
        if (command.CommandKind == RenderGraph::SubmissionCommand::Kind::Pass)
            groupsAndLanes.emplace_back(command.RecordingGroup, command.RecordingLane);
        else
            EXPECT_EQ(command.RecordingGroup, UINT32_MAX);
    }
    ASSERT_EQ(groupsAndLanes.size(), 20u);
    EXPECT_EQ(groupsAndLanes[0], (std::pair{ 0u, 0u }));
    EXPECT_EQ(groupsAndLanes[1], (std::pair{ 0u, 1u }));
    EXPECT_EQ(groupsAndLanes[2], (std::pair{ 1u, 0u }));
    EXPECT_EQ(groupsAndLanes[17], (std::pair{ 1u, 15u }));
    EXPECT_EQ(groupsAndLanes[18], (std::pair{ 2u, 0u }));
    EXPECT_EQ(groupsAndLanes[19], (std::pair{ 2u, 1u }));
}

// Production shape: both compute passes wait for scene depth, and graphics
// consumes GTAO afterwards. Split barriers are ON, as in the default graph.
namespace
{
    Ref<PreparedNode> AddSchedulingNode(RenderGraph& graph, const std::string& name, bool eligible,
                                        std::vector<std::string> dependencies, bool compute = false,
                                        bool declaresAccess = true)
    {
        auto node = Ref<PreparedNode>::Create(name);
        node->Eligible = eligible;
        node->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
        node->SetPassWorkType(compute ? RenderGraphPassWorkType::Compute : RenderGraphPassWorkType::Graphics);
        node->SetAsyncComputeCandidate(compute);
        node->OnSetup = [name, dependencies, compute, declaresAccess](RGBuilder& builder)
        {
            for (const auto& dependency : dependencies)
                builder.DependsOnPass(dependency);
            if (!declaresAccess)
                return;
            auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name + "Output");
            desc.Format = RGResourceFormat::RGBA8UNorm;
            desc.Width = desc.Height = 16;
            const auto output = builder.CreateTexture(name + "Output", desc);
            builder.Write(output, compute ? RGWriteUsage::ShaderStorage : RGWriteUsage::RenderTarget);
        };
        graph.AddNode(node);
        return node;
    }
} // namespace

TEST(RenderGraphParallelRecording, SchedulingWaitsForAnIndependentPartnerBeforePlanningLifetimes)
{
    RenderGraph graph;
    AddSchedulingNode(graph, "Lighting", false, {});
    AddSchedulingNode(graph, "DepthVelocity", true, { "Lighting" });
    AddSchedulingNode(graph, "Particles", false, { "Lighting" });
    AddSchedulingNode(graph, "EASU", true, { "Particles" });
    AddSchedulingNode(graph, "Final", false, { "DepthVelocity", "EASU" });
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    const auto& order = graph.GetExecutionOrder();
    const auto depth = std::ranges::find(order, "DepthVelocity");
    const auto easu = std::ranges::find(order, "EASU");
    ASSERT_NE(depth, order.end());
    ASSERT_NE(easu, order.end());
    EXPECT_EQ(easu, depth + 1);
    EXPECT_LT(std::ranges::find(order, "Particles"), depth);
    for (const auto& lifetime : graph.GetResourceLifetimes())
        if (lifetime.FirstWritePass == "DepthVelocity" || lifetime.FirstWritePass == "EASU")
            EXPECT_EQ(order[lifetime.FirstWritePassIndex], lifetime.FirstWritePass);
    const auto plan = graph.GetSubmissionPlan();
    std::vector<u32> groups;
    for (const auto& command : plan)
        if (command.CommandKind == RenderGraph::SubmissionCommand::Kind::Pass &&
            (command.NodeName == "DepthVelocity" || command.NodeName == "EASU"))
            groups.push_back(command.RecordingGroup);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_NE(groups[0], UINT32_MAX);
    EXPECT_EQ(groups[0], groups[1]);
}

TEST(RenderGraphParallelRecording, SchedulingGroupsEarlyComputeWithLaterReadyPeersAndDrainsDisabledNodes)
{
    RenderGraph graph;
    AddSchedulingNode(graph, "Scene", false, {});
    AddSchedulingNode(graph, "Fog", true, { "Scene" }, true);
    // A disabled color pass can still declare inputs before its enabled guard.
    auto disabled = AddSchedulingNode(graph, "DisabledPrepared", true, {}, true);
    disabled->Enabled = false;
    disabled->OnSetup = [](RGBuilder& builder)
    {
        const auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "DisabledInput");
        const auto input = builder.ImportTextureHandle("DisabledInput", { 60001, 1 }, desc);
        [[maybe_unused]] const auto read = builder.Read(input, RGReadUsage::ShaderSample);
    };
    AddSchedulingNode(graph, "Geometry", false, { "Scene" });
    AddSchedulingNode(graph, "GTAO", true, { "Geometry" }, true);
    AddSchedulingNode(graph, "Marking", true, { "Geometry" }, true);
    AddSchedulingNode(graph, "Final", false, { "Fog", "GTAO", "Marking" });
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 7u);
    const auto fog = std::ranges::find(order, "Fog");
    ASSERT_NE(fog, order.end());
    EXPECT_LT(std::ranges::find(order, "Geometry"), fog);
    EXPECT_EQ(std::ranges::find(order, "GTAO"), fog + 1);
    EXPECT_EQ(std::ranges::find(order, "Marking"), fog + 2);
    EXPECT_EQ(order.back(), "Final");
    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0].ComputeNodes, (std::vector<std::string>{ "DisabledPrepared" }));
    EXPECT_EQ(batches[1].ComputeNodes, (std::vector<std::string>{ "Fog", "GTAO", "Marking" }));
}

TEST(RenderGraphParallelRecording, DisabledOrCulledTailCannotInvalidateAnActiveRecordingGroup)
{
    for (const bool disabled : { true, false })
    {
        PreparedNode first("First"), second("Second"), tail("Tail");
        tail.Enabled = !disabled;
        first.Record = second.Record = [](RGCommandContext&) {};
        std::array nodes{ &first, &second, &tail };
        const auto reachable = [disabled](const std::string& name)
        { return disabled || name != "Tail"; };
        const auto plan = MakePlan(nodes, {}, {}, {}, false, reachable);
        ASSERT_EQ(plan.size(), 3u);
        EXPECT_NE(plan[0].RecordingGroup, UINT32_MAX);
        EXPECT_EQ(plan[0].RecordingGroup, plan[1].RecordingGroup);
        EXPECT_EQ(plan[2].RecordingGroup, UINT32_MAX);
        ThreadedRecordingAPI api;
        RGCommandContext context;
        [[maybe_unused]] const auto timings = RenderGraphPlanExecutor::ExecutePlan({ .SubmissionPlan = plan, .Context = context, .RuntimeBarrierExecutionEnabled = false, .IsPassReachable = reachable, .RecordingAPI = &api });
        EXPECT_EQ(api.Groups, 1u);
        EXPECT_EQ(api.ExecutionOrder, (std::vector<u32>{ 0, 1 }));
    }
}

TEST(RenderGraphParallelRecording, ExternalBatchFencesPreserveDefaultSplitSubmissionAndPermitRecording)
{
    PreparedNode scene("Scene"), gtao("GTAO"), marking("Marking"), lighting("Lighting");
    scene.Eligible = lighting.Eligible = false;
    gtao.SetPassWorkType(RenderGraphPassWorkType::Compute);
    marking.SetPassWorkType(RenderGraphPassWorkType::Compute);
    std::array nodes{ &scene, &gtao, &marking, &lighting };
    const std::array batches{ RenderGraph::AsyncComputeBatch{ .ComputeNodes = { "GTAO", "Marking" } } };
    const auto plan = MakePlan(nodes, { { "GTAO", { "Scene" } }, { "Marking", { "Scene" } }, { "Lighting", { "GTAO" } } }, {}, batches, true);
    using Kind = RenderGraph::SubmissionCommand::Kind;
    const auto position = [&](Kind kind, std::string_view name = {})
    {
        return std::ranges::find_if(plan, [&](const auto& command)
                                    { return command.CommandKind == kind && (name.empty() || command.NodeName == name); });
    };
    const auto begin = position(Kind::BatchBegin);
    const auto end = position(Kind::BatchEnd);
    const auto first = position(Kind::Pass, "GTAO");
    const auto second = position(Kind::Pass, "Marking");
    ASSERT_NE(first, plan.end());
    ASSERT_NE(second, plan.end());
    EXPECT_NE(first->RecordingGroup, UINT32_MAX);
    EXPECT_EQ(first->RecordingGroup, second->RecordingGroup);
    EXPECT_LT(begin, first);
    EXPECT_LT(second, end);
    u32 waits = 0, signals = 0;
    for (auto command = plan.begin(); command != plan.end(); ++command)
    {
        for (const auto& edge : command->FenceEdges)
        {
            if (command->CommandKind == Kind::FenceWait)
            {
                ++waits;
                if (edge.ConsumerPass == "GTAO" || edge.ConsumerPass == "Marking")
                    EXPECT_LT(command, begin);
            }
            if (command->CommandKind == Kind::FenceSignal)
            {
                ++signals;
                if (edge.ProducerPass == "GTAO")
                    EXPECT_GT(command, end);
            }
        }
    }
    EXPECT_EQ(waits, 3u);
    EXPECT_EQ(signals, 3u);
}
