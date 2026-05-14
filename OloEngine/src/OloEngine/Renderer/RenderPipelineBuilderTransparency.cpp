#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const TransparencyAOStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterTransparencyAndAONodes requires pass inputs");

        // OIT pass ordering:
        //   OITPreparePass must run BEFORE any OIT contributor (Particle,
        //   Decal) so the accum/revealage attachments are cleared and the
        //   depth is seeded before transparents write. Previously Prepare
        //   was registered AFTER Particle, with a `DependsOnPass` edge
        //   inside `if (m_OITEnabled)` papering over the order. Register in
        //   the correct execution order so the dependency edges are an
        //   invariant of the graph topology rather than a runtime gate.
        //
        //   OIT contributor ordering between Decal (OIT mode) and Particle
        //   (OIT mode) is still resolved by Particle's Setup calling
        //   builder.DependsOnPreviousWriter("OITAccum").
        graph.AddNode(PrepareGraphNode("OITPreparePass", inputs.Passes->OITPrepare));
        graph.AddNode(PrepareGraphNode("ParticlePass", inputs.Passes->Particle));
        graph.AddNode(PrepareGraphNode("OITResolvePass", inputs.Passes->OITResolve));
        graph.AddNode(PrepareGraphNode("SSSPass", inputs.Passes->SSS));

        // AO writer (SSAOPass / GTAOPass) is registered earlier in RegisterSceneAndLightingNodes
        // so its AOBuffer write is visible to DeferredLightingPass's read in registration order.
        // AOApply consumes the same AOBuffer here; the name-based predecessor lookup wires it up.
        graph.AddNode(PrepareGraphNode("AOApplyPass", inputs.Passes->AOApply));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
