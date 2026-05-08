#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

namespace OloEngine
{
    ToneMapRenderPass::ToneMapRenderPass()
    {
        SetName("ToneMapPass");
    }

    void ToneMapRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");

        DeclareRead(ResourceNames::ColorGradingColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::ToneMapColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("ToneMapRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ToneMapRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ToneMapRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void ToneMapRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void ToneMapRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 43 — self-resolving input framebuffer from the render-graph
        // blackboard. Preference chain: Fog > Precipitation > TAA > MotionBlur >
        // DOF > Bloom > PostProcess.
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> inputFramebuffer;
        if (board)
        {
            auto tryResolveValid = [&](const auto& handle)
            {
                if (inputFramebuffer || !handle.IsValid())
                    return;
                if (auto fb = context.ResolveFramebuffer(handle))
                {
                    if (fb->GetColorAttachmentRendererID(0) != 0)
                        inputFramebuffer = fb;
                }
            };

            tryResolveValid(board->FogColor);
            tryResolveValid(board->PrecipitationColor);
            tryResolveValid(board->TAAColor);
            tryResolveValid(board->MotionBlurColor);
            tryResolveValid(board->DOFColor);
            tryResolveValid(board->BloomColor);
            tryResolveValid(board->PostProcessColor);
            tryResolveValid(board->SceneColor);
        }

        Ref<Framebuffer> outputFramebuffer;
        if (board)
        {
            if (auto fb = context.ResolveFramebuffer(board->ToneMapColor))
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
            static u32 s_MissingInputWarnings = 0;
            if (outputFramebuffer && m_Shader && !inputFramebuffer && s_MissingInputWarnings++ < 10)
            {
                OLO_CORE_WARN("ToneMapRenderPass: No valid input framebuffer resolved (fallback chain exhausted)");
            }
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

    Ref<Framebuffer> ToneMapRenderPass::GetTarget() const
    {
        if (!m_Target)
            return nullptr;
        return m_Target;
    }

    void ToneMapRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ToneMapRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ToneMapRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
