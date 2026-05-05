#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    MotionBlurRenderPass::MotionBlurRenderPass()
    {
        SetName("MotionBlurPass");
    }

    void MotionBlurRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_MotionBlurShader = Shader::Create("assets/shaders/PostProcess_MotionBlur.glsl");

        DeclareRead(ResourceNames::DOFColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::MotionBlurColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("MotionBlurRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void MotionBlurRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("MotionBlurRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_OutputFB = nullptr;
            return;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

        m_OutputFB = Framebuffer::Create(fbSpec);
    }

    void MotionBlurRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void MotionBlurRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 40 — self-resolving input framebuffer.
        // Prefer DOFColor, then BloomColor, else PostProcessColor.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            const auto inputHandle = board->DOFColor.IsValid()     ? board->DOFColor
                                     : board->BloomColor.IsValid() ? board->BloomColor
                                                                   : board->PostProcessColor;
            if (inputHandle.IsValid())
            {
                if (auto resolved = context.ResolveFramebuffer(inputHandle))
                    inputFramebuffer = resolved;
            }
        }
        if (!m_Enabled || !inputFramebuffer || !m_OutputFB || !m_MotionBlurShader)
        {
            return;
        }

        // Phase F slice 40 / Phase H follow-up — self-resolving SceneDepth.
        u32 sceneDepthTextureID = 0;
        if (const auto* board = context.GetBlackboard())
            sceneDepthTextureID = context.ResolveTexture(board->SceneDepth);

        if (sceneDepthTextureID == 0)
            return;

        if (m_MotionBlurUBO)
            m_MotionBlurUBO->Bind();

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

        m_MotionBlurShader->Bind();

        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        m_MotionBlurShader->SetInt("u_Texture", 0);

        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
        m_MotionBlurShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        m_OutputFB->Unbind();
    }

    Ref<Framebuffer> MotionBlurRenderPass::GetTarget() const
    {
        if (!m_Enabled || !m_OutputFB)
            return nullptr;
        return m_OutputFB;
    }

    void MotionBlurRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void MotionBlurRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void MotionBlurRenderPass::OnReset()
    {
    }
} // namespace OloEngine
