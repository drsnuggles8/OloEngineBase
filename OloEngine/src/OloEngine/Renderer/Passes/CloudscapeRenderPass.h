#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Volumetric cloudscape post-process pass (issue #633).
    //
    // Sits between TAAPass and PrecipitationPass in the dynamic post chain:
    //   PostProcess(AO+Bloom+DOF+MB+TAA) → Cloudscape → Precipitation →
    //     Fog → ChromAberration → …
    // so precipitation streaks overlay the clouds and the froxel fog applies
    // aerial perspective over them for free.
    //
    // Three-draw implementation (all inside one Execute, mirroring
    // FogRenderPass's intra-pass write-then-sample discipline plus the TAA
    // history mechanics):
    //   Pass A — half-resolution raymarch (PostProcess_Cloudscape.glsl) into
    //            the graph-owned `CloudsRaw` scratch. RGB = premultiplied
    //            in-scattered radiance, A = transmittance (1 = no cloud).
    //   Pass B — half-resolution temporal resolve
    //            (PostProcess_CloudscapeResolve.glsl) into `CloudsResolved`:
    //            blends pass A with the reprojected prior-frame history
    //            (neighborhood-clamped). The graph copies the resolved buffer
    //            into the pipeline-owned `CloudsHistory` sink after execute
    //            (RegisterHistoryTextureSink / ExtractHistoryTexture — the
    //            TAA pattern).
    //   Pass C — full-resolution depth-aware upsample + composite
    //            (PostProcess_CloudscapeComposite.glsl) onto the upstream
    //            scene colour: result = scene * transmittance + inscatter.
    //
    // Required bindings (the samplers carry explicit layout(binding=N)
    // qualifiers in the shaders, so no SetInt plumbing is needed):
    //   * Shared camera UBO      (binding 0)  — re-bound at Execute start.
    //   * CloudscapeData UBO     (binding 52) — contents uploaded pre-graph by
    //     RenderPipeline::UploadExecutionState (the CloudShadow compute reads
    //     the same UBO outside the graph); re-bound here defensively.
    //   * MotionBlur UBO         (binding 8)  — inverse VP for ray
    //     reconstruction; uploaded + re-bound by UploadExecutionState (its
    //     gate includes the cloudscape enable).
    //   * Cloud noise samplers   (56/57/58)   — ids passed in via
    //     SetNoiseTextures, re-bound defensively at Execute start.
    //   * Scene depth            (binding 19) — resolved from the blackboard.
    //
    // Passthrough semantics: when `Enabled` is false the pass no-ops and
    // downstream candidate ladders skip `CloudsColor` (Renderer3D never
    // declares it while disabled), falling back to TAA/…/PostProcessColor.
    class CloudscapeRenderPass : public RenderGraphNode
    {
      public:
        CloudscapeRenderPass();
        ~CloudscapeRenderPass() override = default;

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
            return m_RaymarchShader && m_RaymarchShader->IsReady() &&
                   m_ResolveShader && m_ResolveShader->IsReady() &&
                   m_CompositeShader && m_CompositeShader->IsReady() &&
                   m_CloudscapeUBO && m_CameraUBO;
        }

        // The full 288-byte shared camera UBO (binding 0). All three cloud
        // shaders read the full CameraMatrices layout — u_CameraPosition
        // (std140 offset 192) and u_RenderOrigin (offset 272) — but an
        // earlier stage can leave a *smaller* (64-byte, ViewProjection-only)
        // camera UBO bound at slot 0, making those reads out-of-bounds.
        // CloudscapeRenderPass re-binds this at Execute start to guarantee
        // the full layout is present (mirrors FogRenderPass).
        void SetCameraUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_CameraUBO = ubo;
        }

        // Per-frame CloudscapeData snapshot, filled by
        // RenderPipeline::ConfigurePassesForFrame and uploaded (pre-graph) by
        // UploadAndBindUBO().
        void SetUBOData(const UBOStructures::CloudscapeUBO& data) noexcept
        {
            m_UBOData = data;
        }

        // Cloud noise field sampler ids (CloudNoise system; the weather map
        // may be a scene-authored override). Bound at TEX_CLOUD_BASE_NOISE /
        // TEX_CLOUD_DETAIL_NOISE / TEX_CLOUD_WEATHER_MAP (56/57/58).
        void SetNoiseTextures(u32 baseNoiseID, u32 detailNoiseID, u32 weatherMapID) noexcept
        {
            m_BaseNoiseTextureID = baseNoiseID;
            m_DetailNoiseTextureID = detailNoiseID;
            m_WeatherMapTextureID = weatherMapID;
        }

        // Pipeline-owned half-res history texture (the CloudsHistory sink
        // target) + its validity for this frame. Must be set AFTER
        // PopulateBlackboard ran (EnsureHistoryStorage may recreate the
        // texture on resize) — UploadExecutionState is the call site.
        void SetHistory(u32 historyTextureID, bool valid) noexcept
        {
            m_HistoryTextureID = historyTextureID;
            m_HistoryValid = valid;
        }

        // Upload + bind the CloudscapeData UBO (binding 52) from the last
        // SetUBOData snapshot. Called by RenderPipeline::UploadExecutionState
        // BEFORE graph execution because CloudShadow_Generate.comp consumes
        // the same UBO outside the graph. Forces the temporal-blend lane
        // (Misc.x) to 0 when no valid history exists so the resolve pass
        // outputs the current frame instead of blending against garbage.
        void UploadAndBindUBO();

        // Upload a zeroed (Misc.w = 0 → disabled) UBO so the cloud shaders
        // early-out. Used when the noise field failed to generate but the
        // pass was already declared enabled for this frame.
        void UploadDisabledUBO();

      private:
        void CreateFramebuffers(u32 width, u32 height);

        bool m_Enabled = false;

        Ref<Shader> m_RaymarchShader;  // Pass A: half-res cloud raymarch
        Ref<Shader> m_ResolveShader;   // Pass B: half-res temporal resolve
        Ref<Shader> m_CompositeShader; // Pass C: full-res upsample + composite

        Ref<UniformBuffer> m_CloudscapeUBO; // CloudscapeData (binding 52)
        Ref<UniformBuffer> m_CameraUBO;     // full shared camera UBO (binding 0)
        UBOStructures::CloudscapeUBO m_UBOData{};

        u32 m_BaseNoiseTextureID = 0;
        u32 m_DetailNoiseTextureID = 0;
        u32 m_WeatherMapTextureID = 0;
        u32 m_HistoryTextureID = 0;
        bool m_HistoryValid = false;

        RGFramebufferHandle m_SelectedCloudsRawFramebuffer{};
        RGFramebufferHandle m_SelectedCloudsResolvedFramebuffer{};
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedHistoryTexture{};
    };
} // namespace OloEngine
