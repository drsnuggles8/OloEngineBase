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

        // AO writer (SSAOPass / GTAOPass) is registered earlier in RegisterSceneAndLightingNodes
        // so its AOBuffer write is visible to DeferredLightingPass's read in registration order.
        // AOApply consumes the same AOBuffer here; the name-based predecessor lookup wires it up.
        graph.AddNode(PrepareGraphNode("AOApplyPass", inputs.Passes->AOApply));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
