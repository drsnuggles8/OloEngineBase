#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

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

        // GPU-driven occlusion cull (#431) — runs right after ScenePass so the
        // non-instanced opaque occluders are already in the scene depth, and
        // BEFORE the AO pass so it is the first SceneColor writer in the
        // post-Scene chain. Self-disables in Deferred / when no batches routed.
        if (inputs.Passes->GPUOcclusion)
        {
            graph.AddNode(PrepareGraphNode("GPUDrivenOcclusionPass", inputs.Passes->GPUOcclusion));
        }

        // Planar reflection replays ScenePass's batched opaque bucket from the
        // mirrored camera into its own target (forward path only; self-disables
        // otherwise). Registered right after ScenePass so the bucket is batched;
        // runs before the render-stream WaterPass that samples the result.
        if (inputs.Passes->PlanarReflection)
        {
            graph.AddNode(PrepareGraphNode("PlanarReflectionPass", inputs.Passes->PlanarReflection));
        }

        // DeferredOpaqueDecalPass writes G-Buffer attachments (incl. SceneNormals)
        // so it must finish before AO reads those normals. Register it BEFORE AO
        // so the AO pass's `builder.Read(SceneNormals)` resolves to the final
        // G-Buffer version and no WAR hazard fires.
        if (inputs.Deferred && inputs.Passes->DeferredOpaqueDecal)
        {
            graph.AddNode(PrepareGraphNode("DeferredOpaqueDecalPass", inputs.Passes->DeferredOpaqueDecal));
        }

        // AO writer is registered between the last G-Buffer producer (Scene +
        // DeferredOpaqueDecalPass when deferred) and DeferredLightingPass so the
        // builder's read-from-AOBuffer edge derivation discovers the producer in
        // registration order. DLP's `builder.Read(AOBuffer)` then auto-derives the
        // chain without per-pass DependsOnPass pinning. AOApply is registered
        // later (in RegisterTransparencyAndAONodes) and consumes the same
        // AOBuffer; its name-based predecessor lookup picks up the AO pass naturally.
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

        if (!inputs.Deferred)
            return;

        graph.AddNode(PrepareGraphNode("DeferredLightingPass", inputs.Passes->DeferredLighting));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
