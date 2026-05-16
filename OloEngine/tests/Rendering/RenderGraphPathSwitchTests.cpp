// =============================================================================
// RenderGraphPathSwitchTests.cpp
//
// Layer-5 resource hazard validation — runtime render-path switching.
//
// `Renderer3D::ConfigureRenderGraph(RenderingPath)` is invoked whenever the
// user toggles between `RenderingPath::Forward`, `ForwardPlus`, and
// `Deferred`. Each call:
//   1. `RenderGraph::ResetTopology()` — drops passes, nodes, per-frame
//      declaration state, and cached execution/submission state.
//   2. Re-adds only the passes relevant to the new path.
//   3. Re-declares the execution edges (keeps only the explicit deferred
//      Scene->DeferredOpaqueDecal edge plus path-shared downstream handoffs,
//      while DeferredLighting/ForwardOverlay ordering is derived at runtime).
//
// The bug-classes this file guards against:
//   * A pass that belongs only to Deferred (DeferredOpaqueDecalPass,
//     DeferredLightingPass, ForwardOverlayPass) lingers in the graph after a Deferred→Forward
//     switch, re-declaring writes and producing a WAW hazard the next time
//     `ValidateResourceHazards` runs.
//   * The reverse: a Forward→Deferred switch forgets to AddExecutionDependency
//     for the new producer→consumer edge and ValidateResourceHazards misses
//     the RAW hazard because neither declarant is present.
//   * `OITResolvePass` (shared by every path) is re-added on every rebuild but
//     its edges are re-issued each cycle — two identical edges must not cause
//     the validator to double-count, and removing the edge must surface.
//
// The tests use the same `DeclarativeStubPass` + `AddDeclStub` helpers as
// `ResourceHazardValidationTests.cpp` — duplicated here so the file reads
// standalone and doesn't rely on TU link order.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "TestDeclarativeNode.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    class DeclarativeStubPass : public TestDeclarativeNode
    {
      public:
        explicit DeclarativeStubPass(const std::string& name)
        {
            m_Name = name;
        }

        void Init(const FramebufferSpecification& /*spec*/) override {}
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& /*context*/) override {}
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }
        void SetupFramebuffer(u32 /*w*/, u32 /*h*/) override {}
        void ResizeFramebuffer(u32 /*w*/, u32 /*h*/) override {}
        void OnReset() override {}

        void TestDeclareRead(std::string_view name)
        {
            DeclareTestRead(name);
        }
        void TestDeclareWrite(std::string_view name)
        {
            DeclareTestWrite(name);
        }
    };

    void DeclarativeStubPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        // TestDeclarativeNode::Setup flushes the recorded reads/writes
        // through the setup-time path (import + builder.Read/Write).
        TestDeclarativeNode::Setup(builder, blackboard);
    }

    void RegisterDeclarativeStubNode(RenderGraph& graph, Ref<DeclarativeStubPass> pass)
    {
        graph.AddNode(pass.As<RenderGraphNode>());
    }

    Ref<DeclarativeStubPass> AddDeclStub(RenderGraph& graph, const std::string& name)
    {
        auto pass = Ref<DeclarativeStubPass>::Create(name);
        pass->SetName(name);
        RegisterDeclarativeStubNode(graph, pass);
        return pass;
    }

    std::string HazardsToString(const std::vector<RenderGraph::Hazard>& hazards)
    {
        if (hazards.empty())
            return "<no hazards>";
        std::string out;
        for (const auto& h : hazards)
        {
            out += "\n  - ";
            out += h.Message;
        }
        return out;
    }

    bool ContainsPass(const RenderGraph& graph, const std::string& name)
    {
        const auto entries = graph.GetNodeSubmissionInfo();
        return std::any_of(entries.begin(), entries.end(),
                           [&](const RenderGraph::NodeSubmissionInfo& entry)
                           { return entry.NodeName == name; });
    }

    // Minimal production-shaped topology. Only the passes relevant to the
    // path-switch invariants (ScenePass, DeferredOpaqueDecalPass,
    // DeferredLightingPass, ForwardOverlayPass, OITResolvePass, FinalPass) are instantiated;
    // downstream post-process chain collapses into FinalPass so the tests
    // focus on the edges ConfigureRenderGraph actually toggles.
    //
    // When `forceSkipDecalEdge` is true, the Forward→Deferred rebuild
    // deliberately skips `ScenePass → DeferredOpaqueDecalPass`, simulating
    // the deferred-edge regression this suite is here to prevent.
    void RebuildTopology(RenderGraph& graph, bool deferred, bool forceSkipDecalEdge = false)
    {
        graph.ResetTopology();

        auto scene = AddDeclStub(graph, "ScenePass");
        scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

        if (deferred)
        {
            auto deferredDecal = AddDeclStub(graph, "DeferredOpaqueDecalPass");
            deferredDecal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            deferredDecal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            AddDeclStub(graph, "DeferredLightingPass");

            AddDeclStub(graph, "ForwardOverlayPass");
        }

        auto oit = AddDeclStub(graph, "OITResolvePass");
        (void)oit;

        auto fin = AddDeclStub(graph, "FinalPass");
        (void)fin;

        if (deferred)
        {
            if (!forceSkipDecalEdge)
                graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
            graph.AddExecutionDependency("DeferredLightingPass", "ForwardOverlayPass");
            graph.AddExecutionDependency("ForwardOverlayPass", "OITResolvePass");
        }
        else
        {
            graph.AddExecutionDependency("ScenePass", "OITResolvePass");
        }
        graph.AddExecutionDependency("OITResolvePass", "FinalPass");

        graph.SetFinalPass("FinalPass");
    }
} // namespace

