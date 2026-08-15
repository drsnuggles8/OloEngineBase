#pragma once

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Colour-vision deficiency adaptation pass (issue #458).
    //
    // Placement: AFTER UICompositePass, immediately before FinalPass —
    //
    //   … → Vignette → FXAA → (Overdraw) → UIComposite → ColorBlind → Final
    //
    // The stage is deliberately NOT slotted next to Vignette. An accessibility
    // remap that only reaches the world leaves the HUD unadapted, and the HUD is
    // where colour is most often the ONLY channel carrying meaning (a red-vs-green
    // health bar, a team colour, a minimap ping). Running last also means only
    // one downstream consumer's candidate list has to learn the new resource
    // (FinalRenderPass) instead of the five that a mid-chain slot would touch —
    // see docs/agent-rules/glsl-shaders.md §9.
    //
    // Operates on the display-referred LDR image; the shader decodes gamma,
    // adapts in linear light, and re-encodes. Passthrough (the pass self-skips
    // and its resource is never declared) when the mode is None.
    //
    // NOT quality-tiering gated, on purpose: a player who needs this needs it at
    // every quality level. QualityTiering only rewrites PostProcessSettings, and
    // the colour-blind mode deliberately lives outside that struct.
    class ColorBlindRenderPass : public RenderGraphNode
    {
      public:
        ColorBlindRenderPass();
        ~ColorBlindRenderPass() override = default;

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

        // The parameters uploaded to UBO_COLORBLIND on the next Execute. Set from
        // the process-global accessibility settings in
        // RenderPipeline::ConfigurePassesForFrame, so the render thread reads a
        // snapshot rather than the live global mid-frame.
        void SetParams(const ColorBlindUBOData& params) noexcept
        {
            m_Params = params;
        }
        [[nodiscard]] const ColorBlindUBOData& GetParams() const noexcept
        {
            return m_Params;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_Shader && m_Shader->IsReady() && m_ParamsUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_ParamsUBO;
        ColorBlindUBOData m_Params;

        bool m_Enabled = false;
    };
} // namespace OloEngine
