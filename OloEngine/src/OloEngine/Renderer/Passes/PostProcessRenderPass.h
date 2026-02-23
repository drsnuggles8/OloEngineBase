#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    // @brief Render pass that applies a chain of post-processing effects.
    //
    // This pass sits between the ParticlePass (scene rendering) and FinalPass (screen blit).
    // It uses two ping-pong RGBA16F framebuffers to chain effects. When no effects are
    // enabled it acts as a passthrough (GetTarget returns the input framebuffer).
    class PostProcessRenderPass : public RenderPass
    {
      public:
        PostProcessRenderPass();
        ~PostProcessRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetInputFramebuffer(const Ref<Framebuffer>& input) override;
        void SetSettings(const PostProcessSettings& settings) { m_Settings = settings; }
        void SetSceneDepthFramebuffer(const Ref<Framebuffer>& sceneFB) { m_SceneDepthFB = sceneFB; }
        void SetPostProcessUBO(Ref<UniformBuffer> ubo, PostProcessUBOData* gpuData) { m_PostProcessUBO = ubo; m_GPUData = gpuData; }

      private:
        void CreatePingPongFramebuffers(u32 width, u32 height);
        void CreateBloomMipChain(u32 width, u32 height);
        void ExecuteBloom(Ref<Framebuffer> sceneColorFB);
        void DrawFullscreenTriangle();

        // Runs a single fullscreen shader pass: binds srcFB color 0 as texture, renders to dstFB
        void RunEffect(const Ref<Shader>& shader, Ref<Framebuffer> srcFB, Ref<Framebuffer> dstFB);

        Ref<Framebuffer> m_InputFramebuffer;
        Ref<Framebuffer> m_PingFB;
        Ref<Framebuffer> m_PongFB;
        Ref<Framebuffer> m_SceneDepthFB; // For DOF / Motion Blur depth access

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

        Ref<VertexArray> m_FullscreenTriangleVA;

        PostProcessSettings m_Settings;

        // UBO reference for per-pass updates (texel size, camera near/far)
        Ref<UniformBuffer> m_PostProcessUBO;
        PostProcessUBOData* m_GPUData = nullptr;

        // Bloom mip chain framebuffers (RGBA16F, progressively smaller)
        static constexpr u32 MAX_BLOOM_MIPS = 5;
        std::vector<Ref<Framebuffer>> m_BloomMipChain;

        // Tracks which ping-pong buffer was last written to
        bool m_LastWrittenIsPing = true;
        // Tracks if Execute() skipped all processing (passthrough to input FB)
        bool m_SkippedThisFrame = false;
    };
} // namespace OloEngine
