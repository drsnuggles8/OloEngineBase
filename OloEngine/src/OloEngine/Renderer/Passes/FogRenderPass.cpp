#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <span>

namespace OloEngine
{
    FogRenderPass::FogRenderPass()
    {
        SetName("FogPass");
    }

    void FogRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedFogHalfResFramebuffer = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedShadowCSMTexture = {};

        // The froxel volumetric fog chain must have integrated its volume
        // before the composite samples it (its output is engine-owned, not a
        // graph resource, so the edge is declared explicitly).
        builder.DependsOnPass("VolumetricFogPass");

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::CloudsColor, ResourceNames::CloudsColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Post.UpscaledSceneDepthTexture.IsValid() ? blackboard.Post.UpscaledSceneDepthTexture : blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(m_SelectedSceneDepthTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            m_SelectedShadowCSMTexture = blackboard.Shadows.ShadowMapCSM;
            [[maybe_unused]] const auto shadowMapRead = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.FogHalfRes.IsValid())
        {
            m_SelectedFogHalfResFramebuffer = blackboard.Scratch.FogHalfRes;
            // Intra-pass write-then-sample: Pass A renders the half-resolution
            // fog evaluation into FogHalfRes; Pass B (bilateral upsample)
            // samples that result inside the same Execute. Graph-owned scratch
            // with no prior writer to chain against. (The old 2D FogHistory
            // extraction died with the screen-space raymarch — temporal
            // accumulation lives in VolumetricFogPass's 3D scatter volume now,
            // issue #435.)
            builder.AllowSamePassReadWrite(blackboard.Scratch.FogHalfRes);
            builder.Write(blackboard.Scratch.FogHalfRes, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto fogHalfRead = builder.Read(blackboard.Scratch.FogHalfRes, RGReadUsage::ShaderSample);
        }

        if (blackboard.Post.FogColor.IsValid())
        {
            constexpr std::string_view fogVersionTag = "FogPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.FogColor, RGWriteUsage::RenderTarget, fogVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::FogColorTexture) + "@" +
                                                            std::string(fogVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void FogRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_FogShader = Shader::Create("assets/shaders/PostProcess_Fog.glsl");
        m_FogUpsampleShader = Shader::Create("assets/shaders/PostProcess_FogUpsample.glsl");

        OLO_CORE_INFO("FogRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void FogRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FogRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            m_FogHalfWidth = 0;
            m_FogHalfHeight = 0;
            return;
        }

        // Half-resolution framebuffers for ray-march and temporal history.
        m_FogHalfWidth = (width + 1) / 2;
        m_FogHalfHeight = (height + 1) / 2;

        m_Target = nullptr;
    }

    void FogRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        Ref<Framebuffer> fogHalfResFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedFogHalfResFramebuffer.IsValid())
            fogHalfResFramebuffer = context.ResolveFramebuffer(m_SelectedFogHalfResFramebuffer);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !m_FogShader || !m_FogUpsampleShader || !fogHalfResFramebuffer)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        const RHI::ResourceHandle sceneDepthTextureID = m_SelectedSceneDepthTexture.IsValid()
                                                            ? context.ResolveTextureHandle(m_SelectedSceneDepthTexture)
                                                            : RHI::NullResource;

        if (!sceneDepthTextureID.IsValid())
            return; // Fog pass requires depth.

        // Placeholder sampler2DArrayShadow when no real CSM bound — shader's
        // u_DirectionalShadowEnabled still gates the actual sample.
        const RHI::ResourceHandle shadowCSMTextureID = m_SelectedShadowCSMTexture.IsValid()
                                                           ? context.ResolveTextureHandle(m_SelectedShadowCSMTexture)
                                                           : ShadowMap::GetCSMPlaceholderHandle();

