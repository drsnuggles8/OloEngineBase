// OLO_TEST_LAYER: plumbing
// =============================================================================
// VirtualShadowMapMarkPassTest.cpp
//
// One invariant, and it is the bug that cost this task the most time (issue
// #702): VirtualShadowMapMarkPass::Setup must declare its scene-depth read
// REGARDLESS of whether Virtual Shadow Maps happen to be enabled at the moment
// the frame graph is compiled.
//
// WHY THAT IS A REAL RULE AND NOT DEFENSIVE TIDYING. RenderGraph::BuildFrameGraph
// caches the compiled frame behind a fingerprint, and that fingerprint does not
// include the VSM setting. Setup therefore runs on the frame the topology was
// last rebuilt and not when the setting is toggled. A Setup that early-returns
// while VSM is off freezes that decision in for the life of the topology: the
// pass keeps being executed, its scene-depth handle stays invalid, Execute bails
// on it every frame, no page is ever marked, nothing is ever allocated — and the
// frame renders completely unshadowed with no error anywhere, while IsActive()
// reports true and the physical pool sits there fully allocated.
//
// The tell is nasty: VSM produced a correct shadow when it was enabled BEFORE the
// first frame and none when it was switched on afterwards — which is the order
// the editor's checkbox uses, and the order the CSM-then-VSM comparison test
// uses. Two tests exercising identical code disagreed.
//
// This test is deliberately HEADLESS. VirtualShadowMapVisualEvidenceTest catches
// the same regression end-to-end, but it needs a GL 4.6 context and SKIPs on CI,
// so the invariant would otherwise be guarded only on a GPU box. Neither
// RenderGraph nor ShadowMap touches the GPU to construct, so this one runs
// everywhere and fails in milliseconds.
//
// See docs/agent-rules/virtual-shadow-map-page-cache.md §5 — the rule is general
// to any render-graph pass with a runtime toggle, not specific to VSM.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/VirtualShadowMapMarkPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace OloEngine::Tests
{
    namespace
    {
        // Mints a blackboard whose SceneDepth names a real (imported) graph
        // resource, which is the only precondition the pass's Setup has.
        [[nodiscard]] RGTextureHandle ImportSceneDepth(RenderGraph& graph)
        {
            return graph.ImportTexture(
                ResourceNames::SceneDepth, 1u,
                RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, ResourceNames::SceneDepth));
        }

        [[nodiscard]] bool DeclaresSceneDepthRead(bool virtualShadowMapsEnabled)
        {
            RenderGraph graph;
            FrameBlackboard blackboard{};
            blackboard.Scene.SceneDepth = ImportSceneDepth(graph);

            ShadowMap shadowMap;
            ShadowSettings settings = shadowMap.GetSettings();
            settings.VSM.Enabled = virtualShadowMapsEnabled;
            shadowMap.SetSettings(settings);

            VirtualShadowMapMarkPass pass;
            pass.SetShadowMap(&shadowMap);

            RGBuilder builder(graph, blackboard);
            builder.BeginPass("VirtualShadowMapMarkPass");
            pass.Setup(builder, blackboard);

            const auto& reads = builder.GetDeclaredReads();
            return std::ranges::find(reads, std::string(ResourceNames::SceneDepth)) != reads.end();
        }
    } // namespace

    // THE invariant. Note the expectation is the same for both inputs on purpose:
    // "declare always, gate in Execute" is the rule, so an implementation that
    // declared only when enabled would satisfy the second case and fail the first.
    TEST(VirtualShadowMapMarkPassSetup, DeclaresSceneDepthReadWhileVirtualShadowMapsAreOFF)
    {
        EXPECT_TRUE(DeclaresSceneDepthRead(false))
            << "Setup() skipped its scene-depth read because VSM is currently off. "
               "BuildFrameGraph caches the compiled frame behind a fingerprint that does NOT include "
               "the VSM setting, so this decision is frozen in for the life of the topology: enabling "
               "VSM afterwards can never re-run Setup, the pass's depth handle stays invalid, and no "
               "page is ever marked — an unshadowed frame with no error anywhere. Declare the read "
               "unconditionally and gate the WORK in Execute(), which does run every frame.";
    }

    TEST(VirtualShadowMapMarkPassSetup, DeclaresSceneDepthReadWhileVirtualShadowMapsAreON)
    {
        EXPECT_TRUE(DeclaresSceneDepthRead(true))
            << "Setup() failed to declare its scene-depth read even with VSM enabled — the pass "
               "cannot mark any page without the depth buffer";
    }

    // The other half of the contract: IsEnabled() is consulted at TOPOLOGY-build
    // time, so it must answer "could this pass ever run", not "is the feature on".
    // An IsEnabled() that tracked the live setting got the node culled out of the
    // graph entirely — the same failure, one layer up.
    TEST(VirtualShadowMapMarkPassSetup, IsEnabledDoesNotTrackTheLiveVirtualShadowMapSetting)
    {
        ShadowMap shadowMap;
        VirtualShadowMapMarkPass pass;
        pass.SetShadowMap(&shadowMap);

        ShadowSettings settings = shadowMap.GetSettings();
        settings.VSM.Enabled = false;
        shadowMap.SetSettings(settings);
        const bool enabledWhileOff = pass.IsEnabled();

        settings.VSM.Enabled = true;
        shadowMap.SetSettings(settings);
        const bool enabledWhileOn = pass.IsEnabled();

        EXPECT_EQ(enabledWhileOff, enabledWhileOn)
            << "IsEnabled() changed with the VSM setting. The graph caches its topology, so a node "
               "that reports 'disabled' at build time is culled and never comes back when the "
               "setting is flipped.";
        EXPECT_TRUE(enabledWhileOff)
            << "the pass must report enabled whenever it has a ShadowMap to work with";
    }

    // Without a ShadowMap the pass has nothing to drive and must say so — this is
    // the one thing IsEnabled() is allowed to depend on, because it cannot change
    // after the pass is wired up.
    TEST(VirtualShadowMapMarkPassSetup, IsDisabledWithoutAShadowMap)
    {
        const VirtualShadowMapMarkPass pass;
        EXPECT_FALSE(pass.IsEnabled());
    }
} // namespace OloEngine::Tests
