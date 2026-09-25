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
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
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
#include "OloEngine/Renderer/Passes/ScenePrepassRenderPass.h"
#include "OloEngine/Renderer/Passes/GPUDrivenOcclusionPrepassPass.h"
#include "OloEngine/Renderer/Passes/ShaderDebugDrawPass.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/VirtualShadowMapMarkPass.h"
#include "OloEngine/Renderer/Passes/RayTracedShadowPass.h"
#include "OloEngine/Renderer/Passes/ReSTIRDIPass.h"
#include "OloEngine/Renderer/Passes/ReSTIRGIPass.h"
#include "OloEngine/Renderer/Passes/ReSTIRPTPass.h"
#include "OloEngine/Renderer/Passes/RayTracingScenePass.h"
#include "OloEngine/Renderer/Passes/SkeletalDeformPass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/Passes/SphereProxyAORenderPass.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/SkinDiffusionPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UpscalerRenderPass.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"
#include "OloEngine/Renderer/Passes/FSR2RenderPass.h"
#include "OloEngine/Renderer/Passes/DepthVelocityUpscalePass.h"
#include "OloEngine/Renderer/Passes/ColorBlindRenderPass.h"
#include "OloEngine/Renderer/Passes/RayTracedReflectionPass.h"
#include "OloEngine/Renderer/Passes/GpuPathTracerPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/Passes/VolumetricFogPass.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <tuple>

namespace OloEngine
{
    struct RenderPipelineInputs;

    struct Renderer3D::PostProcessPassChain
    {
        // Screen-space skin diffusion (issue #1241). Before SSS in the
        // chain and unrelated to it -- that one is snow's wrap-lighting blur.
        Ref<SkinDiffusionPass> SkinDiffusion;
        Ref<SSSRenderPass> SSS;
        Ref<AOApplyRenderPass> AOApply;
        Ref<SSGIRenderPass> SSGI;
        // The ray-query reflection tier (#1057). Registered BEFORE SSR: ADR 0020
        // composites the hierarchy bottom-up, so this tier lays its answer down
        // and SSR — the tier above it — lerps over the result by its own
        // confidence. Reversing the two would double-count both.
        Ref<RayTracedReflectionPass> RayTracedReflection;
        Ref<SSRRenderPass> SSR;
        Ref<ContactShadowRenderPass> ContactShadow;
        // The GPU reference path tracer (#1055). Registered AFTER the whole
        // screen-space chain and BEFORE the upscalers: its colour replaces the
        // rasterised one at the top of the pre-Bloom alias chain.
        Ref<GpuPathTracerPass> GpuPathTracer;
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

        // Every pass in this set, once. Reset() and ForEachPass() both walk
        // this list, and the static_assert in Reset() fails the build
        // when a member is added without being listed here, so neither the
        // shutdown path nor the declaration key (issue #1333) can miss one.
        // Reset() runs from Renderer3D::Shutdown() while the GL context and the
        // RendererAPI are still alive; a pass that survived it would release
        // its shader / UBO / framebuffer afterwards, when the services that own
        // those are gone. That exit-time crash is why the list must be
        // exhaustive.
        [[nodiscard]] static constexpr auto Members()
        {
            return std::tuple{
                &PostProcessPassChain::SkinDiffusion,
                &PostProcessPassChain::SSS,
                &PostProcessPassChain::AOApply,
                &PostProcessPassChain::SSGI,
                &PostProcessPassChain::RayTracedReflection,
                &PostProcessPassChain::SSR,
                &PostProcessPassChain::ContactShadow,
                &PostProcessPassChain::GpuPathTracer,
                &PostProcessPassChain::EASU,
                &PostProcessPassChain::FSR2,
                &PostProcessPassChain::DepthVelocityUpscale,
                &PostProcessPassChain::Bloom,
                &PostProcessPassChain::DOF,
                &PostProcessPassChain::MotionBlur,
                &PostProcessPassChain::TAA,
                &PostProcessPassChain::Precipitation,
                &PostProcessPassChain::VolumetricFog,
                &PostProcessPassChain::Cloudscape,
                &PostProcessPassChain::Fog,
                &PostProcessPassChain::ChromAberration,
                &PostProcessPassChain::ColorGrading,
                &PostProcessPassChain::ToneMap,
                &PostProcessPassChain::Upscaler,
                &PostProcessPassChain::Vignette,
                &PostProcessPassChain::FXAA,
                &PostProcessPassChain::SelectionOutline,
                &PostProcessPassChain::Overdraw,
                &PostProcessPassChain::UIComposite,
                &PostProcessPassChain::ColorBlind,
                &PostProcessPassChain::Final
            };
        }

