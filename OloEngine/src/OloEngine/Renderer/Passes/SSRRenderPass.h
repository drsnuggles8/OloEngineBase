#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Screen-Space Reflections (SSR) post-process pass.
    //
    // Deferred-only stage inserted into the dynamic chain between AO apply and
    // bloom:
    //   AOApplyColor/SSSColor/SceneColor → SSR → SSRColor → Bloom → ...
    //
    // The pass reads the lit scene color plus the deferred G-Buffer (world
    // normal + roughness in RT1, metallic in RT0.a) and scene depth, then
    // ray-marches each opaque pixel's view-space reflection vector against the
    // depth buffer (linear march + binary-search refinement). On a hit it
    // samples the upstream scene color and composites it additively, weighted by
    // Fresnel, roughness fade, and screen-edge / distance / facing fades, into a
    // fresh SSRColor target (so the read/write of scene color never aliases).
    //
    // Inputs:
    //   * Input framebuffer handle (AOApplyColor / SSSColor / SceneColor),
    //     selected during `Setup()` via the versioned name fallback.
    //   * Scene depth texture (for view-space position reconstruction + marching)
    //   * G-Buffer RT1 normal+roughness and RT0 albedo+metallic textures
    //   * SSRUBO (binding 38), uploaded each frame by Renderer3D.
    //
    // Output:
    //   * SSRColor (RGBA16F) — reflection-composited scene color.
    //
    // Disabled / forward-path semantics: when the pass is disabled or the
    // G-Buffer is unavailable (forward / forward+), the graph omits SSRColor so
    // downstream stages alias back to the upstream scene color. There is no
    // runtime passthrough blit.
    class SSRRenderPass : public RenderGraphNode
    {
      public:
        SSRRenderPass();
        ~SSRRenderPass() override = default;

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
            return m_SSRShader && m_SSRShader->IsReady();
        }

        void SetSSRUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_SSRUBO = ubo;
        }

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSRShader;
        Ref<UniformBuffer> m_SSRUBO;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
        RGTextureHandle m_SelectedGBufferAlbedoTexture{};
    };

} // namespace OloEngine
