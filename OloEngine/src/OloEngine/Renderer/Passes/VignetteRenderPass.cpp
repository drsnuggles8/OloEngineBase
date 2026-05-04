#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

namespace OloEngine
{
    VignetteRenderPass::VignetteRenderPass()
    {
        SetName("VignettePass");
    }

    void VignetteRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_Vignette.glsl");

        DeclareRead(ResourceNames::ToneMapColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::VignetteColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("VignetteRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void VignetteRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("VignetteRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_OutputFB = nullptr;
            return;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = {
            FramebufferTextureFormat::RGBA8 // LDR — vignette runs after tone mapping.
        };

        m_OutputFB = Framebuffer::Create(fbSpec);
    }

    void VignetteRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void VignetteRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 43 — self-resolving input framebuffer from the render-graph
        // blackboard. Vignette runs after tone mapping, so ToneMapColor is the
        // primary input. Fall back toward earlier HDR chain outputs only when
        // tone mapping is unavailable.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->ToneMapColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->ColorGradingColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->ChromAbColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->FogColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->PrecipitationColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->TAAColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->MotionBlurColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->DOFColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->BloomColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->PostProcessColor))
                    inputFramebuffer = fb;
        }
        if (!m_Enabled || !inputFramebuffer || !m_OutputFB || !m_Shader)
        {
            return;
        }

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

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

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_Shader->Bind();

        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        m_Shader->SetInt("u_Texture", 0);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        m_OutputFB->Unbind();
    }

    Ref<Framebuffer> VignetteRenderPass::GetTarget() const
    {
        if (!m_Enabled || !m_OutputFB)
            return nullptr;
        return m_OutputFB;
    }

    void VignetteRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void VignetteRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void VignetteRenderPass::OnReset()
    {
    }
} // namespace OloEngine
