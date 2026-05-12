#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    void RegisterRenderStreamNodes(RenderGraph& graph,
                                   const RenderStreamStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterRenderStreamNodes requires pass inputs");

        // The SceneColor RMW chain is now pinned by each modifier's Setup
        // calling builder.DependsOnPreviousWriter("SceneColor") — no
        // class-specific setter wiring is required here. The chain is:
        //   Scene (forward) OR DeferredLighting (deferred)
        //     -> ForwardOverlay (deferred only) -> Foliage -> Decal -> Water
        //     -> Particle -> OITResolve
        // BuildRenderPipelineGraph registers the SceneColor producer
        // (Scene / DeferredLighting) before this stage, so the modifier
        // Setups run in the correct order and the name-based lookup
        // resolves each predecessor.
        if (inputs.Deferred)
        {
            AddExistingNode(graph, inputs.Passes->ForwardOverlay);
        }

        AddExistingNode(graph, inputs.Passes->Foliage);
        AddExistingNode(graph, inputs.Passes->Decal);
        AddExistingNode(graph, inputs.Passes->Water);
    }

    void RegisterSceneAndLightingNodes(RenderGraph& graph,
                                       const SceneLightingStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterSceneAndLightingNodes requires pass inputs");
        graph.AddNode(PrepareGraphNode("ShadowPass", inputs.Passes->Shadow));
        AddExistingNode(graph, inputs.Passes->Scene);

        if (!inputs.Deferred)
            return;

        if (inputs.Passes->DeferredOpaqueDecal)
        {
            graph.AddNode(PrepareGraphNode("DeferredOpaqueDecalPass", inputs.Passes->DeferredOpaqueDecal));
        }
        graph.AddNode(PrepareGraphNode("DeferredLightingPass", inputs.Passes->DeferredLighting));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
