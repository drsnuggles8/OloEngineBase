#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

#include <algorithm>

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
        CreateJFAFramebuffers(spec.Width, spec.Height);

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

    void SelectionOutlineRenderPass::CreateJFAFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            m_JFAFramebuffers[0] = nullptr;
            m_JFAFramebuffers[1] = nullptr;
            return;
        }

        for (auto& fb : m_JFAFramebuffers)
        {
            FramebufferSpecification fbSpec;
            fbSpec.Width = width;
            fbSpec.Height = height;
            fbSpec.Samples = 1;
            fbSpec.Attachments = {
                FramebufferTextureFormat::RGBA32F // Distance field (xy=offset, z=sqDist, w=flag)
            };
            fb = Framebuffer::Create(fbSpec);
        }
    }

    void SelectionOutlineRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Target || !m_InputFramebuffer)
        {
            return;
        }

        auto va = MeshPrimitives::GetFullscreenTriangle();

        // Early-out: no selection, disabled, or no scene FB — blit input straight through
        if (!m_Enabled || m_UBOData.SelectedCount == 0 || !m_SceneFramebuffer)
        {
            m_Target->Bind();
            RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            RenderCommand::SetBlendState(false);
            RenderCommand::SetDepthTest(false);

            m_BlitShader->Bind();
            RenderCommand::BindTexture(0, m_InputFramebuffer->GetColorAttachmentRendererID(0));
            m_BlitShader->SetInt("u_Texture", 0);

            va->Bind();
            RenderCommand::DrawIndexed(va);

            RenderCommand::SetDepthTest(true);
            return;
        }

        const u32 w = m_FramebufferSpec.Width;
        const u32 h = m_FramebufferSpec.Height;
        const glm::vec4 texelSize(1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h), 0.0f, 0.0f);

        // Upload SelectionOutlineUBO (entity IDs for Init pass)
        m_UBOData.TexelSize = texelSize;
        m_OutlineUBO->SetData(&m_UBOData, UBOStructures::SelectionOutlineUBO::GetSize());

        // =====================================================================
        // Pass 1: JFA Init — entity IDs → distance field seed
        // =====================================================================
        m_JFAFramebuffers[0]->Bind();
        RenderCommand::SetViewport(0, 0, w, h);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthTest(false);
        RenderCommand::Clear();

        // Bind entity ID texture from ScenePass attachment 1
        RenderCommand::BindTexture(0, m_SceneFramebuffer->GetColorAttachmentRendererID(1));

        m_JFAInitShader->Bind();
        m_JFAInitShader->SetInt("u_EntityID", 0);

        va->Bind();
        RenderCommand::DrawIndexed(va);

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

            m_JFAFramebuffers[writeIndex]->Bind();
            RenderCommand::SetViewport(0, 0, w, h);
            RenderCommand::Clear();

            // Bind previous JFA result
            RenderCommand::BindTexture(0, m_JFAFramebuffers[readIndex]->GetColorAttachmentRendererID(0));

            m_JFAPassShader->Bind();
            m_JFAPassShader->SetInt("u_Texture", 0);

            va->Bind();
            RenderCommand::DrawIndexed(va);

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
        RenderCommand::SetViewport(0, 0, w, h);

        // Slot 0: scene color from PostProcessPass
        RenderCommand::BindTexture(0, m_InputFramebuffer->GetColorAttachmentRendererID(0));
        // Slot 1: final JFA distance field
        RenderCommand::BindTexture(1, m_JFAFramebuffers[readIndex]->GetColorAttachmentRendererID(0));

        m_JFACompositeShader->Bind();
        m_JFACompositeShader->SetInt("u_SceneColor", 0);
        m_JFACompositeShader->SetInt("u_JFAResult", 1);

        va->Bind();
        RenderCommand::DrawIndexed(va);

        // Restore state
        RenderCommand::SetDepthTest(true);
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
        CreateJFAFramebuffers(width, height);
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

        // Recreate JFA framebuffers (RGBA32F can't always resize in place)
        CreateJFAFramebuffers(width, height);
    }

    void SelectionOutlineRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreateFramebuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            CreateJFAFramebuffers(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        }
    }

    void SelectionOutlineRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input)
    {
        m_InputFramebuffer = input;
    }

    void SelectionOutlineRenderPass::SetSceneFramebuffer(const Ref<Framebuffer>& sceneFB)
    {
        m_SceneFramebuffer = sceneFB;
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

    std::vector<i32> SelectionOutlineRenderPass::ComputeJFASteps(i32 passCount)
    {
        passCount = std::clamp(passCount, 1, 4);
        std::vector<i32> steps;
        steps.reserve(static_cast<size_t>(passCount));
        for (i32 step = 1 << (passCount - 1); step >= 1; step >>= 1)
        {
            steps.push_back(step);
        }
        return steps;
    }
} // namespace OloEngine
