#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const TransparencyAOStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterTransparencyAndAONodes requires pass inputs");

        // OIT contributor ordering between Decal (OIT mode) and Particle (OIT
        // mode) is now resolved by Particle's Setup calling
        // builder.DependsOnPreviousWriter("OITAccum") — DecalPass (registered
        // earlier in RegisterRenderStreamNodes) shows up as the previous
        // writer when its own OIT path is taken, and the call is a no-op
        // otherwise.
        graph.AddNode(PrepareGraphNode("ParticlePass", inputs.Passes->Particle));
        graph.AddNode(PrepareGraphNode("OITPreparePass", inputs.Passes->OITPrepare));
        graph.AddNode(PrepareGraphNode("OITResolvePass", inputs.Passes->OITResolve));
        graph.AddNode(PrepareGraphNode("SSSPass", inputs.Passes->SSS));

        switch (inputs.ActiveAOTechnique)
        {
            case AOTechnique::SSAO:
                graph.AddNode(PrepareGraphNode("SSAOPass", inputs.Passes->SSAO));
                break;
            case AOTechnique::GTAO:
                graph.AddNode(PrepareGraphNode("GTAOPass", inputs.Passes->GTAO));
                break;
            case AOTechnique::None:
                break;
        }

        graph.AddNode(PrepareGraphNode("AOApplyPass", inputs.Passes->AOApply));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
