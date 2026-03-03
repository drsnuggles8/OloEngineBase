#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_set>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Minimal Stub RenderPass for Graph Tests (no GL)
// =============================================================================

class StubRenderPass : public RenderPass
{
  public:
    explicit StubRenderPass(const std::string& name) { m_Name = name; }
    ~StubRenderPass() override = default;

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute() override { m_ExecuteCount++; }
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override { return nullptr; }
    void SetupFramebuffer(u32 /*w*/, u32 /*h*/) override {}
    void ResizeFramebuffer(u32 /*w*/, u32 /*h*/) override {}
    void OnReset() override {}

    u32 GetExecuteCount() const { return m_ExecuteCount; }

  private:
    u32 m_ExecuteCount = 0;
};

// Helper to create and add a stub pass
static Ref<StubRenderPass> AddStub(RenderGraph& graph, const std::string& name)
{
    auto pass = Ref<StubRenderPass>::Create(name);
    pass->SetName(name);
    graph.AddPass(pass);
    return pass;
}

// =============================================================================
// Basic Graph Construction
// =============================================================================

TEST(RenderGraph, AddPassMakesItRetrievable)
{
    RenderGraph graph;

    auto pass = AddStub(graph, "PassA");
    auto retrieved = graph.GetPass<StubRenderPass>("PassA");

    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->GetName(), "PassA");
}

TEST(RenderGraph, GetPassReturnsNullForUnknown)
{
    RenderGraph graph;
    auto pass = graph.GetPass<StubRenderPass>("NonExistent");
    EXPECT_EQ(pass, nullptr);
}

TEST(RenderGraph, GetAllPassesReturnsAll)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    auto all = graph.GetAllPasses();
    EXPECT_EQ(all.size(), 3u);
}

// =============================================================================
// Topological Order Respects DAG
// =============================================================================

TEST(RenderGraph, LinearChainOrder)
{
    // A -> B -> C
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");

    // Execute triggers UpdateDependencyGraph
    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 3u);

    // A must appear before B, and B before C
    auto posA = std::find(order.begin(), order.end(), "A") - order.begin();
    auto posB = std::find(order.begin(), order.end(), "B") - order.begin();
    auto posC = std::find(order.begin(), order.end(), "C") - order.begin();

    EXPECT_LT(posA, posB) << "A must execute before B";
    EXPECT_LT(posB, posC) << "B must execute before C";
}

TEST(RenderGraph, DiamondDependency)
{
    //   A
    //  / \.
    // B   C
    //  \ /
    //   D
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");
    AddStub(graph, "D");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "C");
    graph.ConnectPass("B", "D");
    graph.ConnectPass("C", "D");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 4u);

    auto posA = std::find(order.begin(), order.end(), "A") - order.begin();
    auto posB = std::find(order.begin(), order.end(), "B") - order.begin();
    auto posC = std::find(order.begin(), order.end(), "C") - order.begin();
    auto posD = std::find(order.begin(), order.end(), "D") - order.begin();

    EXPECT_LT(posA, posB);
    EXPECT_LT(posA, posC);
    EXPECT_LT(posB, posD);
    EXPECT_LT(posC, posD);
}

// =============================================================================
// Execution Dependency (ordering without framebuffer piping)
// =============================================================================

TEST(RenderGraph, ExecutionDependencyOrdering)
{
    RenderGraph graph;
    AddStub(graph, "Shadow");
    AddStub(graph, "Scene");

    // Shadow must run before Scene (e.g., shadow map texture dependency)
    graph.AddExecutionDependency("Shadow", "Scene");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 2u);

    auto posShadow = std::find(order.begin(), order.end(), "Shadow") - order.begin();
    auto posScene = std::find(order.begin(), order.end(), "Scene") - order.begin();

    EXPECT_LT(posShadow, posScene) << "Shadow must execute before Scene";
}

// =============================================================================
// All Passes Present in Order
// =============================================================================

TEST(RenderGraph, AllPassesPresentInOrder)
{
    RenderGraph graph;
    std::vector<std::string> names = {"GBuffer", "Lighting", "PostProcess", "UI", "Final"};

    for (const auto& n : names)
    {
        AddStub(graph, n);
    }

    // Chain: GBuffer -> Lighting -> PostProcess -> Final
    graph.ConnectPass("GBuffer", "Lighting");
    graph.ConnectPass("Lighting", "PostProcess");
    graph.ConnectPass("PostProcess", "Final");
    // UI is independent
    graph.AddExecutionDependency("PostProcess", "UI");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    EXPECT_EQ(order.size(), names.size());

    // Verify all names present
    std::unordered_set<std::string> orderSet(order.begin(), order.end());
    for (const auto& n : names)
    {
        EXPECT_TRUE(orderSet.count(n) > 0) << "Pass '" << n << "' missing from execution order";
    }
}

// =============================================================================
// Independent Passes (no dependencies)
// =============================================================================

TEST(RenderGraph, IndependentPassesAllExecute)
{
    RenderGraph graph;
    auto a = AddStub(graph, "IndependentA");
    auto b = AddStub(graph, "IndependentB");
    auto c = AddStub(graph, "IndependentC");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    EXPECT_EQ(order.size(), 3u);

    // All should execute
    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 1u);
}

// =============================================================================
// Final Pass
// =============================================================================

TEST(RenderGraph, SetFinalPassIsFinalPass)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");

    graph.SetFinalPass("B");

    EXPECT_TRUE(graph.IsFinalPass("B"));
    EXPECT_FALSE(graph.IsFinalPass("A"));
}

// =============================================================================
// GetConnections Returns All Connections
// =============================================================================

TEST(RenderGraph, GetConnectionsComplete)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");

    auto connections = graph.GetConnections();
    EXPECT_GE(connections.size(), 2u);
}

// =============================================================================
// Duplicate AddPass Overwrites
// =============================================================================

TEST(RenderGraph, DuplicatePassNameOverwrites)
{
    RenderGraph graph;
    auto first = AddStub(graph, "SameName");
    auto second = AddStub(graph, "SameName");

    auto retrieved = graph.GetPass<StubRenderPass>("SameName");
    // The second addition should overwrite
    EXPECT_EQ(retrieved.Raw(), second.Raw());
}

// =============================================================================
// Multiple Execute Calls Are Idempotent
// =============================================================================

TEST(RenderGraph, MultipleExecuteIdempotent)
{
    RenderGraph graph;
    auto pass = AddStub(graph, "Repeater");

    graph.Execute();
    graph.Execute();
    graph.Execute();

    EXPECT_EQ(pass->GetExecuteCount(), 3u);

    const auto& order = graph.GetPassOrder();
    EXPECT_EQ(order.size(), 1u);
}

// =============================================================================
// Single Pass Graph
// =============================================================================

TEST(RenderGraph, SinglePassGraph)
{
    RenderGraph graph;
    auto pass = AddStub(graph, "Only");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "Only");
    EXPECT_EQ(pass->GetExecuteCount(), 1u);
}
