#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
    AOApplyRenderPass::AOApplyRenderPass()
    {
        SetName("AOApplyPass");
        OLO_CORE_INFO("Creating AOApplyRenderPass.");
    }

    void AOApplyRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        CreateFramebuffer(spec.Width, spec.Height);

        m_SSAOApplyShader = Shader::Create("assets/shaders/PostProcess_SSAOApply.glsl");

        // Resource-aware RDG: AO Apply reads the accumulated scene color
        // (SSSColor or SceneColor), the AO buffer, and the scene depth (for
        // bilateral upsampling), and emits AO-modulated HDR scene color.
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SSSColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::AOBuffer, ResourceHandle::Kind::Texture2D);
        DeclareRead(ResourceNames::SceneDepth, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::AOApplyColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("AOApplyRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void AOApplyRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
        m_OutputFB = Framebuffer::Create(fbSpec);
    }

    Ref<Framebuffer> AOApplyRenderPass::GetTarget() const
    {
        return m_OutputFB;
    }

    void AOApplyRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void AOApplyRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Self-resolving input framebuffer: prefer SSSColor
        // if written this frame, fall back to SceneColor.  Mirrors the
        // conditional that EndScene() previously computed and pushed via the
        // side-channel setter.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            const auto inputHandle = board->SSSColor.IsValid() ? board->SSSColor : board->SceneColor;
            if (inputHandle.IsValid())
            {
                if (auto resolved = context.ResolveFramebuffer(inputHandle))
                    inputFramebuffer = resolved;
            }
        }
        // Self-resolving AO texture and SceneDepth.
        u32 aoTextureID = 0;
        if (const auto* board = context.GetBlackboard())
            aoTextureID = context.ResolveTexture(board->AOBuffer);

        u32 sceneDepthID = m_SceneDepthTextureID;
        if (sceneDepthID == 0)
        {
            if (const auto* board = context.GetBlackboard())
                sceneDepthID = context.ResolveTexture(board->SceneDepth);
        }

        if (!inputFramebuffer || !m_OutputFB)
        {
            return;
        }

        const bool canApplyAO = m_Enabled && m_SSAOApplyShader &&
                                aoTextureID != 0 && sceneDepthID != 0;
        if (!canApplyAO)
        {
            // Robust fallback: pass input through unchanged so downstream
            // PostProcessColor never points at an uninitialized/black target.
            // This avoids frame-to-frame black propagation when AO is
            // temporarily unavailable (startup, resize, technique toggles).
            const u32 srcFbo = inputFramebuffer->GetRendererID();
            const u32 dstFbo = m_OutputFB->GetRendererID();
            const auto& srcSpec = inputFramebuffer->GetSpecification();
            const auto& dstSpec = m_OutputFB->GetSpecification();

            glNamedFramebufferReadBuffer(srcFbo, GL_COLOR_ATTACHMENT0);
            glNamedFramebufferDrawBuffer(dstFbo, GL_COLOR_ATTACHMENT0);
            glBlitNamedFramebuffer(
                srcFbo, dstFbo,
                0, 0, static_cast<GLint>(srcSpec.Width), static_cast<GLint>(srcSpec.Height),
                0, 0, static_cast<GLint>(dstSpec.Width), static_cast<GLint>(dstSpec.Height),
                GL_COLOR_BUFFER_BIT, GL_NEAREST);

            static u32 s_AOPassthroughWarnings = 0;
            if (s_AOPassthroughWarnings++ < 10)
            {
                OLO_CORE_WARN("AOApplyRenderPass: passthrough fallback (enabled={}, shader={}, aoTex={}, depthTex={})",
                              m_Enabled, m_SSAOApplyShader != nullptr, aoTextureID, sceneDepthID);
            }
            return;
        }

        // Rebind the PostProcessUBO before any fullscreen shader reads it.
        // SetData() updates the buffer object but does not restore the
        // indexed binding (IBL precompute also uses binding 7).
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        constexpr u32 colorAttachment = 0;
        m_OutputFB->Bind();

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_SSAOApplyShader->Bind();
        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        context.BindTexture(ShaderBindingLayout::TEX_SSAO, aoTextureID);
        // Scene depth is used by the apply shader for bilateral upsampling.
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        RenderCommand::SetDepthMask(true);
        m_OutputFB->Unbind();
    }

    void AOApplyRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        CreateFramebuffer(width, height);
    }

    void AOApplyRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (m_OutputFB)
            m_OutputFB->Resize(width, height);
    }

    void AOApplyRenderPass::OnReset()
    {
        CreateFramebuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

} // namespace OloEngine