// =============================================================================
// Forward→Deferred: the new passes (DeferredOpaqueDecalPass,
// DeferredLightingPass, ForwardOverlayPass) appear, the
// ScenePass→DeferredOpaqueDecalPass edge is established, and
// validation continues to pass. This is the happy path the user hits every
// time they flip the editor's `Path` dropdown.
// =============================================================================
TEST(RenderGraphPathSwitch, ForwardToDeferredInsertsDeferredPassesAndEdges)
{
    RenderGraph graph;
    RebuildTopology(graph, /*deferred=*/false);

    ASSERT_FALSE(ContainsPass(graph, "DeferredLightingPass")) << "Forward path should not contain DeferredLightingPass";
    ASSERT_FALSE(ContainsPass(graph, "ForwardOverlayPass")) << "Forward path should not contain ForwardOverlayPass";

    {
        const auto hazards = graph.ValidateResourceHazards();
        EXPECT_TRUE(hazards.empty()) << "Forward baseline: " << HazardsToString(hazards);
    }

    RebuildTopology(graph, /*deferred=*/true);

    EXPECT_TRUE(ContainsPass(graph, "DeferredOpaqueDecalPass"));
    EXPECT_TRUE(ContainsPass(graph, "DeferredLightingPass"));
    EXPECT_TRUE(ContainsPass(graph, "ForwardOverlayPass"));
    EXPECT_TRUE(ContainsPass(graph, "OITResolvePass"));

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "Deferred after switch: " << HazardsToString(hazards);
}

// =============================================================================
// Deferred→Forward: `ResetTopology` + a smaller pass set must leave the graph
// with no lingering DeferredOpaqueDecalPass / DeferredLightingPass /
// ForwardOverlayPass. A leftover pass would re-declare `Write(SceneColor)`
// and produce a WAW hazard against ScenePass.
// =============================================================================
TEST(RenderGraphPathSwitch, DeferredToForwardRemovesDeferredOnlyPasses)
{
    RenderGraph graph;
    RebuildTopology(graph, /*deferred=*/true);
    ASSERT_TRUE(ContainsPass(graph, "DeferredOpaqueDecalPass"));
    ASSERT_TRUE(ContainsPass(graph, "DeferredLightingPass"));
    ASSERT_TRUE(ContainsPass(graph, "ForwardOverlayPass"));

    RebuildTopology(graph, /*deferred=*/false);

    EXPECT_FALSE(ContainsPass(graph, "DeferredOpaqueDecalPass"))
        << "DeferredOpaqueDecalPass must be dropped by ResetTopology when switching to Forward";
    EXPECT_FALSE(ContainsPass(graph, "DeferredLightingPass"))
        << "DeferredLightingPass must be dropped by ResetTopology when switching to Forward";
    EXPECT_FALSE(ContainsPass(graph, "ForwardOverlayPass"))
        << "ForwardOverlayPass must be dropped by ResetTopology when switching to Forward";
    EXPECT_TRUE(ContainsPass(graph, "OITResolvePass")) << "OITResolvePass is path-agnostic and must survive the switch";

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Forward after Deferred→Forward switch: " << HazardsToString(hazards);
}