        template<typename TFunc>
        void ForEachPass(TFunc&& func) const
        {
            std::apply([this, &func](auto... member)
                       { (func(this->*member), ...); },
                       Members());
        }

        void Reset()
        {
            // In a member body because the set is a private type of Renderer3D.
            static_assert(sizeof(PostProcessPassChain) == std::tuple_size_v<decltype(Members())> * sizeof(Ref<RenderGraphNode>),
                          "PostProcessPassChain has a member that Members() does not list.");
            std::apply([this](auto... member)
                       { ((this->*member).Reset(), ...); },
                       Members());
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
        // ReSTIR DI (#1140). Registered after the last G-Buffer writer and
        // before DeferredLightingPass, which samples the radiance it produces
        // INSTEAD of running its own punctual / area light loop. Same by-name
        // execution edge on RayTracingScenePass, for the same reason.
        Ref<ReSTIRDIPass> ReSTIRDI;
        Ref<ReSTIRGIPass> ReSTIRGI;
        Ref<ReSTIRPTPass> ReSTIRPT;
        Ref<ParticleRenderPass> Particle;
        Ref<OITPrepareRenderPass> OITPrepare;
        Ref<OITResolveRenderPass> OITResolve;

        // Every pass in this set, once. Reset() and ForEachPass() both walk
        // this list, and the static_assert in Reset() fails the build
        // when a member is added without being listed here, so neither the
        // shutdown path nor the declaration key (issue #1333) can miss one.
        [[nodiscard]] static constexpr auto Members()
        {
            return std::tuple{
                &SceneCompositionPassSet::DeferredLighting,
                &SceneCompositionPassSet::DeferredOpaqueDecal,
                &SceneCompositionPassSet::DeferredGPUOcclusion,
                &SceneCompositionPassSet::PlanarReflection,
                &SceneCompositionPassSet::SSAO,
                &SceneCompositionPassSet::GTAO,
                &SceneCompositionPassSet::SphereProxyAO,
                &SceneCompositionPassSet::RayTracedShadow,
                &SceneCompositionPassSet::ReSTIRDI,
                &SceneCompositionPassSet::ReSTIRGI,
                &SceneCompositionPassSet::ReSTIRPT,
                &SceneCompositionPassSet::Particle,
                &SceneCompositionPassSet::OITPrepare,
                &SceneCompositionPassSet::OITResolve
            };
        }

        template<typename TFunc>
        void ForEachPass(TFunc&& func) const
        {
            std::apply([this, &func](auto... member)
                       { (func(this->*member), ...); },
                       Members());
        }

        void Reset()
        {
            // In a member body because the set is a private type of Renderer3D.
            static_assert(sizeof(SceneCompositionPassSet) == std::tuple_size_v<decltype(Members())> * sizeof(Ref<RenderGraphNode>),
                          "SceneCompositionPassSet has a member that Members() does not list.");
            std::apply([this](auto... member)
                       { ((this->*member).Reset(), ...); },
                       Members());
        }
    };

    struct Renderer3D::FrameCorePassSet
    {
        Ref<ShadowRenderPass> Shadow;
        Ref<SceneRenderPass> Scene;
        // The forward paths' depth(-normal) prepass as its own node, ahead of
        // the screen-space AO passes (issue #1452). Renders Scene's bucket.
        Ref<ScenePrepassRenderPass> ScenePrepass;
        // ...and the GPU-driven instanced batches' share of it (issue #1452),
        // rendered from GPUOcclusion's bucket ahead of the AO passes.
        Ref<GPUDrivenOcclusionPrepassPass> GPUOcclusionPrepass;
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
        // #1229 skinning-to-memory, immediately BEFORE RayTracingScene:
        // it writes the vertex buffers those BLASes are built from.
        Ref<SkeletalDeformPass> SkeletalDeform;
        Ref<RayTracingScenePass> RayTracingScene; // #978 BLAS/TLAS build, first in the frame

