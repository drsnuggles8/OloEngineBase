#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone colour-grading post-process pass.
    //
    // Phase F slice 17 — standalone effect in the dynamic post chain
    // following the pattern established by FXAARenderPass (slice 16).
    //
    // Sits second in the extracted-effect sub-chain:
    //   PostProcess → ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Operates in HDR space. Passthrough when `Enabled` is false.
    class ColorGradingRenderPass : public RenderGraphNode
    {
      public:
        ColorGradingRenderPass();
        ~ColorGradingRenderPass() override;

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

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_Shader && m_Shader->IsReady() && m_PostProcessUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);
        // Builds a 16x16x16 identity LUT laid out as a 256x16 horizontal strip.
        // Required so the pass is a pass-through when no user LUT is set —
        // without it, sampler unit 18 is unbound and the shader emits black.
        void CreateIdentityLUT();

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_PostProcessUBO;

        u32 m_IdentityLUTTexture = 0;

        bool m_Enabled = false;
    };
} // namespace OloEngine