// =============================================================================
// Negative: Forward→Deferred forgetting to add `ScenePass →
// DeferredOpaqueDecalPass` must be flagged. This models the current baseline
// contract where DeferredLighting ordering is reached transitively through
// DeferredOpaqueDecalPass.
// =============================================================================
// =============================================================================
// Phase F slice 27 — ScenePass → DeferredOpaqueDecalPass derived edge.
// Previously, missing this explicit edge was detected as a RAW hazard.
// Slice 27 derives the ordering from DeclareWrite(SceneDepth/SceneNormals) on
// ScenePass + DeclareRead(SceneDepth) on DeferredOpaqueDecalPass, so no
// explicit AddExecutionDependency call is needed.
// =============================================================================
TEST(RenderGraphPathSwitch, ForwardToDeferredMissingSceneToDecalExplicitEdge_DerivedEdgeSufficient)
{
    RenderGraph graph;
    RebuildTopology(graph, /*deferred=*/false);
    RebuildTopology(graph, /*deferred=*/true, /*forceSkipDecalEdge=*/true);

    // Slice 27: ScenePass.DeclareWrite(SceneDepth) +
    // DeferredOpaqueDecalPass.DeclareRead(SceneDepth) derives the RAW edge
    // automatically.  No hazard expected.
    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: declaration-derived edge must prevent the SceneDepth RAW hazard."
        << HazardsToString(hazards);
}

// =============================================================================
// Repeated rebuilds across many paths must not accumulate edges or passes.
// This is stricter than the existing `ResetTopologyAndRebuildAcrossPathsNoLeaks`
// — each cycle's pass count is asserted against the expected value for its
// path, validating `ResetTopology` fully wipes before the rebuild.
// =============================================================================
TEST(RenderGraphPathSwitch, AlternatingRebuildsHaveStablePassCounts)
{
    RenderGraph graph;

    // Forward baseline: Scene + OITResolve + Final = 3.
    constexpr sizet kForwardPassCount = 3u;
    // Deferred adds DeferredOpaqueDecalPass + DeferredLightingPass +
    // ForwardOverlayPass = 6.
    constexpr sizet kDeferredPassCount = 6u;

    const bool paths[] = { false, true, false, true, true, false };
    for (i32 i = 0; i < static_cast<i32>(std::size(paths)); ++i)
    {
        RebuildTopology(graph, paths[i]);
        const auto hazards = graph.ValidateResourceHazards();
        EXPECT_TRUE(hazards.empty())
            << "Cycle " << i << " (deferred=" << paths[i] << "): " << HazardsToString(hazards);

        const sizet expected = paths[i] ? kDeferredPassCount : kForwardPassCount;
        EXPECT_EQ(graph.GetNodeSubmissionInfo().size(), expected)
            << "Cycle " << i << " (deferred=" << paths[i] << "): pass count drift — ResetTopology leaking";
    }
}

