#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3DFrameGraphBuilderInternal.h"

namespace OloEngine
{
    void BuildRenderer3DFrameGraph(const Renderer3DFrameGraphInputs& inputs, RenderingPath path)
    {
        OLO_CORE_ASSERT(inputs.Graph, "BuildRenderer3DFrameGraph requires a valid RenderGraph");
        OLO_CORE_ASSERT(inputs.Settings, "BuildRenderer3DFrameGraph requires renderer settings");
        OLO_CORE_ASSERT(inputs.PostProcess, "BuildRenderer3DFrameGraph requires post-process settings");
        OLO_CORE_ASSERT(inputs.Snow, "BuildRenderer3DFrameGraph requires snow settings");
        OLO_CORE_ASSERT(inputs.Fog, "BuildRenderer3DFrameGraph requires fog settings");
        OLO_CORE_ASSERT(inputs.Precipitation, "BuildRenderer3DFrameGraph requires precipitation settings");

        inputs.Graph->ResetTopology();

        const auto deferred = (path == RenderingPath::Deferred);
        Renderer3DFrameGraphBuilderInternal::RegisterRenderStreamNodes(*inputs.Graph, inputs, deferred);
        Renderer3DFrameGraphBuilderInternal::RegisterSceneAndLightingNodes(*inputs.Graph, inputs, deferred);
        Renderer3DFrameGraphBuilderInternal::RegisterTransparencyAndAONodes(*inputs.Graph, inputs);
        Renderer3DFrameGraphBuilderInternal::RegisterPostProcessNodes(*inputs.Graph, inputs);
        inputs.Graph->SetFinalPass("FinalPass");
    }
} // namespace OloEngine
