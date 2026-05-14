#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    void RegisterPostProcessNodes(RenderGraph& graph,
                                  const PostProcessStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterPostProcessNodes requires pass inputs");
        graph.AddNode(PrepareGraphNode("BloomPass",
                                       inputs.Passes->Bloom));
        graph.AddNode(PrepareGraphNode("DOFPass",
                                       inputs.Passes->DOF));
        graph.AddNode(PrepareGraphNode("MotionBlurPass",
                                       inputs.Passes->MotionBlur));
        graph.AddNode(PrepareGraphNode("TAAPass",
                                       inputs.Passes->TAA));
        graph.AddNode(PrepareGraphNode("PrecipitationPass",
                                       inputs.Passes->Precipitation));
        graph.AddNode(PrepareGraphNode("FogPass",
                                       inputs.Passes->Fog));
        graph.AddNode(PrepareGraphNode("ChromAberrationPass",
                                       inputs.Passes->ChromAberration));
        graph.AddNode(PrepareGraphNode("ColorGradingPass",
                                       inputs.Passes->ColorGrading));
        graph.AddNode(PrepareGraphNode("ToneMapPass",
                                       inputs.Passes->ToneMap));
        graph.AddNode(PrepareGraphNode("VignettePass",
                                       inputs.Passes->Vignette));
        if (inputs.Passes->FXAA)
        {
            graph.AddNode(PrepareGraphNode("FXAAPass",
                                           inputs.Passes->FXAA));
        }
        if (inputs.Passes->SelectionOutline)
        {
            graph.AddNode(PrepareGraphNode("SelectionOutlinePass",
                                           inputs.Passes->SelectionOutline));
        }
        graph.AddNode(PrepareGraphNode("UICompositePass",
                                       inputs.Passes->UIComposite));
        graph.AddNode(PrepareGraphNode("FinalPass",
                                       inputs.Passes->Final));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
