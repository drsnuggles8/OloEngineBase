#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>
#include <span>

#include <glad/gl.h>

namespace OloEngine
{
    SelectionOutlineRenderPass::SelectionOutlineRenderPass()
    {
        SetName("SelectionOutlinePass");
    }

    void SelectionOutlineRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");

        // JFA shaders
        m_JFAInitShader = Shader::Create("assets/shaders/JumpFlood_Init.glsl");
        m_JFAPassShader = Shader::Create("assets/shaders/JumpFlood_Pass.glsl");
        m_JFACompositeShader = Shader::Create("assets/shaders/JumpFlood_Composite.glsl");

        // UBOs
        m_OutlineUBO = UniformBuffer::Create(UBOStructures::SelectionOutlineUBO::GetSize(), ShaderBindingLayout::UBO_SELECTION_OUTLINE);
        m_JFAUbo = UniformBuffer::Create(UBOStructures::JumpFloodUBO::GetSize(), ShaderBindingLayout::UBO_JUMP_FLOOD);

        CreateFramebuffer(spec.Width, spec.Height);

        // Graph-visible contract: receives VignetteColor when FXAA is absent,
        // or FXAAColor when FXAA precedes it. SceneColor is a conservative
        // fallback so the pass can still blit the scene through when the late
        // post chain is unavailable.
        DeclareRead(ResourceNames::VignetteColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::FXAAColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::SelectionOutlineColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("SelectionOutlineRenderPass: Initialized {}x{} (JFA, {} passes)", spec.Width, spec.Height, m_JFAPassCount);
    }

    void SelectionOutlineRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SelectionOutlineRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = {
            FramebufferTextureFormat::RGBA8 // [0] LDR color (scene + outline)
        };

