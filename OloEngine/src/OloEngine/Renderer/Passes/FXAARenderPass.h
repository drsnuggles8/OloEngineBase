#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone FXAA post-process pass.
    //
    // Phase F slice 16 — first effect extracted from the monolithic
    // PostProcessRenderPass into its own graph-native pass. Establishes
    // the pattern for the remaining post-process effect splits.
    //
    // Inputs:
    //   * `PostProcessColor` framebuffer (typed handle) — LDR result of
    //     the post-process chain after tone mapping. Bound as `u_Texture`
    //     at sampler slot 0.
    //   * `PostProcessUBO` (binding 7) — shared with PostProcessRenderPass
    //     for `u_TexelSize` / `u_PixelSize` / gamma uniforms. The buffer
    //     is uploaded by Renderer3D before the post-process chain runs;
    //     this pass only re-binds it to defend against rebinds elsewhere.
    //
    // Outputs:
    //   * `FXAAColor` framebuffer (RGBA8, single colour attachment) —
    //     anti-aliased LDR result. Consumers (SelectionOutline,
    //     UICompositePass) read this when valid, otherwise fall back to
    //     `PostProcessColor`.
    //
    // Passthrough semantics: when `Enabled` is false the pass no-ops and
    // `GetTarget()` returns the input framebuffer so downstream stages
    // see the unmodified post-process output. Renderer3D additionally
    // skips importing `FXAAColor` into the blackboard when disabled, so
    // graph consumers naturally fall back to `PostProcessColor` without
    // any per-frame re-registration.
    class FXAARenderPass : public RenderPass
    {
      public:
        FXAARenderPass();
        ~FXAARenderPass() override = default;

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

        // Shared with PostProcessRenderPass — Renderer3D uploads the data
        // once per frame before the post-process chain executes; this pass
        // only re-binds the UBO at slot 7 so the FXAA shader reads the
        // expected texel-size and gamma values.
        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Framebuffer> m_OutputFB;
        Ref<Shader> m_FXAAShader;
        Ref<UniformBuffer> m_PostProcessUBO;

        bool m_Enabled = false;
    };
} // namespace OloEngine
