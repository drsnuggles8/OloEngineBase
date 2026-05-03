#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    void SSSRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create own output framebuffer (RGBA16F, no depth — fullscreen effect)
        CreateOutputFramebuffer(spec.Width, spec.Height);

        // Load SSS blur shader
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");

        // Resource-aware RDG: reads the scene-color output produced by the
        // OIT-resolve stage (SceneColor) and emits SSSColor (its own RGBA16F
        // target). Deriving the OITResolvePass → SSSPass RAW edge on SceneColor
        // and the SSSPass → AOApplyPass RAW edge on SSSColor. When SSS is
        // disabled, SSSRenderPass is a passthrough (GetTarget returns the input
        // framebuffer), but the static declaration still lets the validator
        // synthesise a conservative ordering edge conservatively.
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::SSSColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("SSSRenderPass: Initialized with {}x{} framebuffer", spec.Width, spec.Height);
    }

    void SSSRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void SSSRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 35 — self-resolving input: look up SceneColor directly
        // from the render graph blackboard so no per-frame side-channel setter
        // call is needed from EndScene().
        Ref<Framebuffer> inputFB;
        if (const auto* board = context.GetBlackboard())
        {
            if (board->SceneColor.IsValid())
            {
                if (auto resolvedInput = context.ResolveFramebuffer(board->SceneColor))
                    inputFB = resolvedInput;
            }
        }

        // Only run when snow is enabled AND SSS blur is explicitly turned on.
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled ||
            !inputFB || !m_SSSBlurShader)
        {
            return;
        }

        // SSS UBO is already uploaded by Renderer3D::EndScene each frame.

        m_Target->Bind();

        const auto& targetSpec = m_Target->GetSpecification();
        context.SetViewport(0, 0, targetSpec.Width, targetSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        m_SSSBlurShader->Bind();

        // Bind input scene color as texture — no read-write hazard since we
        // read from inputFB and write to m_Target.
        const auto colorID = inputFB->GetColorAttachmentRendererID(0);
        context.BindTexture(0, colorID);

        // Bind scene depth for bilateral filtering
        const auto depthID = inputFB->GetDepthAttachmentRendererID();
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle(context);

        context.SetDepthMask(true);
        m_Target->Unbind();
    }

    void SSSRenderPass::DrawFullscreenTriangle(RGCommandContext& context)
    {
        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);
    }

    Ref<Framebuffer> SSSRenderPass::GetTarget() const
    {
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
        {
            return nullptr;
        }
        return m_Target;
    }

    void SSSRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateOutputFramebuffer(width, height);
    }

    void SSSRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (m_Target)
        {
            m_Target->Resize(width, height);
        }
    }

    void SSSRenderPass::OnReset()
    {
        // Framebuffer managed by Ref<> — nothing to manually clean up
    }

    void SSSRenderPass::CreateOutputFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        FramebufferSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Samples = 1;
        spec.Attachments = {
            FramebufferTextureFormat::RGBA16F
        };

        m_Target = Framebuffer::Create(spec);
    }
} // namespace OloEngine
