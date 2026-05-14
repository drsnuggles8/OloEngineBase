#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

namespace OloEngine
{
    void BuildRenderPipelineGraph(const RenderPipelineInputs& inputs, RenderingPath path)
    {
        OLO_CORE_ASSERT(inputs.Graph, "BuildRenderPipelineGraph requires a valid RenderGraph");

        inputs.Graph->ResetTopology();

        const auto deferred = (path == RenderingPath::Deferred);
        // SceneAndLightingNodes registers Shadow + Scene (+ deferred lighting),
        // so it must run before RenderStreamNodes registers the SceneColor RMW
        // modifiers (ForwardOverlay/Foliage/Decal/Water). With that ordering,
        // each modifier's Setup can call builder.DependsOnPreviousWriter("SceneColor")
        // to discover its predecessor by name instead of needing the builder
        // to wire typed pass pointers via class-specific setters.
        RenderPipelineBuilderInternal::RegisterSceneAndLightingNodes(*inputs.Graph,
                                                                     RenderPipelineBuilderInternal::MakeSceneLightingStageInputs(inputs, deferred));
        RenderPipelineBuilderInternal::RegisterRenderStreamNodes(*inputs.Graph,
                                                                 RenderPipelineBuilderInternal::MakeRenderStreamStageInputs(inputs, deferred));
        RenderPipelineBuilderInternal::RegisterTransparencyAndAONodes(*inputs.Graph,
                                                                      RenderPipelineBuilderInternal::MakeTransparencyAOStageInputs(inputs));
        RenderPipelineBuilderInternal::RegisterPostProcessNodes(*inputs.Graph,
                                                                RenderPipelineBuilderInternal::MakePostProcessStageInputs(inputs));
        inputs.Graph->SetFinalPass("FinalPass");
    }
} // namespace OloEngine
