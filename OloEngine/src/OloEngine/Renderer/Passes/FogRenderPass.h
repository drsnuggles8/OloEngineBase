#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
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
    // The pass uses renderer-owned persistent temporal-history storage imported
    // next frame as `FogHistory`. The current-frame full-resolution composite
    // (`FogColor`) and half-resolution integration scratch (`FogHalfRes`) are
    // graph-owned framebuffers selected during `Setup()` and resolved later in
    // `Execute()`.
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
    class FogRenderPass : public RenderGraphNode
    {
      public:
        FogRenderPass();
        ~FogRenderPass() override = default;

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
            return m_FogShader && m_FogShader->IsReady() &&
                   m_FogUpsampleShader && m_FogUpsampleShader->IsReady() &&
                   m_PostProcessUBO;
        }

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        // The full 272-byte shared camera UBO (binding 0). The fog shaders read
        // u_CameraPosition (std140 offset 192) and u_Projection (offset 128) from
        // it, but an earlier stage can leave a *smaller* (64-byte, ViewProjection-
        // only) camera UBO bound at slot 0 — making those reads out-of-bounds.
        // FogRenderPass re-binds this at Execute start to guarantee the full
        // layout is present (mirrors the binding-7 PostProcess UBO re-bind).
        void SetCameraUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_CameraUBO = ubo;
        }

      private:
        void CreateFramebuffers(u32 width, u32 height);

        bool m_Enabled = false;

        u32 m_FogHalfWidth = 0;
        u32 m_FogHalfHeight = 0;

        Ref<Shader> m_FogShader;         // Pass A: ray-march shader
        Ref<Shader> m_FogUpsampleShader; // Pass B: bilateral upsample + composite

        Ref<UniformBuffer> m_PostProcessUBO;
        Ref<UniformBuffer> m_CameraUBO; // full shared camera UBO (binding 0)
        RGFramebufferHandle m_SelectedFogHalfResFramebuffer{};
        RGTextureHandle m_SelectedFogHistoryTexture{};
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedShadowCSMTexture{};
    };
} // namespace OloEngine