        // Every pass in this set, once. Reset() and ForEachPass() both walk
        // this list, and the static_assert in Reset() fails the build
        // when a member is added without being listed here, so neither the
        // shutdown path nor the declaration key (issue #1333) can miss one.
        [[nodiscard]] static constexpr auto Members()
        {
            return std::tuple{
                &FrameCorePassSet::Shadow,
                &FrameCorePassSet::Scene,
                &FrameCorePassSet::ScenePrepass,
                &FrameCorePassSet::GPUOcclusionPrepass,
                &FrameCorePassSet::DDGIProbeUpdate,
                &FrameCorePassSet::VirtualShadowMapMark,
                &FrameCorePassSet::SkeletalDeform,
                &FrameCorePassSet::RayTracingScene
            };
        }

        template<typename TFunc>
        void ForEachPass(TFunc&& func) const
        {
            std::apply([this, &func](auto... member)
                       { (func(this->*member), ...); },
                       Members());
        }

        void Reset()
        {
            // In a member body because the set is a private type of Renderer3D.
            static_assert(sizeof(FrameCorePassSet) == std::tuple_size_v<decltype(Members())> * sizeof(Ref<RenderGraphNode>),
                          "FrameCorePassSet has a member that Members() does not list.");
            std::apply([this](auto... member)
                       { ((this->*member).Reset(), ...); },
                       Members());
        }
    };

    struct Renderer3D::RenderStreamPassSet
    {
        Ref<ForwardOverlayRenderPass> ForwardOverlay;
        Ref<FoliageRenderPass> Foliage;
        Ref<GroomRenderPass> Groom; // #1246 strand visibility
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

        // Every pass in this set, once. Reset() and ForEachPass() both walk
        // this list, and the static_assert in Reset() fails the build
        // when a member is added without being listed here, so neither the
        // shutdown path nor the declaration key (issue #1333) can miss one.
        [[nodiscard]] static constexpr auto Members()
        {
            return std::tuple{
                &RenderStreamPassSet::ForwardOverlay,
                &RenderStreamPassSet::Foliage,
                &RenderStreamPassSet::Groom,
                &RenderStreamPassSet::Water,
                &RenderStreamPassSet::Decal,
                &RenderStreamPassSet::GPUOcclusion,
                &RenderStreamPassSet::FluidIntermediates,
                &RenderStreamPassSet::FluidComposite,
                &RenderStreamPassSet::VirtualGeometry,
                &RenderStreamPassSet::ShaderDebugDraw
            };
        }

        template<typename TFunc>
        void ForEachPass(TFunc&& func) const
        {
            std::apply([this, &func](auto... member)
                       { (func(this->*member), ...); },
                       Members());
        }

