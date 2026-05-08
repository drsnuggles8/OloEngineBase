#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

namespace OloEngine
{
    void BuildRenderPipelineGraph(const RenderPipelineInputs& inputs, RenderingPath path)
    {
        OLO_CORE_ASSERT(inputs.Graph, "BuildRenderPipelineGraph requires a valid RenderGraph");
        OLO_CORE_ASSERT(inputs.Runtime.Renderer, "BuildRenderPipelineGraph requires renderer settings");
        OLO_CORE_ASSERT(inputs.Runtime.PostProcess, "BuildRenderPipelineGraph requires post-process settings");
        OLO_CORE_ASSERT(inputs.Runtime.Snow, "BuildRenderPipelineGraph requires snow settings");
        OLO_CORE_ASSERT(inputs.Runtime.Fog, "BuildRenderPipelineGraph requires fog settings");
        OLO_CORE_ASSERT(inputs.Runtime.Precipitation, "BuildRenderPipelineGraph requires precipitation settings");

        inputs.Graph->ResetTopology();

        const auto deferred = (path == RenderingPath::Deferred);
        RenderPipelineBuilderInternal::RegisterRenderStreamNodes(*inputs.Graph,
                                                                 RenderPipelineBuilderInternal::MakeRenderStreamStageInputs(inputs, deferred));
        RenderPipelineBuilderInternal::RegisterSceneAndLightingNodes(*inputs.Graph,
                                                                     RenderPipelineBuilderInternal::MakeSceneLightingStageInputs(inputs, deferred));
        RenderPipelineBuilderInternal::RegisterTransparencyAndAONodes(*inputs.Graph,
                                                                      RenderPipelineBuilderInternal::MakeTransparencyAOStageInputs(inputs));
        RenderPipelineBuilderInternal::RegisterPostProcessNodes(*inputs.Graph,
                                                                RenderPipelineBuilderInternal::MakePostProcessStageInputs(inputs));
        inputs.Graph->SetFinalPass("FinalPass");
    }
} // namespace OloEngine
