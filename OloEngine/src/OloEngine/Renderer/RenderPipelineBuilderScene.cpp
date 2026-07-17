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
        //     -> FluidComposite -> Particle -> OITResolve
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

        // Screen-space fluid (issue #630): intermediates first (name-based
        // dependency discovery is registration-ordered), then the composite
        // joins the SceneColor RMW chain AFTER Water — registering it before
        // Water would compose the fluid under the water surface.
        AddExistingNode(graph, inputs.Passes->FluidIntermediates);
        AddExistingNode(graph, inputs.Passes->FluidComposite);
    }

    void RegisterSceneAndLightingNodes(RenderGraph& graph,
                                       const SceneLightingStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterSceneAndLightingNodes requires pass inputs");
        graph.AddNode(PrepareGraphNode("ShadowPass", inputs.Passes->Shadow));

        // Realtime DDGI probe update (#632): its relight stage samples the
        // CSM/shadow atlas (after ShadowPass), and BOTH lit paths consume the
        // atlases it publishes at TEX_DDGI_* — ScenePass's forward PBR shaders
        // and DeferredLightingPass alike — so it must execute before ScenePass.
        // Self-disables (disabled-UBO upload only) when no Realtime/Hybrid
        // volume was submitted this frame; registered on every path since DDGI
        // is path-agnostic.
        if (inputs.Passes->DDGIProbeUpdate)
        {
            graph.AddNode(PrepareGraphNode("DDGIProbeUpdatePass", inputs.Passes->DDGIProbeUpdate));
        }

        AddExistingNode(graph, inputs.Passes->Scene);

        // Virtualized geometry (#629): DAG-cut cull compute + hardware MDI
        // raster into the borrowed G-Buffer. Registered right after ScenePass
        // (the G-Buffer owner and first writer) and BEFORE decals / AO /
        // lighting so every downstream G-Buffer consumer sees the clusters.
        // Self-disables outside Deferred.
        if (inputs.Deferred && inputs.Passes->VirtualGeometry)
        {
            graph.AddNode(PrepareGraphNode("VirtualGeometryPass", inputs.Passes->VirtualGeometry));
        }

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

        // DeferredGPUOcclusionPass (#486) draws the disoccluded instanced statics
        // into the G-Buffer (phase 2 of the deferred two-phase occlusion cull)
        // and re-exports the G-Buffer attachments. Register it BEFORE
        // DeferredOpaqueDecalPass and AO so its G-Buffer writes are ordered ahead
        // of the decal writes and the AO/lighting reads; the phase-2 HZB is built
        // from ScenePass's depth (occluders + phase-1 survivors), so it must run
        // after ScenePass. Self-disables in Forward / Forward+ (no G-Buffer).
        if (inputs.Deferred && inputs.Passes->DeferredGPUOcclusion)
        {
            graph.AddNode(PrepareGraphNode("DeferredGPUOcclusionPass", inputs.Passes->DeferredGPUOcclusion));
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