// =============================================================================
// Hand-verify the edges map after a switch to prove the validator is
// operating on the new topology, not stale cached state. Uses `GetExecutionOrder`
// to sample the topologically-sorted result after each rebuild.
// =============================================================================
TEST(RenderGraphPathSwitch, PassOrderReflectsCurrentTopologyAfterSwitch)
{
    RenderGraph graph;

    RebuildTopology(graph, /*deferred=*/true);
    (void)graph.ValidateResourceHazards(); // triggers UpdateDependencyGraph → populates m_PassOrder
    {
        const auto& order = graph.GetExecutionOrder();
        // Topological sort must place ScenePass before
        // DeferredOpaqueDecalPass before DeferredLightingPass before
        // ForwardOverlayPass.
        const auto sceneIdx = std::find(order.begin(), order.end(), "ScenePass");
        const auto decalIdx = std::find(order.begin(), order.end(), "DeferredOpaqueDecalPass");
        const auto lightingIdx = std::find(order.begin(), order.end(), "DeferredLightingPass");
        const auto overlayIdx = std::find(order.begin(), order.end(), "ForwardOverlayPass");
        ASSERT_NE(sceneIdx, order.end());
        ASSERT_NE(decalIdx, order.end());
        ASSERT_NE(lightingIdx, order.end());
        ASSERT_NE(overlayIdx, order.end());
        EXPECT_LT(sceneIdx - order.begin(), decalIdx - order.begin());
        EXPECT_LT(decalIdx - order.begin(), lightingIdx - order.begin());
        EXPECT_LT(lightingIdx - order.begin(), overlayIdx - order.begin());
    }

    RebuildTopology(graph, /*deferred=*/false);
    (void)graph.ValidateResourceHazards();
    {
        const auto& order = graph.GetExecutionOrder();
        EXPECT_EQ(std::find(order.begin(), order.end(), "DeferredOpaqueDecalPass"), order.end())
            << "DeferredOpaqueDecalPass must not appear in Forward pass order";
        EXPECT_EQ(std::find(order.begin(), order.end(), "DeferredLightingPass"), order.end())
            << "DeferredLightingPass must not appear in Forward pass order";
        EXPECT_EQ(std::find(order.begin(), order.end(), "ForwardOverlayPass"), order.end())
            << "ForwardOverlayPass must not appear in Forward pass order";
    }
}

// =============================================================================
// Three-path coverage: Forward / Forward+ / Deferred.
//
// Forward+ adds a `LightCullingPass` that writes a LightGrid SSBO; ScenePass
// reads that SSBO to select per-tile light lists. Deferred adds the G-Buffer
// lighting + overlay passes AND the DeferredOpaqueDecalPass introduced in
// Phase 2 (PR #216): DecalPass reads SceneDepth + SceneNormals and writes
// back into the G-Buffer (SceneColor / SceneNormals) before lighting.
//
// These tests exercise path transitions beyond the Forward↔Deferred pair the
// original suite covered — they guard against regressions in the Forward+
// and decal edges that Phase 2 made production-required.
// =============================================================================
namespace
{
    inline constexpr std::string_view kLightGridResource = "LightGrid";

    enum class TestPath : u8
    {
        Forward = 0,
        ForwardPlus,
        Deferred
    };

    // Rebuild a richer topology that models Forward+ (LightCullingPass) and
    // the DeferredOpaqueDecalPass insertion point. Decal-edge and light-edge
    // can be individually skipped to test negative/hazard cases.
    void RebuildTopologyMultiPath(RenderGraph& graph, TestPath path,
                                  bool skipDecalEdge = false,
                                  bool skipLightGridEdge = false)
    {
        graph.ResetTopology();

        // LightCulling runs before ScenePass on Forward+. Model it first so
        // the producer's DeclareWrite of LightGrid precedes ScenePass' read.
        if (path == TestPath::ForwardPlus)
        {
            auto lc = AddDeclStub(graph, "LightCullingPass");
            lc->TestDeclareWrite(std::string(kLightGridResource));
        }

        auto scene = AddDeclStub(graph, "ScenePass");
        scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
        if (path == TestPath::ForwardPlus)
            scene->TestDeclareRead(std::string(kLightGridResource));

        if (path == TestPath::Deferred)
        {
            // DeferredOpaqueDecalPass reads depth+normals, writes color+normals
            // back into the G-Buffer before DeferredLightingPass composites.
            auto decal = AddDeclStub(graph, "DeferredOpaqueDecalPass");
            decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            decal->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
            decal->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

            AddDeclStub(graph, "DeferredLightingPass");
            AddDeclStub(graph, "ForwardOverlayPass");
        }

        auto oit = AddDeclStub(graph, "OITResolvePass");
        (void)oit;

        auto fin = AddDeclStub(graph, "FinalPass");
        (void)fin;

        if (path == TestPath::ForwardPlus)
        {
            if (!skipLightGridEdge)
                graph.AddExecutionDependency("LightCullingPass", "ScenePass");
            graph.AddExecutionDependency("ScenePass", "OITResolvePass");
        }
        else if (path == TestPath::Deferred)
        {
            if (!skipDecalEdge)
                graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
            graph.AddExecutionDependency("DeferredLightingPass", "ForwardOverlayPass");
            graph.AddExecutionDependency("ForwardOverlayPass", "OITResolvePass");
        }
        else
        {
            graph.AddExecutionDependency("ScenePass", "OITResolvePass");
        }
        graph.AddExecutionDependency("OITResolvePass", "FinalPass");

        graph.SetFinalPass("FinalPass");
    }
} // namespace

