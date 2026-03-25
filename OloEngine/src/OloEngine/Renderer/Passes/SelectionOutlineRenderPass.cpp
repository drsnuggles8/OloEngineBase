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
        m_OutlineShader = Shader::Create("assets/shaders/PostProcess_SelectionOutline.glsl");
        m_OutlineUBO = UniformBuffer::Create(UBOStructures::SelectionOutlineUBO::GetSize(), ShaderBindingLayout::UBO_SELECTION_OUTLINE);

        CreateFramebuffer(spec.Width, spec.Height);

        OLO_CORE_INFO("SelectionOutlineRenderPass: Initialized {}x{}", spec.Width, spec.Height);
    }

    void SelectionOutlineRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SelectionOutlineRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
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
        OLO_PROFILE_FUNCTION();

        if (!m_Target || !m_InputFramebuffer || !m_SceneFramebuffer)
        {
            return;
        }

        // Early-out: no selection or disabled — blit input straight through
        if (!m_Enabled || m_UBOData.SelectedCount == 0)
        {
            m_Target->Bind();
            RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            RenderCommand::SetBlendState(false);
            RenderCommand::SetDepthTest(false);

            m_BlitShader->Bind();
            u32 colorAttachment = m_InputFramebuffer->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, colorAttachment);
            m_BlitShader->SetInt("u_Texture", 0);

            auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);

            RenderCommand::SetDepthTest(true);
            return;
        }

        m_Target->Bind();
        RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthTest(false);

        // Update texel size for the current resolution
        m_UBOData.TexelSize = glm::vec4(
            1.0f / static_cast<f32>(m_FramebufferSpec.Width),
            1.0f / static_cast<f32>(m_FramebufferSpec.Height),
            0.0f, 0.0f);

        // Upload UBO data
        m_OutlineUBO->SetData(&m_UBOData, UBOStructures::SelectionOutlineUBO::GetSize());

        // Bind scene color (from PostProcessPass) to slot 0
        u32 colorAttachment = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorAttachment);

        // Bind entity ID texture (from ScenePass attachment 1) to slot 1
        u32 entityIDAttachment = m_SceneFramebuffer->GetColorAttachmentRendererID(1);
        RenderCommand::BindTexture(1, entityIDAttachment);

        m_OutlineShader->Bind();
        m_OutlineShader->SetInt("u_SceneColor", 0);
        m_OutlineShader->SetInt("u_EntityID", 1);

        auto va = MeshPrimitives::GetFullscreenTriangle();
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
    }

    void SelectionOutlineRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreateFramebuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
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
    }

    void SelectionOutlineRenderPass::SetOutlineWidth(i32 width)
    {
        m_UBOData.OutlineWidth = std::max(1, width);
    }

    void SelectionOutlineRenderPass::SetEnabled(bool enabled)
    {
        m_Enabled = enabled;
    }
} // namespace OloEngine
