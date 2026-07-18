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
        // FSR1 depth+velocity upscale (#480). Runs right after EASU (before the
        // display-res post band): nearest-upscales the reduced depth + velocity to
        // full res and swaps the blackboard SceneDepth/Velocity handles so DOF /
        // Fog / MotionBlur / TAA / ToneMap read full-res depth. Self-skips when
        // Upscale == Off (its output resource is never declared).
        if (inputs.Passes->DepthVelocityUpscale)
        {
            graph.AddNode(PrepareGraphNode("DepthVelocityUpscalePass", inputs.Passes->DepthVelocityUpscale));
        }
        graph.AddNode(PrepareGraphNode("BloomPass",
                                       inputs.Passes->Bloom));
        graph.AddNode(PrepareGraphNode("DOFPass",
                                       inputs.Passes->DOF));
        graph.AddNode(PrepareGraphNode("MotionBlurPass",
                                       inputs.Passes->MotionBlur));
        graph.AddNode(PrepareGraphNode("TAAPass",
                                       inputs.Passes->TAA));
        // Volumetric cloudscape (issue #633): half-res raymarch + temporal
        // resolve + full-res composite. Runs after TAA (it consumes the
        // anti-aliased scene colour) and before Precipitation/Fog so screen
        // streaks overlay the clouds and the froxel fog applies aerial
        // perspective over them. Self-skips when disabled (its CloudsColor
        // resource is never declared).
        if (inputs.Passes->Cloudscape)
        {
            graph.AddNode(PrepareGraphNode("CloudscapePass", inputs.Passes->Cloudscape));
        }
        graph.AddNode(PrepareGraphNode("PrecipitationPass",
                                       inputs.Passes->Precipitation));
        // Froxel volumetric fog compute chain (issue #435): builds the 3D
        // in-scatter/transmittance volume the FogPass composites. Registered
        // right before FogPass; orders itself after ScenePass (cluster lists)
        // + ShadowPass (CSM/atlas reads) via its Setup declarations.
        if (inputs.Passes->VolumetricFog)
        {
            graph.AddNode(PrepareGraphNode("VolumetricFogPass", inputs.Passes->VolumetricFog));
        }
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
        // Overdraw heatmap debug view (#519). Runs late — after tone mapping and
        // every LDR effect — so the heat colours reach the viewport undistorted;
        // when active it replaces the composite (UIComposite/Final read its
        // OverdrawColor at top priority). Self-skips when the debug view is off.
        if (inputs.Passes->Overdraw)
        {
            graph.AddNode(PrepareGraphNode("OverdrawPass",
                                           inputs.Passes->Overdraw));
        }
        graph.AddNode(PrepareGraphNode("UICompositePass",
                                       inputs.Passes->UIComposite));
        graph.AddNode(PrepareGraphNode("FinalPass",
                                       inputs.Passes->Final));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
