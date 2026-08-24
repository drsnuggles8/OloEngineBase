#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/HZBGenerator.h"

namespace OloEngine
{
    // @brief Screen-Space Reflections (SSR) post-process pass.
    //
    // Deferred-only stage inserted into the dynamic chain between AO apply and
    // bloom:
    //   AOApplyColor/SSSColor/SceneColor → SSR → SSRColor → Bloom → ...
    //
    // THREE DRAWS IN ONE NODE (issue #902), the CloudscapeRenderPass shape:
    //   A. PostProcess_SSR.glsl          → SSRSignal   (the stochastic term
    //                                      ONLY: rgb = the reflection DELTA,
    //                                      (reflection - base) * blend, and
    //                                      a = positive view depth)
    //   B. PostProcess_SSRResolve.glsl   → SSRResolved (temporal accumulation
    //                                      against SSRHistory; the graph copies
    //                                      this into the history sink)
    //   C. PostProcess_SSRComposite.glsl → SSRColor    (upstream colour + the
    //                                      resolved delta, which reproduces the
    //                                      old mix(base, reflection, blend))
    //
    // The split is the whole point: compositing into the scene colour made the
    // output un-accumulable, because temporally blending it would smear the
    // base colour along with the noise. Draw B needs the current frame's signal
    // in a real texture (its 3x3 neighbourhood clip gathers neighbours that do
    // not exist yet inside draw A), which is why this is three draws and not
    // one shader with a history sample bolted on. It is also what pays for the
    // raised SSRMaxRoughness default.
    //
    // The pass reads the lit scene color plus the deferred G-Buffer (world
    // normal + roughness in RT1, metallic in RT0.a) and scene depth, then
    // ray-marches each opaque pixel's view-space reflection vector against the
    // depth buffer (linear march + binary-search refinement). On a hit it
    // samples the upstream scene color and composites it with a replace/mix blend
    // (lerp toward the reflection by reflectance x confidence — not additive,
    // which would double-count the IBL already in the base color), weighted by
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
    //   * SSRResolved (RGBA16F, graph scratch) — extracted into SSRHistory.
    //
    // Disabled / forward-path semantics: when the pass is disabled or the
    // G-Buffer is unavailable (forward / forward+), the graph omits SSRColor so
    // downstream stages alias back to the upstream scene color. There is no
    // runtime passthrough blit.
    class SSRRenderPass : public RenderGraphNode
    {
      public:
        SSRRenderPass();
        // Not defaulted: the pass owns the blue-noise tile texture (issue #706).
        ~SSRRenderPass() override;

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
            // executing without it would ray-march against stale/garbage state.
            return m_SSRShader && m_SSRShader->IsReady() &&
                   m_SSRResolveShader && m_SSRResolveShader->IsReady() &&
                   m_SSRCompositeShader && m_SSRCompositeShader->IsReady() && m_SSRUBO;
        }

        void SetSSRUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_SSRUBO = ubo;
        }

        // Temporal-resolve knobs (issue #902). The *runtime* lanes — is there a
        // usable history this frame, is there a velocity buffer — are NOT set
        // here: only Execute knows whether those resources actually resolved,
        // and answering that from a snapshot taken before PopulateBlackboard ran
        // is how a first frame ends up blending against an uninitialised buffer.
        // Execute patches the TemporalParams vec4 in place before its draws.
        void SetTemporalSettings(bool enabled, f32 feedback, f32 clipGamma) noexcept
        {
            m_TemporalResolveEnabled = enabled;
            m_TemporalFeedback = feedback;
            m_TemporalClipGamma = clipGamma;
        }

        // Min-depth HZB pyramid parameters for the current viewport, used to
        // fill the SSR UBO's HZBParams (see SSRUBOData). Derived purely from the
        // framebuffer size, so they are valid as soon as the pass is sized —
        // independent of whether Execute() has run yet this frame.
        [[nodiscard]] glm::vec2 GetHZBUVFactor() const noexcept;
        [[nodiscard]] u32 GetHZBMipCount() const noexcept;

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSRShader;
        Ref<Shader> m_SSRResolveShader;
        Ref<Shader> m_SSRCompositeShader;
        Ref<UniformBuffer> m_SSRUBO;

        bool m_TemporalResolveEnabled = true;
        f32 m_TemporalFeedback = 0.92f;
        f32 m_TemporalClipGamma = 1.25f;

        // SSR owns a dedicated MIN-depth HZB (#284). It cannot share GTAO's HZB:
        // that one stores MAX depth and is only generated when GTAO is the
        // active AO technique, whereas SSR runs independently of the AO setting.
        HZBGenerator m_MinHZB;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
        RGTextureHandle m_SelectedGBufferAlbedoTexture{};
        RGTextureHandle m_SelectedVelocityTexture{};
        RGTextureHandle m_SelectedHistoryTexture{};
        RGFramebufferHandle m_SelectedSignalFramebuffer{};
        RGFramebufferHandle m_SelectedResolvedFramebuffer{};

        // The shared blue-noise tile (issue #706), created once in Init(). Not a
        // graph resource: it is pass-owned, immutable and bound with a Persistent
        // heap lifetime.
        RHI::ResourceHandle m_BlueNoiseTexture{};
    };

} // namespace OloEngine
