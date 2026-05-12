#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone depth-of-field post-process pass.
    //
    // Phase F slice 22 — standalone DOF stage in the dynamic chain:
    //   AOApply/Bloom -> DOF -> MotionBlur -> TAA -> ...
    //
    // Inputs:
    //   * Input framebuffer handle (typically PostProcessColor), selected during `Setup()`
    //   * Scene depth texture handle (required), selected during `Setup()`
    //   * PostProcessUBO (binding 7), uploaded by Renderer3D
    //
    // Output:
    //   * DOFColor (RGBA16F)
    //
    // Passthrough semantics: when disabled, the pass no-ops and GetTarget()
    // returns the input framebuffer.
    class DOFRenderPass : public RenderGraphNode
    {
      public:
        DOFRenderPass();
        ~DOFRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept
        {
            return m_DOFShader && m_DOFShader->IsReady() && m_PostProcessUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

      private:
        bool m_Enabled = false;

        Ref<Shader> m_DOFShader;
        Ref<UniformBuffer> m_PostProcessUBO;
        RGTextureHandle m_SelectedSceneDepthTexture{};
    };
} // namespace OloEngine
