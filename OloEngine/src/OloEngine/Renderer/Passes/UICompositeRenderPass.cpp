#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <span>

#include <glad/gl.h>

namespace OloEngine
{
    UICompositeRenderPass::UICompositeRenderPass()
    {
        SetName("UICompositePass");
        OLO_CORE_INFO("Creating UICompositeRenderPass.");
    }

    void UICompositeRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");

        CreateFramebuffer(spec.Width, spec.Height);

        // Resource-aware RDG: composites post-processed LDR scene + UI.
        // Declare the full upstream preference chain plus a SceneColor
        // emergency fallback so reachability/order inference remains robust
        // even when intermediate effects are disabled.
        DeclareRead(ResourceNames::ToneMapColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::ColorGradingColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::ChromAbColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::FogColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::PrecipitationColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::TAAColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::MotionBlurColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::DOFColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::BloomColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::PostProcessColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::VignetteColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::FXAAColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SelectionOutlineColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::UIComposite, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("UICompositeRenderPass: Initialized {}x{}", spec.Width, spec.Height);
    }

    void UICompositeRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        // Match the ScenePass MRT layout so Renderer2D shaders (which output to 3
        // locations: color, entity ID, view-normal) don't trigger NVIDIA driver
        // shader recompilation when the draw-buffer configuration changes.
        fbSpec.Attachments = {
            FramebufferTextureFormat::RGBA8,       // [0] LDR color (composited scene + UI)
            FramebufferTextureFormat::RED_INTEGER, // [1] Entity ID (for editor picking)
            FramebufferTextureFormat::RG16F        // [2] View-normal (unused, but keeps MRT layout stable)
        };

        m_Target = Framebuffer::Create(fbSpec);
    }

    void UICompositeRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void UICompositeRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 44 — self-resolving input framebuffer from the render-graph
        // blackboard. Preference chain: SceneColor > FXAA > Vignette > ToneMap >
        // ColorGrading > ChromAb > Fog > Precipitation > TAA > MotionBlur >
        // DOF > Bloom > PostProcess. SelectionOutlineColor is
        // intentionally not used as the primary scene source here; it is an
        // overlay product, and if that buffer is empty we must not replace
        // the entire frame with black.
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

            tryResolveValid(board->SceneColor);
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
        }
        if (!m_Target)
        {
            return;
        }

        // Bind our FBO and clear all attachments (handles mixed integer/float types)
        m_Target->Bind();
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        m_Target->ClearAllAttachments({ 0.0f, 0.0f, 0.0f, 1.0f }, -1);

        // Blit the post-processed scene as background
        if (inputFramebuffer && m_BlitShader)
        {
            m_BlitShader->Bind();
            const auto inputColorAttachment = inputFramebuffer->GetColorAttachmentRendererID(0);
            if (inputColorAttachment == 0)
            {
                static u32 s_InvalidInputColorWarnings = 0;
                if (s_InvalidInputColorWarnings++ < 10)
                {
                    const auto& inSpec = inputFramebuffer->GetSpecification();
                    OLO_CORE_WARN("UICompositePass: input framebuffer has invalid color attachment 0 (id=0). Size={}x{}, attachmentCount={}",
                                  inSpec.Width, inSpec.Height, inSpec.Attachments.Attachments.size());
                }
            }
            context.BindTexture(0, inputColorAttachment);
            m_BlitShader->SetInt("u_Texture", 0);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        else if (m_NoInputWarningCount++ < 5)
        {
            OLO_CORE_WARN("UICompositePass: No input framebuffer ({}) or blit shader ({}) — scene background will be black",
                          inputFramebuffer != nullptr, m_BlitShader != nullptr);
        }

        // Render 2D overlays and screen-space UI via the per-frame callback
        if (m_RenderCallback)
        {
            context.SetBlendState(true);
            context.SetAlphaBlendStandard();
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);

            m_RenderCallback();

            // Restore GL state so later passes don't inherit altered state
            context.SetBlendState(false);
            context.SetOpaqueReplaceBlend();
            context.SetDepthTest(true);
            context.SetDepthMask(true);
            context.SetCulling(true);

            // One-shot: clear for next frame
            m_RenderCallback = nullptr;
        }
        else if (m_NoCallbackWarningCount++ < 5)
        {
            OLO_CORE_WARN("UICompositePass: No render callback set — UI will not render this frame");
        }
    }

    Ref<Framebuffer> UICompositeRenderPass::GetTarget() const
    {
        return m_Target;
    }

    void UICompositeRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateFramebuffer(width, height);

        OLO_CORE_INFO("UICompositeRenderPass: Setup {}x{}", width, height);
    }

    void UICompositeRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("UICompositeRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (m_Target)
        {
            m_Target->Resize(width, height);
        }

        OLO_CORE_INFO("UICompositeRenderPass: Resized to {}x{}", width, height);
    }

    void UICompositeRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreateFramebuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        }
    }

    void UICompositeRenderPass::SetRenderCallback(RenderCallback callback)
    {
        m_RenderCallback = std::move(callback);
    }
} // namespace OloEngine
