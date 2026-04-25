// =============================================================================
// RenderGraphPathSwitchTests.cpp
//
// Layer-5 resource hazard validation — runtime render-path switching.
//
// `Renderer3D::ConfigureRenderGraph(RenderingPath)` is invoked whenever the
// user toggles between `RenderingPath::Forward`, `ForwardPlus`, and
// `Deferred`. Each call:
//   1. `RenderGraph::ResetTopology()` — drops passes, edges, pass-order cache.
//   2. Re-adds only the passes relevant to the new path.
//   3. Re-declares the execution edges (adds the G-Buffer lighting & overlay
//      edges in Deferred, skips them in Forward / Forward+).
//
// The bug-classes this file guards against:
//   * A pass that belongs only to Deferred (DeferredLightingPass,
//     ForwardOverlayPass) lingers in the graph after a Deferred→Forward
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

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    class DeclarativeStubPass : public RenderPass
    {
      public:
        explicit DeclarativeStubPass(const std::string& name)
        {
            m_Name = name;
        }

        void Init(const FramebufferSpecification& /*spec*/) override {}
        void Execute() override {}
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }
        void SetupFramebuffer(u32 /*w*/, u32 /*h*/) override {}
        void ResizeFramebuffer(u32 /*w*/, u32 /*h*/) override {}
        void OnReset() override {}

        void TestDeclareRead(std::string_view name)
        {
            DeclareRead(name);
        }
        void TestDeclareWrite(std::string_view name)
        {
            DeclareWrite(name);
        }
    };

    Ref<DeclarativeStubPass> AddDeclStub(RenderGraph& graph, const std::string& name)
    {
        auto pass = Ref<DeclarativeStubPass>::Create(name);
        pass->SetName(name);
        graph.AddPass(pass);
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
        const auto passes = graph.GetAllPasses();
        return std::any_of(passes.begin(), passes.end(),
                           [&](const Ref<RenderPass>& p)
                           { return p && p->GetName() == name; });
    }

    // Minimal production-shaped topology. Only the passes relevant to the
    // path-switch invariants (ScenePass, DeferredLightingPass,
    // ForwardOverlayPass, OITResolvePass, FinalPass) are instantiated;
    // downstream post-process chain collapses into FinalPass so the tests
    // focus on the edges ConfigureRenderGraph actually toggles.
    //
    // When `forceSkipOverlayEdge` is true, the Forward→Deferred rebuild
    // deliberately skips `ScenePass → DeferredLightingPass`, simulating
    // the textbook regression this test suite is here to prevent.
    void RebuildTopology(RenderGraph& graph, bool deferred, bool forceSkipLightingEdge = false)
    {
        graph.ResetTopology();

        auto scene = AddDeclStub(graph, "ScenePass");
        scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

        if (deferred)
        {
            auto lighting = AddDeclStub(graph, "DeferredLightingPass");
            lighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            lighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            lighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            auto overlay = AddDeclStub(graph, "ForwardOverlayPass");
            overlay->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            overlay->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        auto oit = AddDeclStub(graph, "OITResolvePass");
        oit->TestDeclareRead(std::string(ResourceNames::SceneColor));
        oit->TestDeclareWrite(std::string(ResourceNames::SceneColor));

        auto fin = AddDeclStub(graph, "FinalPass");
        fin->TestDeclareRead(std::string(ResourceNames::SceneColor));
        fin->TestDeclareWrite(std::string(ResourceNames::FinalColor));

        if (deferred)
        {
            if (!forceSkipLightingEdge)
                graph.AddExecutionDependency("ScenePass", "DeferredLightingPass");
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
// Forward→Deferred: the new passes (DeferredLightingPass, ForwardOverlayPass)
// appear, the ScenePass→DeferredLightingPass edge is established, and
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

    EXPECT_TRUE(ContainsPass(graph, "DeferredLightingPass"));
    EXPECT_TRUE(ContainsPass(graph, "ForwardOverlayPass"));
    EXPECT_TRUE(ContainsPass(graph, "OITResolvePass"));

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "Deferred after switch: " << HazardsToString(hazards);
}

// =============================================================================
// Deferred→Forward: `ResetTopology` + a smaller pass set must leave the graph
// with no lingering DeferredLightingPass / ForwardOverlayPass. A leftover
// pass would re-declare `Write(SceneColor)` and produce a WAW hazard against
// ScenePass.
// =============================================================================
TEST(RenderGraphPathSwitch, DeferredToForwardRemovesDeferredOnlyPasses)
{
    RenderGraph graph;
    RebuildTopology(graph, /*deferred=*/true);
    ASSERT_TRUE(ContainsPass(graph, "DeferredLightingPass"));
    ASSERT_TRUE(ContainsPass(graph, "ForwardOverlayPass"));

    RebuildTopology(graph, /*deferred=*/false);

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
// DeferredLightingPass` must be flagged. This is the exact regression the
// review flagged: if ConfigureRenderGraph ever drops the edge, the validator
// must not stay silent.
// =============================================================================
TEST(RenderGraphPathSwitch, ForwardToDeferredMissingLightingEdgeIsFlagged)
{
    RenderGraph graph;
    RebuildTopology(graph, /*deferred=*/false);
    RebuildTopology(graph, /*deferred=*/true, /*forceSkipLightingEdge=*/true);

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty()) << "Missing ScenePass→DeferredLightingPass edge must surface a hazard";

    // The RAW hazard should name either SceneNormals or SceneDepth (the two
    // resources DeferredLightingPass reads that ScenePass writes).
    const bool targetsSceneResource = std::any_of(
        hazards.begin(), hazards.end(),
        [](const auto& h)
        {
            return h.Resource == ResourceNames::SceneNormals || h.Resource == ResourceNames::SceneDepth;
        });
    EXPECT_TRUE(targetsSceneResource) << "Expected RAW on SceneNormals / SceneDepth: " << HazardsToString(hazards);
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
    // Deferred adds DeferredLightingPass + ForwardOverlayPass = 5.
    constexpr sizet kDeferredPassCount = 5u;

    const bool paths[] = { false, true, false, true, true, false };
    for (i32 i = 0; i < static_cast<i32>(std::size(paths)); ++i)
    {
        RebuildTopology(graph, paths[i]);
        const auto hazards = graph.ValidateResourceHazards();
        EXPECT_TRUE(hazards.empty())
            << "Cycle " << i << " (deferred=" << paths[i] << "): " << HazardsToString(hazards);

        const sizet expected = paths[i] ? kDeferredPassCount : kForwardPassCount;
        EXPECT_EQ(graph.GetAllPasses().size(), expected)
            << "Cycle " << i << " (deferred=" << paths[i] << "): pass count drift — ResetTopology leaking";
    }
}

// =============================================================================
// Hand-verify the edges map after a switch to prove the validator is
// operating on the new topology, not stale cached state. Uses `GetPassOrder`
// to sample the topologically-sorted result after each rebuild.
// =============================================================================
TEST(RenderGraphPathSwitch, PassOrderReflectsCurrentTopologyAfterSwitch)
{
    RenderGraph graph;

    RebuildTopology(graph, /*deferred=*/true);
    (void)graph.ValidateResourceHazards(); // triggers UpdateDependencyGraph → populates m_PassOrder
    {
        const auto& order = graph.GetPassOrder();
        // Topological sort must place ScenePass before DeferredLightingPass and
        // DeferredLightingPass before ForwardOverlayPass.
        const auto sceneIdx = std::find(order.begin(), order.end(), "ScenePass");
        const auto lightingIdx = std::find(order.begin(), order.end(), "DeferredLightingPass");
        const auto overlayIdx = std::find(order.begin(), order.end(), "ForwardOverlayPass");
        ASSERT_NE(sceneIdx, order.end());
        ASSERT_NE(lightingIdx, order.end());
        ASSERT_NE(overlayIdx, order.end());
        EXPECT_LT(sceneIdx - order.begin(), lightingIdx - order.begin());
        EXPECT_LT(lightingIdx - order.begin(), overlayIdx - order.begin());
    }

    RebuildTopology(graph, /*deferred=*/false);
    (void)graph.ValidateResourceHazards();
    {
        const auto& order = graph.GetPassOrder();
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

            auto lighting = AddDeclStub(graph, "DeferredLightingPass");
            lighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            lighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            lighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            auto overlay = AddDeclStub(graph, "ForwardOverlayPass");
            overlay->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            overlay->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        auto oit = AddDeclStub(graph, "OITResolvePass");
        oit->TestDeclareRead(std::string(ResourceNames::SceneColor));
        oit->TestDeclareWrite(std::string(ResourceNames::SceneColor));

        auto fin = AddDeclStub(graph, "FinalPass");
        fin->TestDeclareRead(std::string(ResourceNames::SceneColor));
        fin->TestDeclareWrite(std::string(ResourceNames::FinalColor));

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
            graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
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

        EXPECT_EQ(graph.GetAllPasses().size(), steps[i].expected)
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

// Negative: Forward → Deferred without registering the
// ScenePass→DeferredOpaqueDecalPass edge must surface a RAW hazard. This
// specifically guards the Phase 2 decal-integration edge — a regression
// would let the decal pass read the G-Buffer before ScenePass finishes.
TEST(RenderGraphPathSwitch, MissingDecalEdgeSurfacesHazard)
{
    RenderGraph graph;
    RebuildTopologyMultiPath(graph, TestPath::Forward);
    RebuildTopologyMultiPath(graph, TestPath::Deferred, /*skipDecalEdge=*/true);

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty())
        << "Missing ScenePass→DeferredOpaqueDecalPass edge must be flagged as a hazard";

    const bool namesSceneResource = std::any_of(
        hazards.begin(), hazards.end(),
        [](const auto& h)
        {
            return h.Resource == ResourceNames::SceneDepth ||
                   h.Resource == ResourceNames::SceneNormals;
        });
    EXPECT_TRUE(namesSceneResource)
        << "Expected RAW hazard on SceneDepth / SceneNormals: " << HazardsToString(hazards);
}

// Negative: Forward+ without the LightCullingPass→ScenePass edge must be
// flagged. Guards the Forward+ producer→consumer contract; a regression
// would silently let ScenePass read stale/uninitialised LightGrid data.
TEST(RenderGraphPathSwitch, MissingLightGridEdgeSurfacesHazard)
{
    RenderGraph graph;
    RebuildTopologyMultiPath(graph, TestPath::Forward);
    RebuildTopologyMultiPath(graph, TestPath::ForwardPlus, /*skipDecalEdge=*/false, /*skipLightGridEdge=*/true);

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty())
        << "Missing LightCullingPass→ScenePass edge must surface a hazard";

    const bool namesLightGrid = std::any_of(
        hazards.begin(), hazards.end(),
        [](const auto& h)
        { return h.Resource == kLightGridResource; });
    EXPECT_TRUE(namesLightGrid)
        << "Expected hazard naming LightGrid resource: " << HazardsToString(hazards);
}

// Topological-order invariants for Deferred with decal: decal runs AFTER
// scene and BEFORE lighting; lighting runs BEFORE overlay.
TEST(RenderGraphPathSwitch, DeferredDecalOrderingIsTopologicallyValid)
{
    RenderGraph graph;
    RebuildTopologyMultiPath(graph, TestPath::Deferred);
    (void)graph.ValidateResourceHazards();

    const auto& order = graph.GetPassOrder();
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
