#pragma once

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"

namespace OloEngine
{
    struct RenderPipelineInputs;

    struct Renderer3D::RenderStreamNodes
    {
        Ref<CommandBufferRenderPass> Geometry;
        Ref<CommandBufferRenderPass> ForwardOverlay;
        Ref<CommandBufferRenderPass> Foliage;
        Ref<CommandBufferRenderPass> Water;
        Ref<CommandBufferRenderPass> Decal;

        [[nodiscard]] auto Get(RenderStreamType stream) -> CommandBufferRenderPass*
        {
            switch (stream)
            {
                case RenderStreamType::Geometry:
                    return Geometry.Raw();
                case RenderStreamType::ForwardOverlay:
                    return ForwardOverlay.Raw();
                case RenderStreamType::Foliage:
                    return Foliage.Raw();
                case RenderStreamType::Water:
                    return Water.Raw();
                case RenderStreamType::Decal:
                    return Decal.Raw();
            }

            return nullptr;
        }

        void ForEach(auto&& func)
        {
            func(Geometry.Raw());
            func(ForwardOverlay.Raw());
            func(Foliage.Raw());
            func(Water.Raw());
            func(Decal.Raw());
        }

        void Reset()
        {
            Geometry.Reset();
            ForwardOverlay.Reset();
            Foliage.Reset();
            Water.Reset();
            Decal.Reset();
        }
    };

    struct Renderer3D::PostProcessPassChain
    {
        Ref<SSSRenderPass> SSS;
        Ref<AOApplyRenderPass> AOApply;
        Ref<BloomRenderPass> Bloom;
        Ref<DOFRenderPass> DOF;
        Ref<MotionBlurRenderPass> MotionBlur;
        Ref<TAARenderPass> TAA;
        Ref<PrecipitationRenderPass> Precipitation;
        Ref<FogRenderPass> Fog;
        Ref<ChromaticAberrationRenderPass> ChromAberration;
        Ref<ColorGradingRenderPass> ColorGrading;
        Ref<ToneMapRenderPass> ToneMap;
        Ref<VignetteRenderPass> Vignette;
        Ref<FXAARenderPass> FXAA;
        Ref<SelectionOutlineRenderPass> SelectionOutline;
        Ref<UICompositeRenderPass> UIComposite;
        Ref<FinalRenderPass> Final;

        void Reset()
        {
            SSS.Reset();
            AOApply.Reset();
            Bloom.Reset();
            DOF.Reset();
            MotionBlur.Reset();
            TAA.Reset();
            Precipitation.Reset();
            Fog.Reset();
            ChromAberration.Reset();
            ColorGrading.Reset();
            ToneMap.Reset();
            Vignette.Reset();
            FXAA.Reset();
            SelectionOutline.Reset();
            UIComposite.Reset();
            Final.Reset();
        }
    };

    struct Renderer3D::SceneCompositionPassSet
    {
        Ref<DeferredLightingPass> DeferredLighting;
        Ref<DeferredOpaqueDecalPass> DeferredOpaqueDecal;
        Ref<SSAORenderPass> SSAO;
        Ref<GTAORenderPass> GTAO;
        Ref<ParticleRenderPass> Particle;
        Ref<OITPrepareRenderPass> OITPrepare;
        Ref<OITResolveRenderPass> OITResolve;

        void Reset()
        {
            DeferredLighting.Reset();
            DeferredOpaqueDecal.Reset();
            SSAO.Reset();
            GTAO.Reset();
            Particle.Reset();
            OITPrepare.Reset();
            OITResolve.Reset();
        }
    };

    struct Renderer3D::FrameCorePassSet
    {
        Ref<ShadowRenderPass> Shadow;
        Ref<SceneRenderPass> Scene;

        void Reset()
        {
            Shadow.Reset();
            Scene.Reset();
        }
    };

    struct Renderer3D::RenderStreamPassSet
    {
        Ref<ForwardOverlayRenderPass> ForwardOverlay;
        Ref<FoliageRenderPass> Foliage;
        Ref<WaterRenderPass> Water;
        Ref<DecalRenderPass> Decal;

        void Reset()
        {
            ForwardOverlay.Reset();
            Foliage.Reset();
            Water.Reset();
            Decal.Reset();
        }
    };

    struct Renderer3D::RenderPipeline
    {
        RenderStreamNodes StreamNodes;
        FrameCorePassSet FrameCorePasses;
        SceneCompositionPassSet SceneCompositePasses;
        RenderStreamPassSet RenderStreamPasses;
        PostProcessPassChain PostProcessPasses;

        void Setup(Renderer3DData& data,
                   ShaderLibrary& shaderLibrary,
                   const FramebufferSpecification& shadowPassSpec,
                   const FramebufferSpecification& scenePassSpec,
                   const FramebufferSpecification& finalPassSpec);
        void PrepareFrame(Renderer3DData& data, ShaderLibrary& shaderLibrary);
        void ConfigurePassesForFrame(Renderer3DData& data);
        void UploadExecutionState(Renderer3DData& data);
        void PopulateBlackboard(Renderer3DData& data);
        void RefreshBlackboardHandles(Renderer3DData& data);
        [[nodiscard]] auto BuildInputs(Renderer3DData& data) -> RenderPipelineInputs;

        void Reset()
        {
            StreamNodes.Reset();
            FrameCorePasses.Reset();
            SceneCompositePasses.Reset();
            RenderStreamPasses.Reset();
            PostProcessPasses.Reset();
        }

      private:
        void ApplyGlobalResources(Renderer3DData& data);
        void CreateFramePasses(Renderer3DData& data,
                               ShaderLibrary& shaderLibrary,
                               const FramebufferSpecification& shadowPassSpec,
                               const FramebufferSpecification& scenePassSpec,
                               const FramebufferSpecification& finalPassSpec);
        void CreateRenderStreamNodes(Renderer3DData& data);
        void CreatePostProcessPasses(const FramebufferSpecification& finalPassSpec);
    };

    inline Renderer3D::Renderer3DData::Renderer3DData()
        : Pipeline(std::make_unique<RenderPipeline>())
    {
    }

    inline Renderer3D::Renderer3DData::~Renderer3DData() = default;
} // namespace OloEngine