        void Reset()
        {
            // In a member body because the set is a private type of Renderer3D.
            static_assert(sizeof(RenderStreamPassSet) == std::tuple_size_v<decltype(Members())> * sizeof(Ref<RenderGraphNode>),
                          "RenderStreamPassSet has a member that Members() does not list.");
            std::apply([this](auto... member)
                       { ((this->*member).Reset(), ...); },
                       Members());
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
        void PopulateBlackboard(Renderer3DData& data, const FrameGraphDeclarationConfig& config);

        // Every pass the pipeline owns, in a fixed order, null members included
        // (the callback receives a `const Ref<T>&`). The declaration key walks
        // this, so a pass cannot be left out of it by being left off a list.
        template<typename TFunc>
        void ForEachPass(TFunc&& func) const
        {
            FrameCorePasses.ForEachPass(func);
            SceneCompositePasses.ForEachPass(func);
            RenderStreamPasses.ForEachPass(func);
            PostProcessPasses.ForEachPass(func);
        }

        // ------------------------------------------------------------------
        // Declaration configuration (issue #1333)
        // ------------------------------------------------------------------
        // One frame's graph compilation, in the order the cache depends on:
        //   1. PrepareDeclarationInputs: every resize and history-storage change
        //      that alters an input, so nothing moves after the capture;
        //   2. CaptureDeclarationConfig: the immutable configuration and its key;
        //   3. PopulateBlackboard(config), UploadExecutionState, and
        //      BuildFrameGraph(key), all keyed on that ONE key.
        // Under OLO_RG_VERIFY_DECLARATION_CACHE a frame that would have been
        // served from the cache is rebuilt and the two plans compared.
        void CompileFrameGraph(Renderer3DData& data);

        void PrepareDeclarationInputs(Renderer3DData& data);
        // `passKeys`, when given, receives each pass's own key in ForEachPass
        // order, for attributing a PassStates change to named passes.
        [[nodiscard]] FrameGraphDeclarationConfig CaptureDeclarationConfig(const Renderer3DData& data,
                                                                           TArray<u64>* passKeys = nullptr) const;
        [[nodiscard]] u64 ComputeDeclarationKey(const Renderer3DData& data) const
        {
            return CaptureDeclarationConfig(data).ComputeKey();
        }

        [[nodiscard]] const FrameGraphDeclarationStats& GetDeclarationStats() const
        {
            return m_DeclarationStats;
        }
        void ResetDeclarationStats()
        {
            m_DeclarationStats = {};
        }
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
            // The passes are gone, so the configuration and plan compiled from
            // them are too; the next compile is a first compile, not a diff
            // against a pipeline that no longer exists.
            m_CompiledConfig = FrameGraphDeclarationConfig{};
            m_HasCompiledConfig = false;
            m_CompiledPassKeys.Reset();
            m_CompiledPlanDigest = 0;
            m_CompiledPlanEntries.clear();
            m_HasSSGIEnableState = false;
            m_PreviousSSGIEnabled = false;
            m_PreviousSSGIHalfResolution = true;
            m_HasJitterMode = false;
            m_PreviousJitterMode = 0u;
            m_ReportedSceneTemporalResolve = false;
            m_ReportedRayTracedShadowGateVerdict = kNoRayTracedShadowVerdict;
            m_ReportedRayTracedShadowMaskVerdict = kNoRayTracedShadowVerdict;
            m_ReportedRayTracedReflectionVerdict = kNoRayTracedShadowVerdict;
            m_ReportedReSTIRDIVerdict = kNoReSTIRDIVerdict;
            m_ReportedReSTIRGIVerdict = kNoReSTIRGIVerdict;
            InvalidateBlackboardCache();
        }

        // PopulateBlackboard skips its body while the declaration key matches
        // the one it last populated from. Call this to force a full repopulate
        // for a reason the configuration cannot see; prefer adding the reason
        // to FrameGraphDeclarationConfig instead.
        void InvalidateBlackboardCache()
        {
            m_HasValidBlackboardCache = false;
        }

