#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    BloomRenderPass::BloomRenderPass()
    {
        SetName("BloomPass");
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

        DeclareRead(ResourceNames::PostProcessColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::BloomColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("BloomRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void BloomRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_OutputFB = nullptr;
            m_BloomMipChain.clear();
            return;
        }

        FramebufferSpecification outSpec;
        outSpec.Width = width;
        outSpec.Height = height;
        outSpec.Samples = 1;
        outSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
        m_OutputFB = Framebuffer::Create(outSpec);

        CreateMipChain(width, height);
    }

    void BloomRenderPass::CreateMipChain(u32 width, u32 height)
    {
        m_BloomMipChain.clear();

        u32 mipWidth = width / 2;
        u32 mipHeight = height / 2;

        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (mipWidth < 2 || mipHeight < 2)
            {
                break;
            }

            FramebufferSpecification mipSpec;
            mipSpec.Width = mipWidth;
            mipSpec.Height = mipHeight;
            mipSpec.Samples = 1;
            mipSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

            m_BloomMipChain.push_back(Framebuffer::Create(mipSpec));

            mipWidth /= 2;
            mipHeight /= 2;
        }

        OLO_CORE_INFO("BloomRenderPass: Created bloom mip chain with {} levels", m_BloomMipChain.size());
    }

    void BloomRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void BloomRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 43 — self-resolving input framebuffer from the render-graph
        // blackboard. Bloom reads PostProcess (the sole input choice).
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
            if (auto fb = context.ResolveFramebuffer(board->PostProcessColor))
                inputFramebuffer = fb;
        if (!m_Enabled || !inputFramebuffer || !m_OutputFB || m_BloomMipChain.empty())
        {
            return;
        }

        if (!m_BloomThresholdShader || !m_BloomDownsampleShader ||
            !m_BloomUpsampleShader || !m_BloomCompositeShader)
        {
            return;
        }

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        constexpr u32 colorAttachment = 0;

        // ------------------------------------------------------------------
        // Step 1: Threshold extract — scene HDR → bloom mip 0
        // ------------------------------------------------------------------
        {
            m_BloomMipChain[0]->Bind();
            const auto& spec = m_BloomMipChain[0]->GetSpecification();
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
            const u32 srcID = inputFramebuffer->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcID);
            m_BloomThresholdShader->SetInt("u_Texture", 0);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            m_BloomMipChain[0]->Unbind();
        }

        // ------------------------------------------------------------------
        // Step 2: Progressive downsample
        // ------------------------------------------------------------------
        for (size_t i = 1; i < m_BloomMipChain.size(); ++i)
        {
            auto& srcMip = m_BloomMipChain[i - 1];
            auto& dstMip = m_BloomMipChain[i];
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
        for (int i = static_cast<int>(m_BloomMipChain.size()) - 2; i >= 0; --i)
        {
            auto& srcMip = m_BloomMipChain[static_cast<size_t>(i) + 1];
            auto& dstMip = m_BloomMipChain[static_cast<size_t>(i)];
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
            m_OutputFB->Bind();
            const auto& outSpec = m_OutputFB->GetSpecification();
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

            const u32 sceneColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
            context.BindTexture(0, sceneColorID);
            m_BloomCompositeShader->SetInt("u_SceneColor", 0);

            const u32 bloomColorID = m_BloomMipChain[0]->GetColorAttachmentRendererID(0);
            context.BindTexture(1, bloomColorID);
            m_BloomCompositeShader->SetInt("u_BloomColor", 1);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            m_OutputFB->Unbind();
        }

        // Restore full viewport for downstream passes.
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        context.SetDepthMask(true);
    }

    Ref<Framebuffer> BloomRenderPass::GetTarget() const
    {
        if (!m_Enabled || !m_OutputFB)
            return nullptr;
        return m_OutputFB;
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

        if (!m_OutputFB)
        {
            CreateFramebuffers(width, height);
        }
        else
        {
            m_OutputFB->Resize(width, height);
            CreateMipChain(width, height);
        }
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

        if (m_OutputFB)
            m_OutputFB->Resize(width, height);

        CreateMipChain(width, height);

        OLO_CORE_INFO("BloomRenderPass: Resized to {}x{}", width, height);
    }

    void BloomRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreateFramebuffers(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            OLO_CORE_INFO("BloomRenderPass reset with dimensions {}x{}",
                          m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        }
    }
} // namespace OloEngine
