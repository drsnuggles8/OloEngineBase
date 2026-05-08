#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

namespace OloEngine
{
    FXAARenderPass::FXAARenderPass()
    {
        SetName("FXAAPass");
    }

    void FXAARenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_FXAAShader = Shader::Create("assets/shaders/PostProcess_FXAA.glsl");

        // Resource-aware RDG: FXAA samples PostProcessColor and writes
        // FXAAColor. Both edges are gated at the Renderer3D level — when
        // FXAA receives Vignette's output (VignetteColor) as its input.
        // When FXAA is absent the blackboard never imports FXAAColor and
        // the executor short-circuits this pass.
        DeclareRead(ResourceNames::VignetteColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::FXAAColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("FXAARenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void FXAARenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FXAARenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void FXAARenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void FXAARenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 44 — self-resolving input framebuffer from the render-graph
        // blackboard. Preference chain: Vignette > ToneMap > ColorGrading > ChromAb >
        // Fog > Precipitation > TAA > MotionBlur > DOF > Bloom > PostProcess.
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> inputFramebuffer;
        if (board)
        {
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->VignetteColor))
                    inputFramebuffer = fb;
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

        Ref<Framebuffer> outputFramebuffer;
        if (board)
        {
            if (auto fb = context.ResolveFramebuffer(board->FXAAColor))
                outputFramebuffer = fb;
        }

        if (!m_Enabled)
        {
            m_Target = inputFramebuffer;
            return;
        }

        if (!board || !inputFramebuffer || !outputFramebuffer || !m_FXAAShader)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        // PostProcessUBO (binding 7) is uploaded once per frame by Renderer3D
        // before the post-process chain runs. SetData() does not restore
        // the indexed binding and other passes (IBL precompute, bloom mip
        // updates) bind binding 7 transiently, so re-bind here so the FXAA
        // shader reads the expected `u_TexelSize` / gamma values.
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        outputFramebuffer->Bind();

        const auto& outSpec = outputFramebuffer->GetSpecification();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        // Mirror the shared fullscreen colour-pass state setup — prefer
        // RGCommandContext setters where available (so graph hazard tracking
        // stays accurate) and fall back to RenderCommand for state the
        // context does not currently expose (stencil / scissor / polygon
        // mode / colour mask).
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

        m_FXAAShader->Bind();

        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        m_FXAAShader->SetInt("u_Texture", 0);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    Ref<Framebuffer> FXAARenderPass::GetTarget() const
    {
        if (!m_Target)
            return nullptr;
        return m_Target;
    }

    void FXAARenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void FXAARenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void FXAARenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
