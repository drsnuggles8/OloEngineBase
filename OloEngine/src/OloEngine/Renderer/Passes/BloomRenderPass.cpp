#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    BloomRenderPass::BloomRenderPass()
    {
        SetName("BloomPass");
    }

    void BloomRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedBloomMipFramebuffers.fill({});

        // Resolve upstream producer by base name: take the latest versioned
        // output of any candidate upstream that ran this frame, else fall
        // through to the canonical blackboard imports. The graph derives
        // the ordering edge automatically from the Read on the versioned
        // texture, so no typed pass-pointer setter is needed.
        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                // FSR1 EASU (when upscaling) is the freshest pre-Bloom source and
                // the only full-display-resolution one — it must sit above the
                // reduced-resolution SS-band colours it already consumed.
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::EASUColor, ResourceNames::EASUColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ContactShadowColor, ResourceNames::ContactShadowColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSRColor, ResourceNames::SSRColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (!m_Enabled || !blackboard.Post.BloomColor.IsValid())
            return;

        constexpr std::string_view bloomVersionTag = "BloomPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.BloomColor, RGWriteUsage::RenderTarget, bloomVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::BloomColorTexture) + "@" +
                                                        std::string(bloomVersionTag),
                                                    outputHandle,
                                                    0u));

        // Downsample/upsample chain reads previous mip via texture() (shader
        // sample), not as an input attachment — barrier planner needs the
        // sample fence, not a sub-pass attachment-read.
        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (!blackboard.Scratch.BloomMips[i].IsValid())
                break;

            m_SelectedBloomMipFramebuffers[i] = blackboard.Scratch.BloomMips[i];
            // Intra-pass ping-pong: the downsample/upsample loop binds each
            // mip as a render target and then samples it from the next-mip
            // shader inside a single Execute. No prior pass produces these
            // mips, so renaming via WriteNewVersion would not buy ordering.
            builder.AllowSamePassReadWrite(blackboard.Scratch.BloomMips[i]);
            builder.Write(blackboard.Scratch.BloomMips[i], RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto mipRead = builder.Read(blackboard.Scratch.BloomMips[i], RGReadUsage::ShaderSample);
        }
    }

    void BloomRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_BloomThresholdShader = Shader::Create("assets/shaders/PostProcess_BloomThreshold.glsl");
        m_BloomDownsampleShader = Shader::Create("assets/shaders/PostProcess_BloomDownsample.glsl");
        m_BloomUpsampleShader = Shader::Create("assets/shaders/PostProcess_BloomUpsample.glsl");
        m_BloomCompositeShader = Shader::Create("assets/shaders/PostProcess_BloomComposite.glsl");

        OLO_CORE_INFO("BloomRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void BloomRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void BloomRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto fb = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = fb;
        }

        if (!m_Enabled)
        {
            m_Target = nullptr;
            m_LastFailureMask = 0;
            return;
        }

        // Phase D / H follow-up: resolve the bloom mip-chain entirely from the
        // transient pool. The execute path no longer seeds from an owned
        // fallback chain.
        std::array<Ref<Framebuffer>, MAX_BLOOM_MIPS> bloomMips{};
        u32 bloomMipCount = 0;
        u32 bloomMipHandleCount = 0;
        u32 bloomMipResolvedCount = 0;
        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (m_SelectedBloomMipFramebuffers[i].IsValid())
            {
                ++bloomMipHandleCount;
                if (auto fb = context.ResolveFramebuffer(m_SelectedBloomMipFramebuffers[i]))
                {
                    bloomMips[i] = fb;
                    ++bloomMipResolvedCount;
                }
            }
        }
        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (bloomMips[i])
                bloomMipCount = i + 1;
            else
                break;
        }
        const bool shadersReady = IsReadyForExecution();

        constexpr u32 FAIL_NO_INPUT = 1u << 0u;
        constexpr u32 FAIL_NO_OUTPUT = 1u << 1u;
        constexpr u32 FAIL_NO_MIPS = 1u << 2u;
        constexpr u32 FAIL_NO_SHADERS = 1u << 3u;

        u32 failureMask = 0;
        if (inputColorTextureID == 0u)
            failureMask |= FAIL_NO_INPUT;
        if (!outputFramebuffer)
            failureMask |= FAIL_NO_OUTPUT;
        if (bloomMipCount == 0)
            failureMask |= FAIL_NO_MIPS;
        if (!shadersReady)
            failureMask |= FAIL_NO_SHADERS;

        if (failureMask != 0)
        {
            if (m_LastFailureMask != failureMask)
            {
                OLO_CORE_ERROR("BloomRenderPass: prerequisites missing (mask=0x{:x}, inputTex={}, outputFB={}, mipCount={}, mipHandlesValid={}, mipResolved={}, shadersReady={})",
                               failureMask,
                               inputColorTextureID,
                               outputFramebuffer ? outputFramebuffer->GetRendererID() : 0u,
                               bloomMipCount,
                               bloomMipHandleCount,
                               bloomMipResolvedCount,
                               shadersReady);
            }

            m_Target = nullptr;
            m_LastFailureMask = failureMask;
            OLO_CORE_ASSERT(false, "BloomRenderPass enabled without complete graph/shader state");
            return;
        }

        if (m_LastFailureMask != 0)
        {
            OLO_CORE_INFO("BloomRenderPass: recovered (outputFB={}, mipCount={})",
                          outputFramebuffer->GetRendererID(), bloomMipCount);
            m_LastFailureMask = 0;
        }

        m_Target = outputFramebuffer;

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        constexpr u32 colorAttachment = 0;

        // ------------------------------------------------------------------
        // Step 1: Threshold extract — scene HDR → bloom mip 0
        // ------------------------------------------------------------------
        {
            bloomMips[0]->Bind();
            const auto& spec = bloomMips[0]->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomThresholdShader->Bind();
            context.BindTexture(0, inputColorTextureID);
            m_BloomThresholdShader->SetInt("u_Texture", 0);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            bloomMips[0]->Unbind();
        }

        // ------------------------------------------------------------------
        // Step 2: Progressive downsample
        // ------------------------------------------------------------------
        for (size_t i = 1; i < static_cast<size_t>(bloomMipCount); ++i)
        {
            auto& srcMip = bloomMips[i - 1];
            auto& dstMip = bloomMips[i];
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            context.SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomDownsampleShader->Bind();
            const u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcID);

            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            dstMip->Unbind();
        }

        // ------------------------------------------------------------------
        // Step 3: Progressive upsample (additive accumulation back up the chain)
        // ------------------------------------------------------------------
        for (int i = static_cast<int>(bloomMipCount) - 2; i >= 0; --i)
        {
            auto& srcMip = bloomMips[static_cast<size_t>(i) + 1];
            auto& dstMip = bloomMips[static_cast<size_t>(i)];
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            context.SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            // Enable additive blending AFTER Bind() — Framebuffer::Bind() may
            // unconditionally re-enable GL_BLEND; set our state after the call.
            context.SetBlendState(true);
            RenderCommand::SetBlendFunc(GL_ONE, GL_ONE);
            // No clear — we're additively accumulating into existing content.

            m_BloomUpsampleShader->Bind();
            const u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcID);

            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            dstMip->Unbind();
        }

        // Restore blend to opaque default.
        context.SetBlendState(false);
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        context.SetDepthMask(true);

        // Restore TexelSize to full resolution so the composite shader (and
        // any subsequent UBO readers) sees the correct value.
        if (m_GPUData && m_PostProcessUBO)
        {
            m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(m_FramebufferSpec.Width);
            m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(m_FramebufferSpec.Height);
            m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
        }

        // ------------------------------------------------------------------
        // Step 4: Composite bloom mip 0 onto scene color → output FB
        // ------------------------------------------------------------------
        {
            outputFramebuffer->Bind();
            const auto& outSpec = outputFramebuffer->GetSpecification();
            context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomCompositeShader->Bind();

            context.BindTexture(0, inputColorTextureID);
            m_BloomCompositeShader->SetInt("u_SceneColor", 0);

            const u32 bloomColorID = bloomMips[0]->GetColorAttachmentRendererID(0);
            context.BindTexture(1, bloomColorID);
            m_BloomCompositeShader->SetInt("u_BloomColor", 1);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            outputFramebuffer->Unbind();
        }

        // Restore full viewport for downstream passes.
        const auto& outputSpec = outputFramebuffer->GetSpecification();
        context.SetViewport(0, 0, outputSpec.Width, outputSpec.Height);
        context.SetDepthMask(true);
    }

    void BloomRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::SetupFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void BloomRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);

        OLO_CORE_INFO("BloomRenderPass: Resized to {}x{}", width, height);
    }

    void BloomRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;
        m_LastFailureMask = 0;
        m_SelectedBloomMipFramebuffers.fill({});
    }
} // namespace OloEngine
