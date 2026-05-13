#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone temporal anti-aliasing post-process pass.
    //
    // Phase F slice 19 — standalone TAA stage in the dynamic chain:
    //   AOApply/Bloom/DOF/MotionBlur/Precipitation → TAA → Fog → ...
    //
    // Uses:
    //   * Renderer-owned persistent RGBA16F history texture imported next
    //     frame as `TAAHistory`
    //   * TAA parameters UBO at binding 32
    //
    // Writes the current-frame `TAAColor` output through the setup-selected
    // graph-owned framebuffer target.
    //
    // Inputs (selected during `Setup()`):
    //   * Post-process colour input framebuffer
    //   * Scene depth texture (required)
    //   * Velocity texture (optional, 0 = camera-only reprojection path)
    //   * MotionBlur UBO at binding 8 is uploaded globally by Renderer3D
    //
    // Passthrough semantics: when disabled the pass no-ops and GetTarget()
    // returns the input framebuffer so downstream stages naturally fall back
    // to PostProcessColor.
    class TAARenderPass : public RenderGraphNode
    {
      public:
        TAARenderPass();
        ~TAARenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
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
            return m_TAAShader && m_TAAShader->IsReady() && m_TAAUBO;
        }

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }

      private:
        void CreateFramebuffers(u32 width, u32 height);

      private:
        bool m_Enabled = false;
        PostProcessSettings m_Settings;

        Ref<Shader> m_TAAShader;
        Ref<UniformBuffer> m_TAAUBO;
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedVelocityTexture{};
        RGTextureHandle m_SelectedHistoryTexture{};
    };
} // namespace OloEngine
