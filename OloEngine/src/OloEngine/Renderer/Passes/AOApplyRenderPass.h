#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone AO Apply post-process pass.
    //
    // Standalone AO stage inserted into the dynamic chain:
    //   SSSColor/SceneColor → AOApply → AOApplyColor → Bloom → ...
    //
    // Inputs:
    //   * Input framebuffer handle (SSSColor or SceneColor), selected during `Setup()`
    //   * AO texture handle (from SSAORenderPass or GTAORenderPass), selected during `Setup()`
    //   * Scene depth texture handle (for bilateral AO upsampling), selected during `Setup()`
    //   * PostProcessUBO (binding 7), uploaded by Renderer3D
    //
    // Output:
    //   * AOApplyColor (RGBA16F) — AO-modulated scene color written through
    //     the setup-selected graph-owned framebuffer target.
    //
    // Disabled semantics: when disabled, the pass no-ops and GetTarget()
    // returns the input framebuffer. When AO apply is not executable for the
    // current frame (for example no imported AO buffer or the shader is not
    // ready yet), the graph simply omits AOApplyColor so downstream stages
    // alias back to the upstream scene color instead of relying on a runtime
    // passthrough blit.
    class AOApplyRenderPass : public RenderGraphNode
    {
      public:
        AOApplyRenderPass();
        ~AOApplyRenderPass() override = default;

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
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_SSAOApplyShader && m_SSAOApplyShader->IsReady();
        }

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        void SetSSAOUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_SSAOUBO = ubo;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSAOApplyShader;
        Ref<UniformBuffer> m_PostProcessUBO;
        Ref<UniformBuffer> m_SSAOUBO;
        RGTextureHandle m_SelectedAOTexture{};
        RGTextureHandle m_SelectedSceneDepthTexture{};
    };

} // namespace OloEngine
