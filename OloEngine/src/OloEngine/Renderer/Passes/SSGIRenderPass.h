#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Screen-Space Global Illumination (SSGI) post-process pass.
    //
    // Deferred-only stage inserted into the dynamic chain between AO apply and
    // SSR:
    //   AOApplyColor/SSSColor/SceneColor → SSGI → SSGIColor → SSR → Bloom → ...
    //
    // The pass reads the lit scene colour plus the deferred G-Buffer (world
    // normal in RT1, albedo in RT0) and scene depth, then casts a
    // cosine-weighted hemisphere of short rays around each opaque pixel's view
    // normal and linear-marches them against scene depth. On a hit it samples the
    // upstream lit colour as incoming indirect radiance; the hemisphere mean,
    // tinted by the receiver albedo, is the one-bounce indirect diffuse. It is
    // ADDED to the lit colour (indirect diffuse is extra bounced light, not a
    // mirror substitution like SSR), scaled by the SSGI intensity, into a fresh
    // SSGIColor target (so the read/write of scene colour never aliases).
    //
    // Inputs:
    //   * Input framebuffer handle (AOApplyColor / SSSColor / SceneColor),
    //     selected during `Setup()` via the versioned name fallback.
    //   * Scene depth texture (for view-space position reconstruction + marching)
    //   * G-Buffer RT1 normal+roughness and RT0 albedo+metallic textures
    //   * SSGIUBO (binding 40), uploaded each frame by Renderer3D.
    //
    // Output:
    //   * SSGIColor (RGBA16F) — indirect-diffuse-composited scene colour.
    //
    // Disabled / forward-path semantics: when the pass is disabled or the
    // G-Buffer is unavailable (forward / forward+), the graph omits SSGIColor so
    // downstream stages alias back to the upstream scene colour. There is no
    // runtime passthrough blit.
    class SSGIRenderPass : public RenderGraphNode
    {
      public:
        SSGIRenderPass();
        ~SSGIRenderPass() override = default;

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
            // The UBO carries the camera matrices + ray params the shader needs;
            // executing without it would gather against stale/garbage state.
            return m_SSGIShader && m_SSGIShader->IsReady() && m_SSGIUBO;
        }

        void SetSSGIUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_SSGIUBO = ubo;
        }

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSGIShader;
        Ref<UniformBuffer> m_SSGIUBO;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
        RGTextureHandle m_SelectedGBufferAlbedoTexture{};
    };

} // namespace OloEngine
