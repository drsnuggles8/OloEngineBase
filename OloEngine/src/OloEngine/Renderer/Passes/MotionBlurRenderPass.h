#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone motion-blur post-process pass.
    //
    // Phase F slice 21 — standalone motion-blur stage in the dynamic chain:
    //   AOApply/Bloom/DOF -> MotionBlur -> TAA -> Precipitation -> Fog -> ...
    //
    // Inputs:
    //   * Input framebuffer handle (typically PostProcessColor), selected during `Setup()`
    //   * Scene depth texture handle (required), selected during `Setup()`
    //   * MotionBlurUBO (binding 8), uploaded by Renderer3D
    //
    // Output:
    //   * MotionBlurColor (RGBA16F)
    //
    // Passthrough semantics: when disabled, the pass no-ops and GetTarget()
    // returns the input framebuffer.
    class MotionBlurRenderPass : public RenderGraphNode
    {
      public:
        MotionBlurRenderPass();
        ~MotionBlurRenderPass() override = default;

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

        void SetMotionBlurUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_MotionBlurUBO = ubo;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept
        {
            return m_MotionBlurShader && m_MotionBlurShader->IsReady() && m_MotionBlurUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

      private:
        bool m_Enabled = false;

        Ref<Shader> m_MotionBlurShader;
        Ref<UniformBuffer> m_MotionBlurUBO;
        RGTextureHandle m_SelectedSceneDepthTexture{};
    };
} // namespace OloEngine
