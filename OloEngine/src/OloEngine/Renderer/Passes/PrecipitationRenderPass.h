#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone screen-space precipitation post-process pass.
    //
    // Phase F slice 20 — standalone precipitation stage in the dynamic chain
    // following the same pattern established for FogRenderPass (slice 18) and
    // TAARenderPass (slice 19).
    //
    // Sits between TAA and Fog in the sub-chain:
    //   PostProcess(AO+Bloom+DOF+MotionBlur) → TAA → Precipitation → Fog →
    //     ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Applies directional snow/rain streak overlays and lens snowflake/raindrop
    // impacts onto the stable (TAA-resolved) scene colour.  CPU-side state is
    // managed by `ScreenSpacePrecipitation`; this pass only performs the GPU
    // draw call.
    //
    // Inputs (provided via setters):
    //   * Input colour framebuffer (TAAColor, or PostProcessColor as fallback)
    //   * Precipitation UBO at binding 19 (`UBO_PRECIPITATION_SCREEN`) is
    //     populated from ScreenSpacePrecipitation statics inside Execute.
    //
    // Passthrough semantics: when disabled the pass no-ops and GetTarget()
    // returns the input framebuffer so downstream stages fall back naturally.
    class PrecipitationRenderPass : public RenderPass
    {
      public:
        PrecipitationRenderPass();
        ~PrecipitationRenderPass() override = default;

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

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Framebuffer> m_OutputFB;

        Ref<Shader> m_PrecipitationShader;
        Ref<UniformBuffer> m_PrecipitationScreenUBO;

        FramebufferSpecification m_FramebufferSpec;
        bool m_Enabled = false;
    };
} // namespace OloEngine
