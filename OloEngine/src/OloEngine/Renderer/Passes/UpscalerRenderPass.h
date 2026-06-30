#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone spatial-upscaler / sharpening post-process pass (#432).
    //
    // First slice ships Contrast Adaptive Sharpening (CAS, FidelityFX): an
    // adaptive unsharp mask applied to the tonemapped (LDR) image. It runs
    // AFTER ToneMap and BEFORE Vignette in the dynamic chain — CAS's contrast
    // term and final saturate assume the [0,1] display range, so sharpening in
    // unbounded HDR (pre-tonemap) would zero out on bright pixels. The full FSR1
    // EASU/RCAS spatial upscale (render below display res, then upscale) is a
    // deferred follow-up that will reuse this pass scaffold.
    //
    // Inputs (selected during Setup()):
    //   * Post-process colour input framebuffer (ToneMapColor or the freshest
    //     upstream LDR colour)
    //   * CASParams UBO (binding 44), uploaded by this pass each frame from the
    //     PostProcessSettings sharpness.
    //
    // Output:
    //   * UpscalerColor (RGBA16Float, carrying LDR values)
    //
    // Passthrough semantics: when disabled the pass no-ops, declares no
    // UpscalerColor output, and leaves m_Target null (so GetTarget() returns
    // null). Downstream stages then resolve their input by name and naturally
    // fall back past the absent UpscalerColor to ToneMapColor.
    class UpscalerRenderPass : public RenderGraphNode
    {
      public:
        UpscalerRenderPass();
        ~UpscalerRenderPass() override = default;

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

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_CASShader && m_CASShader->IsReady() && m_CASUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

      private:
        bool m_Enabled = false;
        PostProcessSettings m_Settings;

        Ref<Shader> m_CASShader;
        Ref<UniformBuffer> m_CASUBO;
    };
} // namespace OloEngine
