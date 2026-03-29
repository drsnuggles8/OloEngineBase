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
    // Uses the Jump Flood Algorithm (JFA) to produce smooth, anti-aliased outlines
    // from the ScenePass entity-ID attachment. Multi-pass pipeline:
    //   1. JFA Init  — reads entity IDs, outputs seed distance field
    //   2. JFA Flood — N ping-pong passes propagating nearest seeds
    //   3. JFA Composite — converts distance field to anti-aliased outline
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

        // JFA-specific configuration
        void SetOutlineThickness(f32 inner, f32 outer);
        void SetJFAPassCount(i32 count);

      private:
        void CreateFramebuffer(u32 width, u32 height);
        void CreateJFAFramebuffers(u32 width, u32 height);

        Ref<Framebuffer> m_InputFramebuffer;
        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<Shader> m_BlitShader;

        // Selection ID UBO (binding 27) — shared with JFA Init for entity ID lookup
        Ref<UniformBuffer> m_OutlineUBO;
        UBOStructures::SelectionOutlineUBO m_UBOData;

        // JFA pipeline
        Ref<Shader> m_JFAInitShader;
        Ref<Shader> m_JFAPassShader;
        Ref<Shader> m_JFACompositeShader;
        Ref<Framebuffer> m_JFAFramebuffers[2]; // ping-pong RGBA32F
        Ref<UniformBuffer> m_JFAUbo;
        UBOStructures::JumpFloodUBO m_JFAUboData;
        i32 m_JFAPassCount = 2; // Number of flood passes (1–4)

        bool m_Enabled = true;
    };
} // namespace OloEngine
