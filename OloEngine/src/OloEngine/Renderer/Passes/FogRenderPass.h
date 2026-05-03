#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone volumetric fog post-process pass.
    //
    // Phase F slice 18 — extracted from the monolithic PostProcessRenderPass
    // following the pattern established by FXAARenderPass (slice 16) and the
    // four effects extracted in slice 17.
    //
    // Now sits at the tail of the effects that remain in PostProcessRenderPass,
    // immediately before the ChromaticAberrationRenderPass in the sub-chain:
    //   PostProcess(AO+Bloom+DOF+MB+TAA+Precip) → Fog →
    //     ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Two-pass implementation:
    //   Pass A — half-resolution ray-march with temporal reprojection.
    //            Outputs RGB = accumulated inscatter, A = transmittance.
    //   Pass B — bilateral upsample + composite onto the full-resolution
    //            scene colour input.
    //
    // The pass owns its half-resolution ray-march framebuffer and the
    // persistent temporal-history framebuffer (ping-pong swap each frame).
    // Both are allocated in Init and recreated on resize.
    //
    // Required bindings (provided via setters):
    //   * `PostProcessUBO`       (UBO binding 7) — re-bound at Execute start.
    //   * Scene depth texture ID — bound at TEX_POSTPROCESS_DEPTH (19).
    //   * Shadow CSM texture ID  — bound at TEX_SHADOW (8), optional.
    //
    // Passthrough semantics: when `Enabled` is false the pass no-ops and
    // `GetTarget()` returns the input framebuffer. Renderer3D skips importing
    // `FogColor` into the blackboard when fog is disabled, so graph consumers
    // fall back to `PostProcessColor` automatically.
    class FogRenderPass : public RenderPass
    {
      public:
        FogRenderPass();
        ~FogRenderPass() override = default;

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

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        // Phase F slice 42 — SetSceneDepthTextureHandle, SetShadowMapCSMHandle removed;
        // Execute() self-resolves from blackboard. Raw ID setters retained for
        // headless / test fallback paths.
        void SetSceneDepthTextureID(u32 id) noexcept
        {
            m_SceneDepthTextureID = id;
        }
        void SetShadowCSMTextureID(u32 id) noexcept
        {
            m_ShadowCSMTextureID = id;
        }

        // Returns the GL texture ID of the current frame's fog integration
        // result (held in m_FogHistoryFB after the per-frame history swap).
        // Used by Renderer3D::SetupFrameBlackboard to import `FogHistory`
        // into the blackboard so the fog shader can read it next frame.
        // Returns 0 when unavailable.
        [[nodiscard]] u32 GetFogHistoryTextureID() const;

      private:
        void CreateFramebuffers(u32 width, u32 height);

        bool m_Enabled = false;

        // Full-resolution RGBA16F composited output (scene + fog).
        Ref<Framebuffer> m_OutputFB;

        // Half-resolution framebuffers used by the two-pass fog algorithm.
        Ref<Framebuffer> m_FogHalfResFB; // ray-march output (overwritten each frame)
        Ref<Framebuffer> m_FogHistoryFB; // temporal history for next-frame reprojection
        u32 m_FogHalfWidth  = 0;
        u32 m_FogHalfHeight = 0;

        Ref<Shader> m_FogShader;         // Pass A: ray-march shader
        Ref<Shader> m_FogUpsampleShader; // Pass B: bilateral upsample + composite

        Ref<UniformBuffer> m_PostProcessUBO;

        u32 m_SceneDepthTextureID = 0;
        u32 m_ShadowCSMTextureID = 0;

        FramebufferSpecification m_FramebufferSpec;
    };
} // namespace OloEngine
