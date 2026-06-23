#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Screen-Space Contact Shadows (SSCS) post-process pass.
    //
    // Deferred-only stage inserted into the dynamic chain between SSR and Bloom:
    //   SSRColor/SSGIColor/AOApplyColor/SSSColor/SceneColor → ContactShadow →
    //   ContactShadowColor → Bloom → ...
    //
    // The pass reads the lit scene colour plus the deferred G-Buffer (world
    // normal in RT1) and scene depth, then for each lit opaque pixel marches a
    // single short ray TOWARD the primary directional light against scene depth.
    // If a nearby on-screen occluder crosses the ray within a thin thickness
    // window the pixel is darkened — grounding dynamic geometry the coarse shadow
    // map misses where it touches a surface. The shadow factor MULTIPLIES the lit
    // colour (occlusion of direct light, unlike SSGI's add or SSR's replace/mix)
    // into a fresh ContactShadowColor target (so the read/write of scene colour
    // never aliases).
    //
    // Inputs:
    //   * Input framebuffer handle (latest upstream colour), selected during
    //     `Setup()` via the versioned name fallback.
    //   * Scene depth texture (for view-space position reconstruction + marching)
    //   * G-Buffer RT1 normal+roughness texture (N·L cull + start bias)
    //   * ContactShadowUBO (binding 41), uploaded each frame by Renderer3D.
    //
    // Output:
    //   * ContactShadowColor (RGBA16F) — contact-shadow-composited scene colour.
    //
    // Disabled / forward-path semantics: when the pass is disabled or the
    // G-Buffer is unavailable (forward / forward+), the graph omits
    // ContactShadowColor so downstream stages alias back to the upstream scene
    // colour. There is no runtime passthrough blit.
    class ContactShadowRenderPass : public RenderGraphNode
    {
      public:
        ContactShadowRenderPass();
        ~ContactShadowRenderPass() override = default;

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
            // The UBO carries the camera matrices + light dir + ray params the
            // shader needs; executing without it would march against stale state.
            return m_ContactShadowShader && m_ContactShadowShader->IsReady() && m_ContactShadowUBO;
        }

        void SetContactShadowUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_ContactShadowUBO = ubo;
        }

      private:
        bool m_Enabled = false;

        Ref<Shader> m_ContactShadowShader;
        Ref<UniformBuffer> m_ContactShadowUBO;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
    };

} // namespace OloEngine
