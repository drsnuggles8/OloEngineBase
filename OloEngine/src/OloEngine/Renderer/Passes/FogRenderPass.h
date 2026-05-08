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
    // Phase F slice 18 — standalone fog stage in the dynamic post chain
    // following the pattern established by FXAARenderPass (slice 16) and the
    // four effects extracted in slice 17.
    //
    // Sits before the ChromaticAberrationRenderPass in the sub-chain:
    //   PostProcess(AO+Bloom+DOF+MB+TAA+Precip) → Fog →
    //     ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Two-pass implementation:
    //   Pass A — half-resolution ray-march with temporal reprojection.
    //            Outputs RGB = accumulated inscatter, A = transmittance.
    //   Pass B — bilateral upsample + composite onto the full-resolution
    //            scene colour input.
    //
    // The pass owns only the persistent temporal-history storage imported next
    // frame as `FogHistory`. The current-frame full-resolution composite
    // (`FogColor`) and half-resolution integration scratch (`FogHalfRes`) are
    // graph-owned framebuffers resolved from the frame blackboard.
    //
    // Required bindings:
    //   * `PostProcessUBO`       (UBO binding 7) — re-bound at Execute start.
    //   * Scene depth texture ID — resolved from the blackboard and bound at TEX_POSTPROCESS_DEPTH (19).
    //   * Shadow CSM texture ID  — resolved from the blackboard and bound at TEX_SHADOW (8), optional.
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

        // Returns the GL texture ID of the persistent fog history texture.
        // Used by RenderPipeline::PopulateBlackboard(...) to import
        // `FogHistory` into the blackboard so the fog shader can read it
        // next frame.
        // Returns 0 until a valid history frame has been written back.
        [[nodiscard]] u32 GetFogHistoryTextureID() const;

      private:
        void CreateFramebuffers(u32 width, u32 height);
        void StoreHistoryTexture(u32 textureID);

        bool m_Enabled = false;

        Ref<Framebuffer> m_FogHistoryFB; // persistent temporal history for next-frame reprojection
        u32 m_FogHalfWidth = 0;
        u32 m_FogHalfHeight = 0;

        Ref<Shader> m_FogShader;         // Pass A: ray-march shader
        Ref<Shader> m_FogUpsampleShader; // Pass B: bilateral upsample + composite

        Ref<UniformBuffer> m_PostProcessUBO;
        bool m_FogHistoryValid = false;
    };
} // namespace OloEngine
