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
    explicit StubRenderPass(const std::string& name)
    {
        m_Name = name;
    }
    ~StubRenderPass() override = default;

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute() override
    {
        m_ExecuteCount++;
    }
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }
    void SetupFramebuffer(u32 /*w*/, u32 /*h*/) override {}
    void ResizeFramebuffer(u32 /*w*/, u32 /*h*/) override {}
    void OnReset() override {}

    u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

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
    std::vector<std::string> names = { "GBuffer", "Lighting", "PostProcess", "UI", "Final" };

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
// Layer 5 structural validation — production ordering invariants
//
// These tests encode the engine's pipeline contract: regardless of how passes
// are added or connected, a correctly-authored RenderGraph must topologically
// sort them so shadow rendering precedes scene rendering, scene precedes
// post-processing, and post-processing precedes the final screen blit.
//
// Failures here indicate the render graph violates its fundamental ordering
// contract — a class of bugs that would produce missing shadows, ungraded
// output, or tone-mapped geometry being post-processed twice.
// =============================================================================

TEST(RenderGraphStructural, ProductionPassOrderingAlwaysRespected)
{
    RenderGraph graph;
    // Deliberately add in a random order to prove topological sort doesn't
    // rely on insertion order.
    AddStub(graph, "FinalPass");
    AddStub(graph, "ScenePass");
    AddStub(graph, "ShadowPass");
    AddStub(graph, "PostProcessPass");

    graph.AddExecutionDependency("ShadowPass", "ScenePass");
    graph.ConnectPass("ScenePass", "PostProcessPass");
    graph.ConnectPass("PostProcessPass", "FinalPass");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    auto posOf = [&](const std::string& n)
    {
        return std::find(order.begin(), order.end(), n) - order.begin();
    };
    const auto shadowPos = posOf("ShadowPass");
    const auto scenePos = posOf("ScenePass");
    const auto postPos = posOf("PostProcessPass");
    const auto finalPos = posOf("FinalPass");

    // std::find returns end() for missing names, whose index equals order.size().
    // Without these existence asserts the EXPECT_LT comparisons below could
    // silently pass (both sides clamped to size()) when the pass dropped out
    // of the execution order entirely.
    const auto orderSize = static_cast<decltype(shadowPos)>(order.size());
    ASSERT_LT(shadowPos, orderSize) << "ShadowPass missing from execution order";
    ASSERT_LT(scenePos, orderSize) << "ScenePass missing from execution order";
    ASSERT_LT(postPos, orderSize) << "PostProcessPass missing from execution order";
    ASSERT_LT(finalPos, orderSize) << "FinalPass missing from execution order";

    EXPECT_LT(shadowPos, scenePos) << "Shadow must precede Scene";
    EXPECT_LT(scenePos, postPos) << "Scene must precede PostProcess";
    EXPECT_LT(postPos, finalPos) << "PostProcess must precede Final";
}

TEST(RenderGraphStructural, DuplicateConnectPassIsIdempotent)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "B"); // Duplicate: must not be added twice
    graph.ConnectPass("A", "B");

    const auto connections = graph.GetConnections();
    u32 abCount = 0;
    for (const auto& c : connections)
    {
        if (c.OutputPass == "A" && c.InputPass == "B")
            ++abCount;
    }
    EXPECT_EQ(abCount, 1u) << "Duplicate ConnectPass calls must not register multiple edges";
}

TEST(RenderGraphStructural, EachPassExecutesExactlyOncePerExecute)
{
    RenderGraph graph;
    auto a = AddStub(graph, "A");
    auto b = AddStub(graph, "B");
    auto c = AddStub(graph, "C");

    // Diamond — B and C both depend on A, and the engine previously had bugs
    // where a pass with multiple downstream consumers ran more than once.
    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "C");

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u) << "A with 2 downstreams must still run once";
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 1u);

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 2u);
    EXPECT_EQ(b->GetExecuteCount(), 2u);
    EXPECT_EQ(c->GetExecuteCount(), 2u);
}