// Full 3-way cycle: Forward → Forward+ → Deferred → Forward+ → Forward, with
// hazard-set and pass-count assertions at every step. Guards against
// ResetTopology leaving stale decal or light-culling passes behind.
TEST(RenderGraphPathSwitch, ThreeWayCycleCleansUpAllPathSpecificPasses)
{
    RenderGraph graph;

    // Forward = ScenePass + OITResolve + Final.
    constexpr sizet kForwardCount = 3u;
    // Forward+ adds LightCullingPass.
    constexpr sizet kForwardPlusCount = 4u;
    // Deferred adds DeferredOpaqueDecalPass + DeferredLightingPass +
    // ForwardOverlayPass (no LightCullingPass).
    constexpr sizet kDeferredCount = 6u;

    struct Step
    {
        TestPath path;
        sizet expected;
    };
    const Step steps[] = {
        { TestPath::Forward, kForwardCount },
        { TestPath::ForwardPlus, kForwardPlusCount },
        { TestPath::Deferred, kDeferredCount },
        { TestPath::ForwardPlus, kForwardPlusCount },
        { TestPath::Forward, kForwardCount },
        { TestPath::Deferred, kDeferredCount },
        { TestPath::Forward, kForwardCount },
    };

    for (sizet i = 0; i < std::size(steps); ++i)
    {
        RebuildTopologyMultiPath(graph, steps[i].path);

        const auto hazards = graph.ValidateResourceHazards();
        EXPECT_TRUE(hazards.empty())
            << "Cycle " << i << " path=" << static_cast<i32>(steps[i].path)
            << ": " << HazardsToString(hazards);

        EXPECT_EQ(graph.GetNodeSubmissionInfo().size(), steps[i].expected)
            << "Cycle " << i << ": pass count drift — ResetTopology leak";

        // Path-specific passes must be present iff path matches.
        const bool isFwdPlus = steps[i].path == TestPath::ForwardPlus;
        const bool isDeferred = steps[i].path == TestPath::Deferred;

        EXPECT_EQ(ContainsPass(graph, "LightCullingPass"), isFwdPlus);
        EXPECT_EQ(ContainsPass(graph, "DeferredOpaqueDecalPass"), isDeferred);
        EXPECT_EQ(ContainsPass(graph, "DeferredLightingPass"), isDeferred);
        EXPECT_EQ(ContainsPass(graph, "ForwardOverlayPass"), isDeferred);
    }
}

// Phase F slice 27: DeferredOpaqueDecalPass.DeclareRead(SceneDepth/SceneNormals)
// + ScenePass.DeclareWrite(...) derives the ordering edge; no explicit
// ScenePass→DeferredOpaqueDecalPass call needed.
TEST(RenderGraphPathSwitch, MissingDecalExplicitEdge_DerivedEdgeSufficient)
{
    RenderGraph graph;
    RebuildTopologyMultiPath(graph, TestPath::Forward);
    RebuildTopologyMultiPath(graph, TestPath::Deferred, /*skipDecalEdge=*/true);

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: ScenePass→DeferredOpaqueDecalPass is derived from declarations."
        << HazardsToString(hazards);
}

// Phase F slice 27: LightCullingPass.DeclareWrite(LightGrid) +
// ScenePass.DeclareRead(LightGrid) derives the ordering edge; no explicit
// LightCullingPass→ScenePass call needed.
TEST(RenderGraphPathSwitch, MissingLightGridExplicitEdge_DerivedEdgeSufficient)
{
    RenderGraph graph;
    RebuildTopologyMultiPath(graph, TestPath::Forward);
    RebuildTopologyMultiPath(graph, TestPath::ForwardPlus, /*skipDecalEdge=*/false, /*skipLightGridEdge=*/true);

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: LightCullingPass→ScenePass is derived from LightGrid declarations."
        << HazardsToString(hazards);
}

