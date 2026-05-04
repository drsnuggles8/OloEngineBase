#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <array>
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
    // Sits between the post chain output and UICompositePass in the render graph:
    //   PostChainOutput -> SelectionOutline -> UIComposite -> Final
    class SelectionOutlineRenderPass : public RenderPass
    {
      public:
        SelectionOutlineRenderPass();
        ~SelectionOutlineRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
        // Set selected entity IDs for this frame. Max 64 entities.
        void SetSelectedEntityIDs(std::span<const i32> ids);

        void SetOutlineColor(const glm::vec4& color);
        void SetOutlineWidth(i32 width);
        void SetEnabled(bool enabled);

        // JFA-specific configuration
        void SetOutlineThickness(f32 inner, f32 outer);
        void SetJFAPassCount(i32 count);

        // Fixed-capacity result for ComputeJFASteps (no heap allocation).
        static constexpr i32 MaxJFAPasses = 4;
        struct JFAStepSequence
        {
            std::array<i32, MaxJFAPasses> Steps{};
            i32 Count = 0;

            [[nodiscard]] auto begin() const
            {
                return Steps.begin();
            }
            [[nodiscard]] auto end() const
            {
                return Steps.begin() + Count;
            }
        };

        // Compute the JFA flood step sequence for a given pass count.
        // Clamps passCount to [1, 4], returns descending powers-of-two: {2^(n-1), ..., 2, 1}.
        [[nodiscard]] static JFAStepSequence ComputeJFASteps(i32 passCount);

      private:
        void CreateFramebuffer(u32 width, u32 height);
        void CreateJFAFramebuffers(u32 width, u32 height);

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