        m_Target = Framebuffer::Create(fbSpec);
    }

    void SelectionOutlineRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void SelectionOutlineRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 44 — self-resolving input framebuffer from the render-graph
        // blackboard. Preference chain: FXAA > Vignette > ToneMap > ColorGrading >
        // ChromAb > Fog > Precipitation > TAA > MotionBlur > DOF > Bloom > PostProcess > SceneColor.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            // Self-resolve the scene framebuffer (entity-ID attachment source).
            if (auto fb = context.ResolveFramebuffer(board->SceneColor))
                m_SceneFramebuffer = fb;

            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->FXAAColor))
                    inputFramebuffer = fb;
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
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->SceneColor))
                    inputFramebuffer = fb;
        }
        if (!m_Target || !inputFramebuffer)
        {
            return;
        }

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        constexpr u32 colorAttachment = 0;

        // Early-out: no selection, disabled, or no scene FB — blit input straight through
        if (!m_Enabled || m_UBOData.SelectedCount == 0 || !m_SceneFramebuffer)
        {
            m_Target->Bind();
            context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
            context.SetBlendState(false);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);

            m_BlitShader->Bind();
            context.BindTexture(0, inputFramebuffer->GetColorAttachmentRendererID(0));
            m_BlitShader->SetInt("u_Texture", 0);

            va->Bind();
            context.DrawIndexed(va);

            context.SetDepthTest(true);
            return;
        }

        // Phase D / H follow-up: resolve JFA ping-pong framebuffers entirely
        // from the transient pool. The execute path no longer keeps owned
        // fallback framebuffers for headless / unit-test contexts.
        std::array<Ref<Framebuffer>, 2> jfaFBs{};
        if (const auto* board = context.GetBlackboard())
        {
            if (auto fb = context.ResolveFramebuffer(board->JFAPing))
                jfaFBs[0] = fb;
            if (auto fb = context.ResolveFramebuffer(board->JFAPong))
                jfaFBs[1] = fb;
        }
        if (!jfaFBs[0] || !jfaFBs[1])
            return;

        const u32 w = m_FramebufferSpec.Width;
        const u32 h = m_FramebufferSpec.Height;
        const glm::vec4 texelSize(1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h), 0.0f, 0.0f);

        // Upload SelectionOutlineUBO (entity IDs for Init pass)
        m_UBOData.TexelSize = texelSize;
        m_OutlineUBO->SetData(&m_UBOData, UBOStructures::SelectionOutlineUBO::GetSize());

        // =====================================================================
        // Pass 1: JFA Init — entity IDs → distance field seed
        // =====================================================================
        jfaFBs[0]->Bind();
        context.SetViewport(0, 0, w, h);
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        context.SetBlendState(false);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        context.Clear();

        // Bind entity ID texture from ScenePass attachment 1
        context.BindTexture(0, m_SceneFramebuffer->GetColorAttachmentRendererID(1));

        m_JFAInitShader->Bind();
        m_JFAInitShader->SetInt("u_EntityID", 0);

        va->Bind();
        context.DrawIndexed(va);

        // =====================================================================
        // Pass 2: JFA Flood — ping-pong propagation passes
        // =====================================================================
        auto const jfaSteps = ComputeJFASteps(m_JFAPassCount);
        i32 readIndex = 0;

        for (i32 step : jfaSteps)
        {
            i32 writeIndex = (readIndex + 1) % 2;

            // Update JFA UBO with current step
            m_JFAUboData.TexelSize = texelSize;
            m_JFAUboData.Step = step;
            m_JFAUbo->SetData(&m_JFAUboData, UBOStructures::JumpFloodUBO::GetSize());

            jfaFBs[writeIndex]->Bind();
            context.SetViewport(0, 0, w, h);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
            context.SetBlendState(false);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.Clear();

            // Bind previous JFA result
            context.BindTexture(0, jfaFBs[readIndex]->GetColorAttachmentRendererID(0));

            m_JFAPassShader->Bind();
            m_JFAPassShader->SetInt("u_Texture", 0);

            va->Bind();
            context.DrawIndexed(va);

            readIndex = writeIndex;
        }

        // =====================================================================
        // Pass 3: JFA Composite — distance field → anti-aliased outline
        // =====================================================================
        // Upload final JFA UBO with outline parameters
        m_JFAUboData.TexelSize = texelSize;
        m_JFAUboData.Step = 0; // Not used in composite
        m_JFAUbo->SetData(&m_JFAUboData, UBOStructures::JumpFloodUBO::GetSize());

        m_Target->Bind();
        context.SetViewport(0, 0, w, h);
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        context.SetBlendState(false);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);

        // Slot 0: scene color from dynamic post chain output
        context.BindTexture(0, inputFramebuffer->GetColorAttachmentRendererID(0));
        // Slot 1: final JFA distance field (from transient or fallback JFA ping-pong)
        context.BindTexture(1, jfaFBs[readIndex]->GetColorAttachmentRendererID(0));

        m_JFACompositeShader->Bind();
        m_JFACompositeShader->SetInt("u_SceneColor", 0);
        m_JFACompositeShader->SetInt("u_JFAResult", 1);

        va->Bind();
        context.DrawIndexed(va);

        // Restore state
        context.SetDepthTest(true);
    }

    Ref<Framebuffer> SelectionOutlineRenderPass::GetTarget() const
    {
        return m_Target;
    }

    void SelectionOutlineRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateFramebuffer(width, height);
    }

    void SelectionOutlineRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SelectionOutlineRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (m_Target)
        {
            m_Target->Resize(width, height);
        }
        else
        {
            CreateFramebuffer(width, height);
        }
    }

    void SelectionOutlineRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreateFramebuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        }
    }

    void SelectionOutlineRenderPass::SetSelectedEntityIDs(std::span<const i32> ids)
    {
        constexpr u32 maxEntities = UBOStructures::SelectionOutlineUBO::MaxSelectedEntities;
        u32 count = static_cast<u32>(std::min(ids.size(), static_cast<size_t>(maxEntities)));

        m_UBOData.SelectedCount = static_cast<i32>(count);

        // Zero out all IDs first (use -1 as "no entity" sentinel)
        for (auto& v : m_UBOData.SelectedIDs)
        {
            v = glm::ivec4(-1);
        }

        // Pack IDs into ivec4 array
        for (u32 i = 0; i < count; ++i)
        {
            u32 vecIndex = i / 4;
            u32 compIndex = i % 4;
            m_UBOData.SelectedIDs[vecIndex][compIndex] = ids[i];
        }
    }

    void SelectionOutlineRenderPass::SetOutlineColor(const glm::vec4& color)
    {
        m_UBOData.OutlineColor = color;
        m_JFAUboData.OutlineColor = color;
    }

    void SelectionOutlineRenderPass::SetOutlineWidth(i32 width)
    {
        m_UBOData.OutlineWidth = std::max(1, width);

        // Translate integer pixel width into JFA smoothstep thickness.
        // Inner/outer define the anti-aliased band in normalized screen distance.
        constexpr f32 baseInner = 0.002f;
        constexpr f32 baseOuter = 0.004f;
        f32 scale = static_cast<f32>(m_UBOData.OutlineWidth);
        m_JFAUboData.OutlineThicknessInner = baseInner * scale;
        m_JFAUboData.OutlineThicknessOuter = baseOuter * scale;
    }

    void SelectionOutlineRenderPass::SetEnabled(bool enabled)
    {
        m_Enabled = enabled;
    }

    void SelectionOutlineRenderPass::SetOutlineThickness(f32 inner, f32 outer)
    {
        if (inner < 0.0f || outer < 0.0f || inner >= outer)
        {
            OLO_CORE_ERROR("SelectionOutlineRenderPass::SetOutlineThickness: invalid values (inner={}, outer={}). "
                           "Requires inner >= 0, outer >= 0, inner < outer.",
                           inner, outer);
            return;
        }
        m_JFAUboData.OutlineThicknessInner = inner;
        m_JFAUboData.OutlineThicknessOuter = outer;
    }

    void SelectionOutlineRenderPass::SetJFAPassCount(i32 count)
    {
        m_JFAPassCount = std::clamp(count, 1, 4);
    }

    SelectionOutlineRenderPass::JFAStepSequence SelectionOutlineRenderPass::ComputeJFASteps(i32 passCount)
    {
        passCount = std::clamp(passCount, 1, MaxJFAPasses);
        JFAStepSequence seq;
        for (i32 step = 1 << (passCount - 1); step >= 1; step >>= 1)
        {
            seq.Steps[static_cast<size_t>(seq.Count++)] = step;
        }
        return seq;
    }
} // namespace OloEngine