// Topological-order invariants for Deferred with decal: decal runs AFTER
// scene and BEFORE lighting; lighting runs BEFORE overlay.
TEST(RenderGraphPathSwitch, DeferredDecalOrderingIsTopologicallyValid)
{
    RenderGraph graph;
    RebuildTopologyMultiPath(graph, TestPath::Deferred);
    (void)graph.ValidateResourceHazards();

    const auto& order = graph.GetExecutionOrder();
    auto indexOf = [&](const char* name) -> i64
    {
        const auto it = std::find(order.begin(), order.end(), name);
        return (it == order.end()) ? -1 : (it - order.begin());
    };

    const i64 scene = indexOf("ScenePass");
    const i64 decal = indexOf("DeferredOpaqueDecalPass");
    const i64 lighting = indexOf("DeferredLightingPass");
    const i64 overlay = indexOf("ForwardOverlayPass");

    ASSERT_GE(scene, 0);
    ASSERT_GE(decal, 0);
    ASSERT_GE(lighting, 0);
    ASSERT_GE(overlay, 0);

    EXPECT_LT(scene, decal) << "Decal must run after ScenePass populates the G-Buffer";
    EXPECT_LT(decal, lighting) << "Decal must run before DeferredLightingPass composites";
    EXPECT_LT(lighting, overlay) << "Overlay runs after DeferredLighting";
}

// =============================================================================
// Determinism: rebuilding the same path twice — whether on a fresh graph or
// after ResetTopology — must produce byte-identical execution orders.
//
// Why this matters even though Kahn's topological sort is deterministic on
// paper: pass registration into `RenderGraph` flows through string-keyed
// containers internally. If any of those become `std::unordered_map`
// (hash-randomized in some STL builds) the iteration order in Kahn's ready-
// queue silently diverges between runs. Symptoms: GPU debugging timelines
// reorder, transient-pool aliasing decisions shift, frame-to-frame stability
// breaks on the *same* scene. This test pins the contract.
// =============================================================================
TEST(RenderGraphDeterminism, ForwardPathBuildOrderIsStableAcrossFreshInstances)
{
    RenderGraph g1;
    RebuildTopology(g1, /*deferred=*/false);
    (void)g1.ValidateResourceHazards(); // triggers UpdateDependencyGraph

    RenderGraph g2;
    RebuildTopology(g2, /*deferred=*/false);
    (void)g2.ValidateResourceHazards();

    EXPECT_EQ(g1.GetExecutionOrder(), g2.GetExecutionOrder())
        << "Forward path built on two fresh RenderGraph instances produced "
        << "different execution orders — Kahn's topological sort is reading "
        << "from a hash-randomized container somewhere.";
}

TEST(RenderGraphDeterminism, DeferredPathBuildOrderIsStableAcrossFreshInstances)
{
    RenderGraph g1;
    RebuildTopology(g1, /*deferred=*/true);
    (void)g1.ValidateResourceHazards();

    RenderGraph g2;
    RebuildTopology(g2, /*deferred=*/true);
    (void)g2.ValidateResourceHazards();

    EXPECT_EQ(g1.GetExecutionOrder(), g2.GetExecutionOrder())
        << "Deferred path built on two fresh RenderGraph instances produced "
        << "different execution orders.";
}

TEST(RenderGraphDeterminism, RebuildAfterResetMatchesFreshBuild)
{
    // Same-instance rebuild after ResetTopology must match a freshly
    // constructed instance — catches stale state leaks through Reset.
    RenderGraph reused;
    RebuildTopology(reused, /*deferred=*/false);
    RebuildTopology(reused, /*deferred=*/true);
    RebuildTopology(reused, /*deferred=*/false); // back to forward
    (void)reused.ValidateResourceHazards();

    RenderGraph fresh;
    RebuildTopology(fresh, /*deferred=*/false);
    (void)fresh.ValidateResourceHazards();

    EXPECT_EQ(reused.GetExecutionOrder(), fresh.GetExecutionOrder())
        << "Rebuilt-after-reset graph diverges from a fresh-instance build of "
        << "the same path — ResetTopology is leaving state behind.";
}
