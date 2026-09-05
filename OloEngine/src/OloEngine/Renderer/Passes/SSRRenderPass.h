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
    // FIVE DRAWS IN ONE NODE (issues #902, #708), the CloudscapeRenderPass shape:
    //   A. PostProcess_SSR.glsl          → SSRSignal   (the stochastic term
    //                                      ONLY: rgb = the reflection DELTA,
    //                                      (reflection - base) * blend, and
    //                                      a = positive view depth; RT1 = the
    //                                      guide plane)
    //   B. PostProcess_SSRPreBlur.glsl   → SSRPreBlurred (roughness-scaled
    //                                      depth+normal guided blur)
    //   C. PostProcess_SSRResolve.glsl   → SSRResolved (temporal accumulation
    //                                      against SSRHistory; the graph copies
    //                                      this into the history sink)
    //   D. PostProcess_SSRPostBlur.glsl  → SSRDenoised (roughness-scaled
    //                                      cleanup filter)
    //   E. PostProcess_SSRComposite.glsl → SSRColor    (upstream colour + the
    //                                      resolved delta, which reproduces the
    //                                      old mix(base, reflection, blend))
    //
    // WHICH OF ISSUE #708's FIVE STAGES TRANSFERRED FROM SSGI, AND WHICH DID
    // NOT. Two did, unchanged in shape and changed in guide; three did not, and
    // saying so plainly is more useful than forcing one denoiser onto two very
    // different signals:
    //
    //   * pre-blur and post-blur — YES, sharing the kernel in
    //     include/SpatialDenoise.glsl, but with the radius driven by ROUGHNESS
    //     rather than by variance and history length. A mirror carries the
    //     sharpest detail in the frame and its reflection of a high-contrast
    //     edge is legitimately high-variance, so a variance guide would blur
    //     precisely the pixels that must stay sharp. What sets the correct
    //     filter width for a reflection is the width of the specular lobe the
    //     single VNDF sample came from, and that is roughness.
    //   * ray distribution over the 2x2 quad — NO. It subdivides the ray strata
    //     of a multi-ray hemisphere across neighbouring pixels; SSR draws ONE
    //     VNDF sample per pixel, so there are no strata to subdivide. The
    //     blue-noise rotation already decorrelates neighbours at one sample.
    //   * half-resolution trace and guided upscale — NO. Indirect diffuse is
    //     smooth, which is exactly why half resolution costs SSGI so little; a
    //     reflection is the opposite, and tracing it at a quarter of the pixels
    //     discards the detail that makes it read as a reflection at all.
    //   * variance-driven history length — NO. SSR's resolve keeps no moment
    //     attachments to read a variance or a history length from, and the guide
    //     would be wrong for specular anyway (see the pre/post-blur note above),
    //     so adding them would be cost for a worse result.
    //
    // The split into separate draws is the whole point: compositing into the
    // scene colour made the output un-accumulable, because temporally blending
    // it would smear the base colour along with the noise. And every stage here
    // gathers a NEIGHBOURHOOD of the stage before it - the pre-blur's Poisson
    // disc, the resolve's 3x3 clip box, the post-blur's disc - so each one needs
    // its input in a real texture that the whole previous draw has finished
    // writing. None of these can be folded into the one before it. The resolve
    // is also what pays for the raised SSRMaxRoughness default.
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
    //   * SSRSignal / SSRPreBlurred / SSRDenoised (RGBA16F, graph scratch) —
    //     the denoiser chain's intermediates. SSRSignal's attachment 1 is the
    //     guide plane whose roughness sets both spatial stages' radius.
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
                   m_SSRPreBlurShader && m_SSRPreBlurShader->IsReady() &&
                   m_SSRResolveShader && m_SSRResolveShader->IsReady() &&
                   m_SSRPostBlurShader && m_SSRPostBlurShader->IsReady() &&
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

        // Denoiser-stage gates (issue #708). Only whether the two spatial
        // stages run at all: their radii travel in the UBO, which the pipeline
        // fills. A stage whose radius is 0 is SKIPPED, and its consumer binds
        // the previous stage's output instead — see Execute.
        void SetDenoiseSettings(bool preBlurEnabled, bool postBlurEnabled) noexcept
        {
            m_PreBlurEnabled = preBlurEnabled;
            m_PostBlurEnabled = postBlurEnabled;
        }

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSRShader;
        Ref<Shader> m_SSRPreBlurShader;
        Ref<Shader> m_SSRResolveShader;
        Ref<Shader> m_SSRPostBlurShader;
        Ref<Shader> m_SSRCompositeShader;
        Ref<UniformBuffer> m_SSRUBO;

        bool m_PreBlurEnabled = true;
        bool m_PostBlurEnabled = true;

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
        RGFramebufferHandle m_SelectedPreBlurredFramebuffer{};
        RGFramebufferHandle m_SelectedResolvedFramebuffer{};
        RGFramebufferHandle m_SelectedDenoisedFramebuffer{};

        // The shared blue-noise tile (issue #706), created once in Init(). Not a
        // graph resource: it is pass-owned, immutable and bound with a Persistent
        // heap lifetime.
        RHI::ResourceHandle m_BlueNoiseTexture{};
    };

} // namespace OloEngine