TEST(RenderGraphStructural, CycleIsDetectedAndDoesNotCrash)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");

    // Add a cycle A → B → A. Topological sort must NOT infinite-loop.
    // We tolerate either outcome: cycle rejected silently or passes
    // dropped. The binding contract is "no infinite loop / no crash."
    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "A");

    EXPECT_NO_THROW(graph.Execute())
        << "Cycle in render graph must not crash execution";
}

TEST(RenderGraphStructural, ConnectingToMissingPassDoesNotCorruptGraph)
{
    RenderGraph graph;
    AddStub(graph, "A");

    // Neither input nor output exists — engine logs an error but graph
    // state must remain consistent.
    graph.ConnectPass("A", "Nonexistent");
    graph.ConnectPass("Nonexistent", "A");

    EXPECT_NO_THROW(graph.Execute());
    EXPECT_EQ(graph.GetAllPasses().size(), 1u)
        << "Connect calls with missing passes must not register new passes";
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

// =============================================================================
// ResetTopology: per-RenderingPath rebuild contract (Option 3)
//
// Renderer3D::ConfigureRenderGraph calls RenderGraph::ResetTopology() and
// then re-registers only the passes relevant for the active RenderingPath.
// These tests pin down the ResetTopology primitive that the rebuild
// depends on.
// =============================================================================

TEST(RenderGraphResetTopology, ClearsPassesAndAllowsRebuild)
{
    RenderGraph graph;

    // Initial "deferred-like" topology: 3 passes, a chain.
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");
    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");
    graph.SetFinalPass("C");
    graph.Execute();
    EXPECT_EQ(graph.GetAllPasses().size(), 3u);
    EXPECT_EQ(graph.GetPassOrder().size(), 3u);

    graph.ResetTopology();

    // After reset the graph must behave as freshly constructed: no
    // passes, no cached order, no stale connections leaking into the
    // next rebuild.
    EXPECT_EQ(graph.GetAllPasses().size(), 0u);
    EXPECT_EQ(graph.GetConnections().size(), 0u);

    // Rebuild as a different "forward-like" topology (2 passes).
    AddStub(graph, "X");
    AddStub(graph, "Y");
    graph.ConnectPass("X", "Y");
    graph.SetFinalPass("Y");
    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "X");
    EXPECT_EQ(order[1], "Y");
    EXPECT_EQ(graph.GetPass<StubRenderPass>("A"), nullptr)
        << "Old passes must not be retrievable after ResetTopology";
    EXPECT_EQ(graph.GetPass<StubRenderPass>("B"), nullptr);
    EXPECT_EQ(graph.GetPass<StubRenderPass>("C"), nullptr);
}

TEST(RenderGraphResetTopology, PreservesPassReferenceOwnership)
{
    // The graph stores passes by weak lookup; ResetTopology must only
    // drop the graph's references, not destroy passes still held by
    // external owners (in the real engine that's Renderer3D::s_Data).
    RenderGraph graph;
    auto a = Ref<StubRenderPass>::Create("A");
    a->SetName("A");
    graph.AddPass(a);

    graph.ResetTopology();

    ASSERT_NE(a.Raw(), nullptr);
    EXPECT_EQ(a->GetName(), "A");

    // Re-registering the same pass instance is legal — simulates the
    // per-path rebuild re-AddPass'ing the persistent pass instances.
    graph.AddPass(a);
    graph.SetFinalPass("A");
    graph.Execute();
    EXPECT_EQ(a->GetExecuteCount(), 1u);
}

TEST(RenderGraphResetTopology, MultipleResetsAreSafe)
{
    // Rebuilding repeatedly (e.g. user toggling RenderingPath rapidly)
    // must not accumulate state or crash.
    RenderGraph graph;
    for (i32 i = 0; i < 5; ++i)
    {
        graph.ResetTopology();
        AddStub(graph, "P");
        graph.SetFinalPass("P");
        graph.Execute();
        EXPECT_EQ(graph.GetAllPasses().size(), 1u);
    }
}