        // The two ray-traced-shadow diagnostics (issue #1056) warn once per
        // CHANGE of verdict rather than once per frame. Members, not
        // function-local statics: a static would be shared by every pipeline for
        // the life of the process, and it must also be CLEARED when the gate it
        // describes becomes healthy, or a user who fixes the cause and later
        // reintroduces it gets silence the second time. Both are reset on the
        // frame their own gate passes.
        static constexpr u32 kNoRayTracedShadowVerdict = ~0u;
        u32 m_ReportedRayTracedShadowGateVerdict = kNoRayTracedShadowVerdict;
        u32 m_ReportedRayTracedShadowMaskVerdict = kNoRayTracedShadowVerdict;
        // Same latch, same reason, for the ray-query REFLECTION tier (#1057):
        // "I enabled the tier and the frame did not change" has no answer
        // anywhere else, because a tier whose target is never declared is
        // culled before it can count anything about itself.
        u32 m_ReportedRayTracedReflectionVerdict = kNoRayTracedShadowVerdict;
        // Same latch, same reason, for the ReSTIR DI tier (#1140).
        static constexpr u32 kNoReSTIRDIVerdict = ~0u;
        u32 m_ReportedReSTIRDIVerdict = kNoReSTIRDIVerdict;
        static constexpr u32 kNoReSTIRGIVerdict = ~0u;
        u32 m_ReportedReSTIRGIVerdict = kNoReSTIRGIVerdict;
        u64 ReSTIRPTSceneEpoch = 1;

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
        // The configuration the cached blackboard and build were compiled
        // from, and the per-pass keys that make up its PassStates, in
        // ForEachPass order, so a rebuild can be attributed to named fields and
        // named passes rather than to "the key moved".
        FrameGraphDeclarationConfig m_CompiledConfig;
        bool m_HasCompiledConfig = false;
        TArray<u64> m_CompiledPassKeys;
        u64 m_CompiledPlanDigest = 0;
        std::vector<RenderGraph::PlanDigestEntry> m_CompiledPlanEntries;
        FrameGraphDeclarationStats m_DeclarationStats;
        bool m_HasSSGIEnableState = false;
        bool m_PreviousSSGIEnabled = false;
        // Tracked alongside the enable because the #708 half-resolution toggle
        // resizes every SSGI history; see the invalidation in RenderPipeline.cpp.
        // Defaults to PostProcessSettings::SSGIHalfResolution's own default so
        // the first frame after a reset does not read as a change.
        bool m_PreviousSSGIHalfResolution = true;
        bool m_HasJitterMode = false;
        u8 m_PreviousJitterMode = 0u; // 0=none, 1=TAA, 2=temporal upscale
        // Whether the "TAA runs on the scene's request" line has been logged for
        // the current state (#1429). A member rather than a function static so a
        // renderer re-init reports it again.
        bool m_ReportedSceneTemporalResolve = false;
    };

    namespace Renderer3DDetail
    {
        // A see-through debug mesh (Renderer3D::DrawLine / DrawSphere): what
        // DrawMesh must do differently while one is being submitted.
        //
        //  * On the Deferred path it goes to ForwardOverlayPass with the forward
        //    PBR shader, not into the G-Buffer: with depth test off it writes no
        //    depth, so over the sky DeferredLighting shaded it as background,
        //    and its attachment-0-only mask (written for the scene framebuffer's
        //    layout, where 1 is entity ID and 2 the view normal) kept it out of
        //    the G-Buffer's emissive lane everywhere else.
        //  * On EVERY path its render state is patched INSIDE DrawMesh, before
        //    the packet can be submitted: depth test off, colour attachment 0
        //    only, UI view layer, and two-sided when asked. The helpers used to
        //    patch the returned packet, but the overlay route submits the packet
        //    itself and returns nullptr, so on Deferred the patch never landed
        //    and the lines were drawn depth-tested and back-face culled — one
        //    face of each cross on OpenGL and none on Vulkan (issue #1472).
        //
        // Thread-local: the scope brackets one DrawMesh call on one thread.
        struct DebugDrawRequest
        {
            bool Active = false;
            bool TwoSided = false;
        };
        inline thread_local DebugDrawRequest t_DebugDraw{};

        class DebugDrawScope
        {
          public:
            explicit DebugDrawScope(bool twoSided)
                : m_Previous(t_DebugDraw)
            {
                t_DebugDraw = DebugDrawRequest{ .Active = true, .TwoSided = twoSided };
            }
            ~DebugDrawScope()
            {
                t_DebugDraw = m_Previous;
            }
            DebugDrawScope(const DebugDrawScope&) = delete;
            DebugDrawScope& operator=(const DebugDrawScope&) = delete;
            DebugDrawScope(DebugDrawScope&&) = delete;
            DebugDrawScope& operator=(DebugDrawScope&&) = delete;

          private:
            DebugDrawRequest m_Previous;
        };
    } // namespace Renderer3DDetail

    inline Renderer3D::Renderer3DData::Renderer3DData()
        : Pipeline(std::make_unique<RenderPipeline>())
    {
    }

    inline Renderer3D::Renderer3DData::~Renderer3DData() = default;
} // namespace OloEngine
