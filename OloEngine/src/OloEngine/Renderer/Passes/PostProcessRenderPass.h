#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Render pass that applies a chain of post-processing effects.
    //
    // This pass sits between the ParticlePass (scene rendering) and FinalPass (screen blit).
    // It uses two ping-pong RGBA16F framebuffers to chain effects, then resolves
    // the final color into a stable output framebuffer for downstream passes.
    class PostProcessRenderPass : public RenderPass
    {
      public:
        PostProcessRenderPass();
        ~PostProcessRenderPass() override = default;

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

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }
        void SetPostProcessUBO(Ref<UniformBuffer> ubo, PostProcessUBOData* gpuData)
        {
            m_PostProcessUBO = ubo;
            m_GPUData = gpuData;
        }
        void SetFogEnabled(bool enabled)
        {
            m_FogEnabled = enabled;
        }
        // Phase F slice 16 — when true, the inline FXAA stage is skipped
        // because a standalone `FXAARenderPass` runs after this pass.
        void SetFXAAHandledExternally(bool externally) noexcept
        {
            m_FXAAHandledExternally = externally;
        }
        // Phase F slice 17 — when true the four late-chain inline effects
        // are each skipped because their standalone passes run after this one.
        void SetChromAbHandledExternally(bool externally) noexcept
        {
            m_ChromAbHandledExternally = externally;
        }
        void SetColorGradingHandledExternally(bool externally) noexcept
        {
            m_ColorGradingHandledExternally = externally;
        }
        void SetToneMapHandledExternally(bool externally) noexcept
        {
            m_ToneMapHandledExternally = externally;
        }
        void SetVignetteHandledExternally(bool externally) noexcept
        {
            m_VignetteHandledExternally = externally;
        }
        // Phase F slice 20 — when true the inline precipitation section is
        // skipped because the standalone `PrecipitationRenderPass` runs after.
        void SetPrecipitationHandledExternally(bool externally) noexcept
        {
            m_PrecipitationHandledExternally = externally;
        }
        // Phase F slice 19 — when true the inline TAA section is skipped
        // because the standalone `TAARenderPass` runs after this pass.
        void SetTAAHandledExternally(bool externally) noexcept
        {
            m_TAAHandledExternally = externally;
        }
        // Phase F slice 21 — when true the inline motion blur section is
        // skipped because the standalone `MotionBlurRenderPass` runs after.
        void SetMotionBlurHandledExternally(bool externally) noexcept
        {
            m_MotionBlurHandledExternally = externally;
        }
        // Phase F slice 23 — when true the inline Bloom section is skipped
        // because the standalone `BloomRenderPass` runs after this pass.
        void SetBloomHandledExternally(bool externally) noexcept
        {
            m_BloomHandledExternally = externally;
        }
        // Phase F slice 24 — when true the inline AO Apply section is skipped
        // because the standalone `AOApplyRenderPass` runs before this pass.
        void SetAOApplyHandledExternally(bool externally) noexcept
        {
            m_AOApplyHandledExternally = externally;
        }
        // Phase F slice 22 — when true the inline DOF section is skipped
        // because the standalone `DOFRenderPass` runs after this pass.
        void SetDOFHandledExternally(bool externally) noexcept
        {
            m_DOFHandledExternally = externally;
        }
        // Phase F slice 18 — when true the inline fog section is skipped
        // because the standalone `FogRenderPass` runs after this pass.
        void SetFogHandledExternally(bool externally) noexcept
        {
            m_FogHandledExternally = externally;
        }
        void SetPrecipitationScreenEffectsEnabled(bool enabled)
        {
            m_PrecipitationScreenEffectsEnabled = enabled;
        }
        // Hot-reload a post-process shader by name (stem, e.g. "PostProcess_BloomThreshold")
        void ReloadShader(const std::string& name);

        // -------------------------------------------------------------------
        // Phase B — history accessors for FrameBlackboard population
        // -------------------------------------------------------------------
        // Returns the GL texture ID of the previous TAA accumulation result
        // (the buffer that will be sampled as "history" in the current frame).
        // Returns 0 if no valid history has been produced yet (first frame /
        // after a resize / after OnReset).
        [[nodiscard]] u32 GetTAAHistoryTextureID() const
        {
            if (!m_TAAHistoryValid || !m_TAAHistoryFB)
                return 0;
            return m_TAAHistoryFB->GetColorAttachmentRendererID(0);
        }

        // Returns the GL texture ID of the previous fog integration result.
        // Returns 0 when no fog history is available.
        [[nodiscard]] u32 GetFogHistoryTextureID() const
        {
            if (!m_FogHistoryFB)
                return 0;
            return m_FogHistoryFB->GetColorAttachmentRendererID(0);
        }

        // Phase F slice 25 — returns true when every post-process effect has been
        // extracted into its own standalone pass.  When true, PostProcessRenderPass
        // is a transparent (zero-cost) node: Execute() returns immediately and
        // GetTarget() / SetupFrameBlackboard alias PostProcessColor to the upstream
        // source without any GPU blit.
        [[nodiscard]] bool IsAllHandledExternally() const noexcept
        {
            return m_FXAAHandledExternally && m_ChromAbHandledExternally &&
                   m_ColorGradingHandledExternally && m_ToneMapHandledExternally &&
                   m_VignetteHandledExternally && m_PrecipitationHandledExternally &&
                   m_TAAHandledExternally && m_MotionBlurHandledExternally &&
                   m_DOFHandledExternally && m_BloomHandledExternally &&
                   m_FogHandledExternally && m_AOApplyHandledExternally;
        }

      private:
        void CreatePingPongFramebuffers(u32 width, u32 height);
        void CreateBloomMipChain(u32 width, u32 height);
        void ExecuteBloom(Ref<Framebuffer> sceneColorFB);
        void ResolveToOutput(const Ref<Framebuffer>& sourceFB);
        void DrawFullscreenTriangle();
        std::vector<Ref<Shader>*> GetAllShaderRefs();

        // Runs a single fullscreen shader pass: binds srcFB color 0 as texture, renders to dstFB
        void RunEffect(const Ref<Shader>& shader, Ref<Framebuffer> srcFB, Ref<Framebuffer> dstFB);

        Ref<Framebuffer> m_PingFB;
        Ref<Framebuffer> m_PongFB;
        Ref<Framebuffer> m_OutputFB;

        // Shaders for each effect (loaded once in Init)
        Ref<Shader> m_BloomThresholdShader;
        Ref<Shader> m_BloomDownsampleShader;
        Ref<Shader> m_BloomUpsampleShader;
        Ref<Shader> m_BloomCompositeShader;
        Ref<Shader> m_VignetteShader;
        Ref<Shader> m_ChromaticAberrationShader;
        Ref<Shader> m_ColorGradingShader;
        Ref<Shader> m_ToneMapShader;
        Ref<Shader> m_FXAAShader;
        Ref<Shader> m_DOFShader;
        Ref<Shader> m_MotionBlurShader;
        Ref<Shader> m_TAAShader;
        Ref<Shader> m_FogShader;
        Ref<Shader> m_SSAOApplyShader;
        Ref<Shader> m_PrecipitationShader;

        PostProcessSettings m_Settings;

        // UBO reference for per-pass updates (texel size, camera near/far)
        Ref<UniformBuffer> m_PostProcessUBO;
        PostProcessUBOData* m_GPUData = nullptr;
        bool m_FogEnabled = false;
        bool m_PrecipitationScreenEffectsEnabled = false;
        // Phase F slice 16 — when true, the inline FXAA stage is skipped
        // because a standalone `FXAARenderPass` runs after this pass.
        bool m_FXAAHandledExternally = false;
        // Phase F slice 17 — per-effect "handled externally" flags for
        // the four extracted late-chain passes.
        bool m_ChromAbHandledExternally = false;
        bool m_ColorGradingHandledExternally = false;
        bool m_ToneMapHandledExternally = false;
        bool m_VignetteHandledExternally = false;
        // Phase F slice 20 — when true the inline precipitation section is
        // skipped because the standalone `PrecipitationRenderPass` runs after.
        bool m_PrecipitationHandledExternally = false;
        // Phase F slice 19 — when true the inline TAA section is skipped
        // because the standalone `TAARenderPass` runs after this pass.
        bool m_TAAHandledExternally = false;
        // Phase F slice 21 — when true the inline motion blur section is
        // skipped because the standalone `MotionBlurRenderPass` runs after.
        bool m_MotionBlurHandledExternally = false;
        // Phase F slice 23 — when true the inline Bloom section is skipped
        // because the standalone `BloomRenderPass` runs after this pass.
        bool m_BloomHandledExternally = false;
        // Phase F slice 24 — when true the inline AO Apply section is skipped
        // because the standalone `AOApplyRenderPass` runs before this pass.
        bool m_AOApplyHandledExternally = false;
        // Phase F slice 22 — when true the inline DOF section is skipped
        // because the standalone `DOFRenderPass` runs after this pass.
        bool m_DOFHandledExternally = false;
        // Phase F slice 18 — when true the inline fog section is skipped
        // because the standalone `FogRenderPass` runs after this pass.
        bool m_FogHandledExternally = false;
        Ref<UniformBuffer> m_PrecipitationScreenUBO;

        // Half-resolution volumetric fog framebuffers
        Ref<Framebuffer> m_FogHalfResFB; // RGBA16F: RGB = inscatter, A = transmittance
        Ref<Framebuffer> m_FogHistoryFB; // Temporal reprojection history
        u32 m_FogHalfWidth = 0;
        u32 m_FogHalfHeight = 0;
        Ref<Shader> m_FogUpsampleShader; // Bilateral upsample + composite

        // Bloom mip chain framebuffers (RGBA16F, progressively smaller)
        static constexpr u32 MAX_BLOOM_MIPS = 5;
        std::vector<Ref<Framebuffer>> m_BloomMipChain;

        // TAA history framebuffer (persists across frames). RGBA16F full-res.
        // After each TAA resolve we blit the resolved colour into this FB so
        // the next frame's TAA pass can sample it as "previous".
        Ref<Framebuffer> m_TAAHistoryFB;
        bool m_TAAHistoryValid = false; // false on first frame / after resize
        Ref<UniformBuffer> m_TAAUBO;    // binding 32 (TAAParams)
    };
} // namespace OloEngine
