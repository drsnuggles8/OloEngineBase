#pragma once

#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine
{
    class RenderGraph;
    class CommandBufferRenderPass;
    class SceneRenderPass;
    class ShadowRenderPass;
    class DeferredLightingPass;
    class DeferredOpaqueDecalPass;
    class WaterRenderPass;
    class DecalRenderPass;
    class SSAORenderPass;
    class GTAORenderPass;
    class ParticleRenderPass;
    class OITPrepareRenderPass;
    class OITResolveRenderPass;
    class SSSRenderPass;
    class AOApplyRenderPass;
    class BloomRenderPass;
    class DOFRenderPass;
    class MotionBlurRenderPass;
    class TAARenderPass;
    class PrecipitationRenderPass;
    class FogRenderPass;
    class ChromaticAberrationRenderPass;
    class ColorGradingRenderPass;
    class ToneMapRenderPass;
    class VignetteRenderPass;
    class FXAARenderPass;
    class SelectionOutlineRenderPass;
    class UICompositeRenderPass;
    class FinalRenderPass;
    struct RendererSettings;
    struct PostProcessSettings;
    struct SnowSettings;
    struct FogSettings;
    struct PrecipitationSettings;

    struct RenderPipelineNodeInputs
    {
        CommandBufferRenderPass* Geometry = nullptr;
        CommandBufferRenderPass* ForwardOverlay = nullptr;
        CommandBufferRenderPass* Foliage = nullptr;
        CommandBufferRenderPass* Decal = nullptr;
        CommandBufferRenderPass* Water = nullptr;
    };

    struct RenderPipelinePassInputs
    {
        SceneRenderPass* Scene = nullptr;
        ShadowRenderPass* Shadow = nullptr;
        DeferredLightingPass* DeferredLighting = nullptr;
        DeferredOpaqueDecalPass* DeferredOpaqueDecal = nullptr;
        WaterRenderPass* Water = nullptr;
        DecalRenderPass* Decal = nullptr;
        SSAORenderPass* SSAO = nullptr;
        GTAORenderPass* GTAO = nullptr;
        ParticleRenderPass* Particle = nullptr;
        OITPrepareRenderPass* OITPrepare = nullptr;
        OITResolveRenderPass* OITResolve = nullptr;
        SSSRenderPass* SSS = nullptr;
        AOApplyRenderPass* AOApply = nullptr;
        BloomRenderPass* Bloom = nullptr;
        DOFRenderPass* DOF = nullptr;
        MotionBlurRenderPass* MotionBlur = nullptr;
        TAARenderPass* TAA = nullptr;
        PrecipitationRenderPass* Precipitation = nullptr;
        FogRenderPass* Fog = nullptr;
        ChromaticAberrationRenderPass* ChromAberration = nullptr;
        ColorGradingRenderPass* ColorGrading = nullptr;
        ToneMapRenderPass* ToneMap = nullptr;
        VignetteRenderPass* Vignette = nullptr;
        FXAARenderPass* FXAA = nullptr;
        SelectionOutlineRenderPass* SelectionOutline = nullptr;
        UICompositeRenderPass* UIComposite = nullptr;
        FinalRenderPass* Final = nullptr;
    };

    struct RenderPipelineRuntimeInputs
    {
        RendererSettings* Renderer = nullptr;
        PostProcessSettings* PostProcess = nullptr;
        SnowSettings* Snow = nullptr;
        FogSettings* Fog = nullptr;
        PrecipitationSettings* Precipitation = nullptr;
    };

    struct RenderPipelineInputs
    {
        RenderGraph* Graph = nullptr;
        RenderPipelineNodeInputs Nodes;
        RenderPipelinePassInputs Passes;
        RenderPipelineRuntimeInputs Runtime;
    };

    void BuildRenderPipelineGraph(const RenderPipelineInputs& inputs, RenderingPath path);
} // namespace OloEngine