        // Re-bind PostProcessUBO at binding 7 — IBL precompute and bloom-mip
        // updates can transiently claim this slot before the post-process chain.
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        // Re-bind the full shared camera UBO at binding 0. Both fog shaders read
        // the full CameraMatrices layout — u_CameraPosition (std140 offset 192,
        // PostProcess_Fog.glsl) and u_Projection (offset 128, PostProcess_Fog-
        // Upsample.glsl) — but an earlier 64-byte ViewProjection-only camera UBO
        // (Renderer2D / ParticleBatchRenderer style) can be left bound at slot 0,
        // which makes those reads out-of-bounds (origin-centred scenes survive
        // only because robust-access OOB reads return 0 ≈ the true camera). Pin
        // the full 288-byte UBO here so off-origin worlds fog correctly.
        if (m_CameraUBO)
            m_CameraUBO->Bind();

        // ----------------------------------------------------------------
        // Pass A — Half-resolution ray-march.
        // Output: RGBA16F (RGB = accumulated inscatter, A = transmittance).
        // ----------------------------------------------------------------
        fogHalfResFramebuffer->Bind();
        const auto& fogHalfSpec = fogHalfResFramebuffer->GetSpecification();
        context.SetViewport(0, 0, fogHalfSpec.Width, fogHalfSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_FogShader->Bind();

        // Full-resolution depth (the shader samples at half-res UV).
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);

        // CSM shadow map for volumetric light shafts (slot TEX_SHADOW = 8).
        //
        // STAYS A REAL BIND, PERMANENTLY, and this is the seam's rule protecting
        // the pass rather than tripping it. PostProcess_Fog.glsl is a bindless
        // VARIANT (its depth and froxel inputs are converted), but the shadow
        // array is declared in the shared DeferredLightingShared.glsl and cannot
        // be `#define`d away without rewriting that header's declaration in every
        // shader that includes it. A converted declaration is what makes a seam
        // call correct; without one, BindTextureOrHeapOffset here would record an
        // offset, skip the bind, and leave the sampler dark
        // (glsl-shaders.md §5c). Convert this only when TEX_SHADOW moves in the
        // shared header, and then in one step across every consumer.
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW, shadowCSMTextureID);

        // Integrated froxel fog volume (issue #435): the shader's volumetric
        // branch fetches it with one trilinear tap per pixel. When the froxel
        // chain did not run this frame, re-upload its disabled UBO so the
        // shader falls back to the analytic path instead of sampling a stale
        // volume.
        const bool froxelRan = m_VolumetricFogPass && m_VolumetricFogPass->RanThisFrame();
        if (froxelRan)
        {
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_FROXEL_FOG,
                                            m_VolumetricFogPass->GetIntegratedVolumeID(),
                                            RHI::HeapSlotLifetime::FrameTransient);
        }
        else if (m_VolumetricFogPass)
        {
            m_VolumetricFogPass->UploadDisabledUBO();
        }

        {
            // Publish the heap offsets recorded above (no-op with the heap off).
            context.FlushHeapOffsets();

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        fogHalfResFramebuffer->Unbind();

        // ----------------------------------------------------------------
        // Pass B — Bilateral upsample + composite onto full-resolution scene.
        // ----------------------------------------------------------------
        outputFramebuffer->Bind();
        const auto& outSpec = outputFramebuffer->GetSpecification();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_FogUpsampleShader->Bind();

        // All three SetInt sampler companions are gone with the binds: already
        // redundant against the shader's own `layout(binding = N)`, and a
        // "uniform not found" warning every frame under the bindless variant
        // where each name is a #define (issue #691 Phase 3).

        // Scene colour at slot 0.
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);

        const RHI::ResourceHandle fogTexture = fogHalfResFramebuffer->GetColorAttachmentHandle(0);
        context.BindTextureOrHeapOffset(1, fogTexture, RHI::HeapSlotLifetime::FrameTransient);

        // Full-res depth for bilateral edge detection.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.FlushHeapOffsets();
            context.DrawIndexed(va);
        }

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void FogRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void FogRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void FogRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedFogHalfResFramebuffer = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedShadowCSMTexture = {};
    }
} // namespace OloEngine
