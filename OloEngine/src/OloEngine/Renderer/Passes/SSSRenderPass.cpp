#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <span>

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

        // Track framebuffer metadata for the graph-owned current-frame output.
        CreateOutputFramebuffer(spec.Width, spec.Height);

        // Load SSS blur shader
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");

        // Resource-aware RDG: reads the scene-color output produced by the
        // OIT-resolve stage (SceneColor) and emits the graph-owned SSSColor
        // framebuffer. This derives the OITResolvePass → SSSPass RAW edge on
        // SceneColor and the SSSPass → AOApplyPass RAW edge on SSSColor.
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
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> outputFramebuffer;
        if (board)
        {
            if (board->SceneColor.IsValid())
            {
                if (auto resolvedInput = context.ResolveFramebuffer(board->SceneColor))
                    inputFB = resolvedInput;
            }
            if (board->SSSColor.IsValid())
            {
                if (auto resolvedOutput = context.ResolveFramebuffer(board->SSSColor))
                    outputFramebuffer = resolvedOutput;
            }
        }

        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputFB || !outputFramebuffer)
        {
            m_Target = nullptr;
            return;
        }

        if (!IsReadyForExecution())
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        const auto& targetSpec = outputFramebuffer->GetSpecification();
        constexpr u32 colorAttachment = 0;

        // SSS UBO is already uploaded by Renderer3D::EndScene each frame.

        outputFramebuffer->Bind();

        context.SetViewport(0, 0, targetSpec.Width, targetSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        m_SSSBlurShader->Bind();

        // Bind input scene color as texture — no read-write hazard since we
        // read from inputFB and write to the graph-owned SSSColor target.
        const auto colorID = inputFB->GetColorAttachmentRendererID(0);
        context.BindTexture(0, colorID);

        // Bind scene depth for bilateral filtering
        const auto depthID = inputFB->GetDepthAttachmentRendererID();
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle(context);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
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

        CreateOutputFramebuffer(width, height);
    }

    void SSSRenderPass::OnReset()
    {
        m_Target = nullptr;
    }

    void SSSRenderPass::CreateOutputFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }
} // namespace OloEngine
