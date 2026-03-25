#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <span>

namespace OloEngine
{
    // @brief Render pass that draws selection outlines around selected entities.
    //
    // Uses entity-ID edge detection: samples the ScenePass entity-ID attachment
    // and draws outline pixels where the ID transitions between selected and
    // non-selected entities. Single fullscreen pass, no geometry re-rendering.
    //
    // Sits between PostProcessPass and UICompositePass in the render graph:
    //   PostProcess -> SelectionOutline -> UIComposite -> Final
    class SelectionOutlineRenderPass : public RenderPass
    {
      public:
        SelectionOutlineRenderPass();
        ~SelectionOutlineRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
        void SetInputFramebuffer(const Ref<Framebuffer>& input) override;

        // Set the ScenePass framebuffer to read entity IDs from attachment 1
        void SetSceneFramebuffer(const Ref<Framebuffer>& sceneFB);

        // Set selected entity IDs for this frame. Max 64 entities.
        void SetSelectedEntityIDs(std::span<const i32> ids);

        void SetOutlineColor(const glm::vec4& color);
        void SetOutlineWidth(i32 width);
        void SetEnabled(bool enabled);

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Framebuffer> m_InputFramebuffer;
        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<Shader> m_BlitShader;
        Ref<Shader> m_OutlineShader;
        Ref<UniformBuffer> m_OutlineUBO;

        UBOStructures::SelectionOutlineUBO m_UBOData;
        bool m_Enabled = true;
    };
} // namespace OloEngine
