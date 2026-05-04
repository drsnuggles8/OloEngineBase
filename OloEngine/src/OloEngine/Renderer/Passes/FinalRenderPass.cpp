#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

namespace OloEngine
{
    FinalRenderPass::FinalRenderPass()
    {
        SetName("FinalRenderPass");
        OLO_CORE_INFO("Creating FinalRenderPass.");
    }

    void FinalRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Load or create the blit shader
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");

        // Resource-aware RDG: final pass reads the composited (post-process
        // + UI) color piped in via SetInputFramebuffer and presents to the
        // backbuffer / swapchain image as a side effect.
        DeclareRead(ResourceNames::UIComposite, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::Backbuffer, ResourceHandle::Kind::Framebuffer);

        // Phase E/F: Mark this pass as side-effecting (Present) so it's never culled.
        // The final pass writes to the swap-chain and must always execute.
        SetSideEffects(RenderPass::SideEffect::Present);

        OLO_CORE_INFO("FinalRenderPass: Initialized with viewport dimensions {}x{}",
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void FinalRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void FinalRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 44 — self-resolving input framebuffer from the render-graph
        // blackboard. FinalPass reads UIComposite.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
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

            // Primary source + conservative fallback chain.
            tryResolveValid(board->UIComposite);
            tryResolveValid(board->SelectionOutlineColor);
            tryResolveValid(board->FXAAColor);
            tryResolveValid(board->VignetteColor);
            tryResolveValid(board->ToneMapColor);
            tryResolveValid(board->ColorGradingColor);
            tryResolveValid(board->ChromAbColor);
            tryResolveValid(board->FogColor);
            tryResolveValid(board->PrecipitationColor);
            tryResolveValid(board->TAAColor);
            tryResolveValid(board->MotionBlurColor);
            tryResolveValid(board->DOFColor);
            tryResolveValid(board->BloomColor);
            tryResolveValid(board->PostProcessColor);
            tryResolveValid(board->SceneColor);
        }
        context.ResetGraphicsStateToDefault();
        context.BindDefaultFramebuffer();
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        if (!inputFramebuffer || !m_BlitShader)
        {
            static u32 s_MissingFinalInputWarnings = 0;
            if (s_MissingFinalInputWarnings++ < 10)
            {
                OLO_CORE_WARN("FinalRenderPass: missing input framebuffer ({}) or blit shader ({})",
                              inputFramebuffer != nullptr, m_BlitShader != nullptr);
            }
            return;
        }

        m_BlitShader->Bind();

        // Bind the color attachment from the input framebuffer as a texture
        const auto colorAttachmentID = inputFramebuffer->GetColorAttachmentRendererID(0);
        if (colorAttachmentID == 0)
        {
            static u32 s_InvalidFinalInputWarnings = 0;
            if (s_InvalidFinalInputWarnings++ < 10)
            {
                const auto& inSpec = inputFramebuffer->GetSpecification();
                OLO_CORE_WARN("FinalRenderPass: input framebuffer color attachment 0 is invalid (id=0). Size={}x{}, attachmentCount={}",
                              inSpec.Width, inSpec.Height, inSpec.Attachments.Attachments.size());
            }
        }
        context.BindTexture(0, colorAttachmentID);
        m_BlitShader->SetInt("u_Texture", 0);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);
    }

    Ref<Framebuffer> FinalRenderPass::GetTarget() const
    {
        return m_Target;
    }

    void FinalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        OLO_CORE_INFO("FinalRenderPass setup with dimensions: {}x{}", width, height);
    }

    void FinalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FinalRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }

        // Update stored dimensions
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        OLO_CORE_INFO("FinalRenderPass: Resized viewport to {}x{}", width, height);
    }

    void FinalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        // TODO: Recreate the fullscreen triangle and shader if needed
    }
} // namespace OloEngine
