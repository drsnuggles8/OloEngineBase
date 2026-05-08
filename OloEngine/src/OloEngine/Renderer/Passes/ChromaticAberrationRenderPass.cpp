#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

namespace OloEngine
{
    ChromaticAberrationRenderPass::ChromaticAberrationRenderPass()
    {
        SetName("ChromAberrationPass");
    }

    void ChromaticAberrationRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ChromaticAberration.glsl");

        DeclareRead(ResourceNames::FogColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::ChromAbColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("ChromaticAberrationRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ChromaticAberrationRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ChromaticAberrationRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void ChromaticAberrationRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void ChromaticAberrationRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 43 — self-resolving input framebuffer from the render-graph
        // blackboard. Preference chain: Fog > Precipitation > TAA > MotionBlur >
        // DOF > Bloom > PostProcess.
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> inputFramebuffer;
        if (board)
        {
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

        Ref<Framebuffer> outputFramebuffer;
        if (board)
        {
            if (auto fb = context.ResolveFramebuffer(board->ChromAbColor))
                outputFramebuffer = fb;
        }

        if (!m_Enabled)
        {
            m_Target = inputFramebuffer;
            return;
        }

        if (!board || !inputFramebuffer || !outputFramebuffer || !m_Shader)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

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
        outputFramebuffer->Unbind();
    }

    Ref<Framebuffer> ChromaticAberrationRenderPass::GetTarget() const
    {
        if (!m_Target)
            return nullptr;
        return m_Target;
    }

    void ChromaticAberrationRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ChromaticAberrationRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ChromaticAberrationRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
