#pragma once

#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine
{
    class RenderGraph;
    class PassGraphNode;
    class SceneRenderPass;
    class ShadowRenderPass;
    class DeferredLightingPass;
    class DeferredOpaqueDecalPass;
    class WaterRenderPass;
    class DecalRenderPass;
    class SSAORenderPass;
    class GTAORenderPass;
    class ParticleRenderPass;
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

    struct Renderer3DFrameGraphInputs
    {
        RenderGraph* Graph = nullptr;
        PassGraphNode* GeometryNode = nullptr;
        PassGraphNode* ForwardOverlayNode = nullptr;
        PassGraphNode* FoliageNode = nullptr;
        PassGraphNode* DecalNode = nullptr;
        PassGraphNode* WaterNode = nullptr;

        SceneRenderPass* ScenePass = nullptr;
        ShadowRenderPass* ShadowPass = nullptr;
        DeferredLightingPass* DeferredLightPass = nullptr;
        DeferredOpaqueDecalPass* OpaqueDecalPass = nullptr;
        WaterRenderPass* WaterPass = nullptr;
        DecalRenderPass* DecalPass = nullptr;
        SSAORenderPass* SSAOPass = nullptr;
        GTAORenderPass* GTAOPass = nullptr;
        ParticleRenderPass* ParticlePass = nullptr;
        OITResolveRenderPass* OITResolvePass = nullptr;
        SSSRenderPass* SSSPass = nullptr;
        AOApplyRenderPass* AOApplyPass = nullptr;
        BloomRenderPass* BloomPass = nullptr;
        DOFRenderPass* DOFPass = nullptr;
        MotionBlurRenderPass* MotionBlurPass = nullptr;
        TAARenderPass* TAAPass = nullptr;
        PrecipitationRenderPass* PrecipitationPass = nullptr;
        FogRenderPass* FogPass = nullptr;
        ChromaticAberrationRenderPass* ChromAberrationPass = nullptr;
        ColorGradingRenderPass* ColorGradingPass = nullptr;
        ToneMapRenderPass* ToneMapPass = nullptr;
        VignetteRenderPass* VignettePass = nullptr;
        FXAARenderPass* FXAAPass = nullptr;
        SelectionOutlineRenderPass* SelectionOutlinePass = nullptr;
        UICompositeRenderPass* UICompositePass = nullptr;
        FinalRenderPass* FinalPass = nullptr;

        RendererSettings* Settings = nullptr;
        PostProcessSettings* PostProcess = nullptr;
        SnowSettings* Snow = nullptr;
        FogSettings* Fog = nullptr;
        PrecipitationSettings* Precipitation = nullptr;
        bool* EnableSelectionOutline = nullptr;
    };

    void BuildRenderer3DFrameGraph(const Renderer3DFrameGraphInputs& inputs, RenderingPath path);
} // namespace OloEngine
