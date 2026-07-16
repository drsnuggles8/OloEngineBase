#pragma once

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/CloudscapeRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/ContactShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/DeferredGPUOcclusionPass.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"
#include "OloEngine/Renderer/Passes/FluidCompositePass.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/GPUDrivenOcclusionPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"
#include "OloEngine/Renderer/Passes/OverdrawRenderPass.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Passes/PlanarReflectionRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UpscalerRenderPass.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"
#include "OloEngine/Renderer/Passes/DepthVelocityUpscalePass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/Passes/VolumetricFogPass.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

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
        Ref<EASURenderPass> EASU;
        Ref<DepthVelocityUpscalePass> DepthVelocityUpscale;
        Ref<BloomRenderPass> Bloom;
        Ref<DOFRenderPass> DOF;
        Ref<MotionBlurRenderPass> MotionBlur;
        Ref<TAARenderPass> TAA;
        Ref<PrecipitationRenderPass> Precipitation;
        Ref<VolumetricFogPass> VolumetricFog;
        Ref<CloudscapeRenderPass> Cloudscape; // #633 raymarch + temporal resolve + composite (between TAA and Precipitation)
        Ref<FogRenderPass> Fog;
        Ref<ChromaticAberrationRenderPass> ChromAberration;
        Ref<ColorGradingRenderPass> ColorGrading;
        Ref<ToneMapRenderPass> ToneMap;
        Ref<UpscalerRenderPass> Upscaler;
        Ref<VignetteRenderPass> Vignette;
        Ref<FXAARenderPass> FXAA;
        Ref<SelectionOutlineRenderPass> SelectionOutline;
        Ref<OverdrawRenderPass> Overdraw;
        Ref<UICompositeRenderPass> UIComposite;
        Ref<FinalRenderPass> Final;

        void Reset()
        {
            SSS.Reset();
            AOApply.Reset();
            SSGI.Reset();
            SSR.Reset();
            ContactShadow.Reset();
            EASU.Reset();
            DepthVelocityUpscale.Reset();
            Bloom.Reset();
            DOF.Reset();
            MotionBlur.Reset();
            TAA.Reset();
            Precipitation.Reset();
            VolumetricFog.Reset();
            Cloudscape.Reset();
            Fog.Reset();
            ChromAberration.Reset();
            ColorGrading.Reset();
            ToneMap.Reset();
            Upscaler.Reset();
            Vignette.Reset();
            FXAA.Reset();
            SelectionOutline.Reset();
            Overdraw.Reset();
            UIComposite.Reset();
            Final.Reset();
        }
    };

    struct Renderer3D::SceneCompositionPassSet
    {
        Ref<DeferredLightingPass> DeferredLighting;
        Ref<DeferredOpaqueDecalPass> DeferredOpaqueDecal;
        Ref<DeferredGPUOcclusionPass> DeferredGPUOcclusion;
        Ref<PlanarReflectionRenderPass> PlanarReflection;
        Ref<SSAORenderPass> SSAO;
        Ref<GTAORenderPass> GTAO;
        Ref<ParticleRenderPass> Particle;
        Ref<OITPrepareRenderPass> OITPrepare;
        Ref<OITResolveRenderPass> OITResolve;

        void Reset()
        {
            DeferredLighting.Reset();
            DeferredOpaqueDecal.Reset();
            DeferredGPUOcclusion.Reset();
            PlanarReflection.Reset();
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
        // Realtime DDGI probe capture/relight/blend (#632). Path-agnostic:
        // registered between ShadowPass (its relight samples the CSM/atlas)
        // and ScenePass (the forward lit shaders sample the atlases it
        // publishes; DeferredLightingPass does too).
        Ref<DDGIProbeUpdatePass> DDGIProbeUpdate;

        void Reset()
        {
            Shadow.Reset();
            Scene.Reset();
            DDGIProbeUpdate.Reset();
        }
    };

    struct Renderer3D::RenderStreamPassSet
    {
        Ref<ForwardOverlayRenderPass> ForwardOverlay;
        Ref<FoliageRenderPass> Foliage;
        Ref<WaterRenderPass> Water;
        Ref<DecalRenderPass> Decal;
        Ref<GPUDrivenOcclusionPass> GPUOcclusion;
        Ref<FluidIntermediatesPass> FluidIntermediates; // #630 depth splat + smooth + thickness
        Ref<FluidCompositePass> FluidComposite;         // #630 SceneColor RMW shading pass
        Ref<VirtualGeometryPass> VirtualGeometry;       // #629 cluster LOD DAG cull + raster

        void Reset()
        {
            ForwardOverlay.Reset();
            Foliage.Reset();
            Water.Reset();
            Decal.Reset();
            GPUOcclusion.Reset();
            FluidIntermediates.Reset();
            FluidComposite.Reset();
            VirtualGeometry.Reset();
        }
    };

    struct Renderer3D::RenderPipeline
    {
        FrameCorePassSet FrameCorePasses;
        SceneCompositionPassSet SceneCompositePasses;
        RenderStreamPassSet RenderStreamPasses;
        PostProcessPassChain PostProcessPasses;
        Ref<Texture2D> TAAHistoryTexture;
        bool TAAHistoryValid = false;
        // Half-resolution cloudscape resolve history (issue #633) — same
        // sink/import mechanics as the TAA history above.
        Ref<Texture2D> CloudsHistoryTexture;
        bool CloudsHistoryValid = false;
        // Surface weather response UBO (binding 53, issue #633): wetness +
        // cloud-shadow map transform for the PBR surface shaders. Uploaded
        // every frame by UploadExecutionState (zeroed when nothing is
        // enabled — wetness applies with or without clouds).
        Ref<UniformBuffer> AtmosphereShadingUBO;

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
                case RenderStreamType::GPUOcclusion:
                    return RenderStreamPasses.GPUOcclusion.Raw();
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
            func(GetRenderStreamNode(RenderStreamType::GPUOcclusion));
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
            TAAHistoryValid = false;
            CloudsHistoryTexture.Reset();
            CloudsHistoryValid = false;
            AtmosphereShadingUBO.Reset();
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
