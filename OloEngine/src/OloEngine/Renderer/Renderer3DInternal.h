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
#include "OloEngine/Renderer/Passes/ShaderDebugDrawPass.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/VirtualShadowMapMarkPass.h"
#include "OloEngine/Renderer/Passes/RayTracedShadowPass.h"
#include "OloEngine/Renderer/Passes/RayTracingScenePass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/Passes/SphereProxyAORenderPass.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UpscalerRenderPass.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"
#include "OloEngine/Renderer/Passes/FSR2RenderPass.h"
#include "OloEngine/Renderer/Passes/DepthVelocityUpscalePass.h"
#include "OloEngine/Renderer/Passes/ColorBlindRenderPass.h"
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
        Ref<FSR2RenderPass> FSR2; // #684 temporal upscale; the Technique setting picks it OR EASU, never both
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
        Ref<ColorBlindRenderPass> ColorBlind; // #458 accessibility remap; runs after UIComposite so the HUD is adapted too
        Ref<FinalRenderPass> Final;

        void Reset()
        {
            SSS.Reset();
            AOApply.Reset();
            SSGI.Reset();
            SSR.Reset();
            ContactShadow.Reset();
            EASU.Reset();
            FSR2.Reset();
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
            ColorBlind.Reset();
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
        // Analytic sphere-proxy AO (#710). Registered right after the AO
        // producer and before DeferredLightingPass, so its multiply lands in
        // AOBuffer before anything reads it.
        Ref<SphereProxyAORenderPass> SphereProxyAO;
        // Hybrid ray-traced shadows (#1056). Registered after the last
        // G-Buffer writer (it reads scene depth + the world normal) and before
        // DeferredLightingPass (which samples the mask it produces), with a
        // by-name execution edge on RayTracingScenePass.
        Ref<RayTracedShadowPass> RayTracedShadow;
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
            SphereProxyAO.Reset();
            RayTracedShadow.Reset();
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
        // Virtual Shadow Map page marking (#702). Registered LATE — it projects
        // the finished scene depth into the clip levels, and ShadowPass (which
        // consumes what it marks) is the first node in the graph, so its output is
        // one frame ahead of its consumer by construction.
        Ref<VirtualShadowMapMarkPass> VirtualShadowMapMark;
        Ref<RayTracingScenePass> RayTracingScene; // #978 BLAS/TLAS build, first in the frame

        void Reset()
        {
            Shadow.Reset();
            Scene.Reset();
            DDGIProbeUpdate.Reset();
            VirtualShadowMapMark.Reset();
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
        // #725 GPU-pushable debug primitives. Lives in this set because it is
        // registered by RegisterRenderStreamNodes, but it carries no command
        // bucket (its draws come from GPU-appended SSBOs), so it is deliberately
        // absent from GetRenderStreamNode / ForEachRenderStreamNode — both of
        // which deal in CommandBufferRenderPass.
        Ref<ShaderDebugDrawPass> ShaderDebugDraw;

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
            ShaderDebugDraw.Reset();
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
        // Per-pass stochastic-signal histories (issue #902) — SSGI and SSR
        // each accumulate their OWN signal, so each needs its own sink. Same
        // mechanics as the two above, at the scene-band resolution the
        // SSGISignal / SSRSignal scratch targets are declared at.
        Ref<Texture2D> SSRHistoryTexture;
        bool SSRHistoryValid = false;
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
            SSRHistoryTexture.Reset();
            SSRHistoryValid = false;
            AtmosphereShadingUBO.Reset();
            m_HasSSGIEnableState = false;
            m_PreviousSSGIEnabled = false;
            m_PreviousSSGIHalfResolution = true;
            m_HasJitterMode = false;
            m_PreviousJitterMode = 0u;
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
        bool m_HasSSGIEnableState = false;
        bool m_PreviousSSGIEnabled = false;
        // Tracked alongside the enable because the #708 half-resolution toggle
        // resizes every SSGI history; see the invalidation in RenderPipeline.cpp.
        // Defaults to PostProcessSettings::SSGIHalfResolution's own default so
        // the first frame after a reset does not read as a change.
        bool m_PreviousSSGIHalfResolution = true;
        bool m_HasJitterMode = false;
        u8 m_PreviousJitterMode = 0u; // 0=none, 1=TAA, 2=temporal upscale
    };

    inline Renderer3D::Renderer3DData::Renderer3DData()
        : Pipeline(std::make_unique<RenderPipeline>())
    {
    }

    inline Renderer3D::Renderer3DData::~Renderer3DData() = default;
} // namespace OloEngine
