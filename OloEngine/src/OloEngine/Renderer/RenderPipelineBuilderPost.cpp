#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    void RegisterPostProcessNodes(RenderGraph& graph,
                                  const PostProcessStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterPostProcessNodes requires pass inputs");
        // SSGI (deferred-only) adds one-bounce indirect diffuse to the AO-applied
        // scene colour. It runs BEFORE SSR so reflections include the bounced
        // light, and self-skips on the forward path (its SSGIColor resource is
        // never declared).
        if (inputs.Passes->SSGI)
        {
            graph.AddNode(PrepareGraphNode("SSGIPass", inputs.Passes->SSGI));
        }
        // SSR (deferred-only) reflects the AO-applied (and SSGI-lit) scene colour,
        // so it is registered ahead of Bloom. It self-skips on the forward path
        // (its SSRColor resource is never declared).
        if (inputs.Passes->SSR)
        {
            graph.AddNode(PrepareGraphNode("SSRPass", inputs.Passes->SSR));
        }
        // ContactShadow (deferred-only) darkens the SSR/SSGI/AO-lit scene colour
        // with short-range contact shadows for the sun. It runs last in the
        // screen-space chain (after SSR, before Bloom) and self-skips on the
        // forward path (its ContactShadowColor resource is never declared).
        if (inputs.Passes->ContactShadow)
        {
            graph.AddNode(PrepareGraphNode("ContactShadowPass", inputs.Passes->ContactShadow));
        }
        // FSR1 EASU spatial upscale (#480). Runs right after the screen-space
        // band and BEFORE Bloom: it upscales the reduced-resolution HDR scene
        // colour to display res so every downstream display-res post stage runs
        // at full resolution. Self-skips when Upscale == Off (its EASUColor
        // resource is never declared).
        if (inputs.Passes->EASU)
        {
            graph.AddNode(PrepareGraphNode("EASUPass", inputs.Passes->EASU));
        }
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
        // Spatial upscaler / CAS sharpening. Runs on the tonemapped (LDR) image
        // — CAS's contrast term assumes the [0,1] display range — so it sits
        // right after ToneMap and before Vignette. Self-skips when CAS is
        // disabled (its UpscalerColor resource is never declared).
        if (inputs.Passes->Upscaler)
        {
            graph.AddNode(PrepareGraphNode("UpscalerPass",
                                           inputs.Passes->Upscaler));
        }
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
