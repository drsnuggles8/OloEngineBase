#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const TransparencyAOStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterTransparencyAndAONodes requires pass inputs");

        // Skin diffusion (issue #1241) runs FIRST in this group, and the
        // position is load bearing in both directions:
        //
        //   * it must be AFTER every opaque scene-colour writer, because it
        //     diffuses the lit diffuse half of a skin pixel and that half does
        //     not exist until the lighting has run — forward geometry or the
        //     deferred lighting pass, both of which are in the earlier group.
        //   * it must be BEFORE the transparents, so a pane of glass or a
        //     particle in front of a face composites over the DIFFUSED head
        //     rather than being smeared into it. Those passes also write the
        //     scene framebuffer's skin-diffuse attachment (with zero, since
        //     nothing but skin has a diffusion hand-off), so running after them
        //     would additionally mean reading an attachment they had already
        //     cleared.
        //
        // It is registered unconditionally and culls itself: the pass claims no
        // resources when its scratch target was not declared, and skips both
        // draws when no authored profile in the frame asks to be diffused.
        graph.AddNode(PrepareGraphNode("SkinDiffusionPass", inputs.Passes->SkinDiffusion));

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

        // GPU-pushable shader debug draws (issue #725). Registered HERE, right
        // after OITResolve — the last SceneColor writer — and BEFORE SSS/AOApply,
        // and the position is load bearing in both directions:
        //
        //   * later than this and it would not work at all. A reader resolves
        //     SceneColor to the last writer registered BEFORE it, so registering
        //     after AOApply would leave AOApply reading the pre-debug version and
        //     the lines would simply never reach the screen.
        //   * earlier than this (in the render-stream group, the natural-looking
        //     home) and the transparents drawn afterwards would composite over
        //     the debug geometry. A debug overlay that particles can hide is not
        //     an overlay.
        //
        // It is also after every pass that can PUSH — the cluster cull, DDGI, the
        // particle/fluid computes, any G-Buffer or forward fragment stage — and
        // after the scene depth is final, which is what lets world-space debug
        // geometry depth-test against real geometry instead of floating over it.
        //
        // Declares nothing when disabled; the enable is hashed into the
        // blackboard fingerprint so flipping it rebuilds the graph.
        if (inputs.Passes->ShaderDebugDraw)
        {
            AddExistingNode(graph, inputs.Passes->ShaderDebugDraw);
        }

        graph.AddNode(PrepareGraphNode("SSSPass", inputs.Passes->SSS));

        // AO writer (SSAOPass / GTAOPass) is registered earlier in RegisterSceneAndLightingNodes
        // so its AOBuffer write is visible to DeferredLightingPass's read in registration order.
        // AOApply consumes the same AOBuffer here; the name-based predecessor lookup wires it up.
        graph.AddNode(PrepareGraphNode("AOApplyPass", inputs.Passes->AOApply));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
