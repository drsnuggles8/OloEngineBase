#pragma once

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/ContactShadowRenderPass.h"
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
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine
{
    struct RenderPipelineInputs;

    struct Renderer3D::PostProcessPassChain
    {
        Ref<SSSRenderPass> SSS;
        Ref<AOApplyRenderPass> AOApply;
        Ref<SSGIRenderPass> SSGI;
        Ref<SSRRenderPass> SSR;
        Ref<ContactShadowRenderPass> ContactShadow;
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
            SSGI.Reset();
            SSR.Reset();
            ContactShadow.Reset();
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
        FrameCorePassSet FrameCorePasses;
        SceneCompositionPassSet SceneCompositePasses;
        RenderStreamPassSet RenderStreamPasses;
        PostProcessPassChain PostProcessPasses;
        Ref<Texture2D> TAAHistoryTexture;
        Ref<Texture2D> FogHistoryTexture;
        bool TAAHistoryValid = false;
        bool FogHistoryValid = false;

        [[nodiscard]] auto GetRenderStreamNode(const RenderStreamType stream) -> CommandBufferRenderPass*
        {
            switch (stream)
            {
                case RenderStreamType::Geometry:
                    return FrameCorePasses.Scene.Raw();
                case RenderStreamType::ForwardOverlay:
                    return RenderStreamPasses.ForwardOverlay.Raw();
                case RenderStreamType::Foliage:
                    return RenderStreamPasses.Foliage.Raw();
                case RenderStreamType::Water:
                    return RenderStreamPasses.Water.Raw();
                case RenderStreamType::Decal:
                    return RenderStreamPasses.Decal.Raw();
            }

            return nullptr;
        }

        template<typename TFunc>
        void ForEachRenderStreamNode(TFunc&& func)
        {
            func(GetRenderStreamNode(RenderStreamType::Geometry));
            func(GetRenderStreamNode(RenderStreamType::ForwardOverlay));
            func(GetRenderStreamNode(RenderStreamType::Foliage));
            func(GetRenderStreamNode(RenderStreamType::Water));
            func(GetRenderStreamNode(RenderStreamType::Decal));
        }

        void Setup(Renderer3DData& data,
                   ShaderLibrary& shaderLibrary,
                   const FramebufferSpecification& shadowPassSpec,
                   const FramebufferSpecification& scenePassSpec,
                   const FramebufferSpecification& finalPassSpec);
        void PrepareFrame(Renderer3DData& data, ShaderLibrary& shaderLibrary);
        void ConfigurePassesForFrame(Renderer3DData& data);
        void UploadExecutionState(Renderer3DData& data);
        void PopulateBlackboard(Renderer3DData& data);
        [[nodiscard]] auto BuildInputs(Renderer3DData& data) -> RenderPipelineInputs;

        void Reset()
        {
            FrameCorePasses.Reset();
            SceneCompositePasses.Reset();
            RenderStreamPasses.Reset();
            PostProcessPasses.Reset();
            TAAHistoryTexture.Reset();
            FogHistoryTexture.Reset();
            TAAHistoryValid = false;
            FogHistoryValid = false;
            InvalidateBlackboardCache();
        }

        // PopulateBlackboard caches its previous-frame result via a fingerprint
        // hash of the inputs that drive its branches. When the hash matches the
        // previous frame, the function short-circuits and the existing handles
        // in FrameBlackboard remain valid. Call this to force a full repopulate
        // (e.g., on resize, settings change, or pass set rebuild).
        void InvalidateBlackboardCache()
        {
            m_HasValidBlackboardCache = false;
        }

        // Computes a fingerprint of all per-frame inputs that affect both
        // PopulateBlackboard's output and the per-pass Setup callbacks that run
        // inside RenderGraph::BuildFrameGraph. The same fingerprint is used as
        // the cache key for both layers so they short-circuit consistently
        // whenever the inputs match the previous frame.
        [[nodiscard]] u64 ComputeBlackboardFingerprint(const Renderer3DData& data) const;

      private:
        void ApplyGlobalResources(Renderer3DData& data) const;
        void CreateFramePasses(Renderer3DData& data,
                               ShaderLibrary& shaderLibrary,
                               const FramebufferSpecification& shadowPassSpec,
                               const FramebufferSpecification& scenePassSpec,
                               const FramebufferSpecification& finalPassSpec);
        void CreatePostProcessPasses(const FramebufferSpecification& finalPassSpec);

        u64 m_BlackboardFingerprint = 0;
        bool m_HasValidBlackboardCache = false;
    };

    inline Renderer3D::Renderer3DData::Renderer3DData()
        : Pipeline(std::make_unique<RenderPipeline>())
    {
    }

    inline Renderer3D::Renderer3DData::~Renderer3DData() = default;
} // namespace OloEngine
