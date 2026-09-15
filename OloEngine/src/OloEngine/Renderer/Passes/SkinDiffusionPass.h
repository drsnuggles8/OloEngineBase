#pragma once

// =============================================================================
// SkinDiffusionPass.h — the screen-space half of skin scattering. Issue #1241.
//
// TWO FULLSCREEN DRAWS, ONE SCRATCH TARGET. The kernel is separable, so the
// pass blurs the scene framebuffer's skin-diffuse attachment horizontally into a
// scratch target, then blurs that vertically and ADDS the difference into scene
// colour. See include/SkinDiffusionCommon.glsl for what the attachment holds and
// Renderer/SkinDiffusion.h for where the maths lives.
//
// WHY IT ADDS A DIFFERENCE RATHER THAN REPLACING SCENE COLOUR. Scene colour
// already holds the sharp composite that #1231 produced. Adding
// `blur(diffuse) - diffuse` swaps one for the other without this pass ever
// reading — or being able to disturb — the specular half, and it makes a frame
// in which this pass does not run the #1231 frame rather than a head with no
// diffuse lighting. It also means the pass owns no scene-colour version of its
// own, so nothing downstream has to be rewired to find the result.
//
// IT IS NOT SSSRenderPass. That pass is SNOW's wrap-lighting blur
// (SSS_Blur.glsl): it blurs the COMBINED scene colour through an alpha mask, in
// texel units, with no profile, no per-channel radius and no world units. The
// two share a UBO binding (UBO_SSS) and nothing else.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>
#include <glm/glm.hpp>

namespace OloEngine
{
    class SkinDiffusionPass : public RenderGraphNode
    {
      public:
        SkinDiffusionPass();
        ~SkinDiffusionPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSettings(const SkinDiffusionSettings& settings) noexcept
        {
            m_Settings = settings;
        }

        // Camera parameters for the world-units-to-pixels projection. Supplied
        // by the pipeline rather than read from a global so the pass can be
        // driven from a test with a known projection — the radius maths is the
        // thing most likely to be wrong and the thing a test can pin.
        // `depthLinearizeA` / `B` are P[2][2] and P[3][2]: the pair that turns a
        // [0,1] device-Z into a positive view-space distance, exactly as GTAO
        // carries them.
        void SetCameraParameters(f32 projectionScaleY, f32 depthLinearizeA, f32 depthLinearizeB) noexcept
        {
            m_ProjectionScaleY = projectionScaleY;
            m_DepthLinearizeA = depthLinearizeA;
            m_DepthLinearizeB = depthLinearizeB;
        }

        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Settings.Enabled;
        }
        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_Shader && m_Shader->IsReady();
        }

        // How many slots uploaded a non-identity kernel last frame. Zero means
        // no authored profile asked for diffusion, which is the normal state of
        // a scene with no skin in it — and the number a test asserts on rather
        // than scraping a log.
        [[nodiscard]] u32 GetActiveProfileCount() const noexcept
        {
            return m_ActiveProfileCount;
        }

      private:
        // Rebuild the per-slot kernels whose authored parameters or quality tier
        // changed since the last frame, and fill `m_GPUData`.
        //
        // The cache is what makes this affordable: a kernel is ~1500 evaluations
        // of a quadrature per channel, which is nothing when a profile is saved
        // and far too much every frame for seven slots.
        void UpdateKernels();

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_UBO;
        Ref<Framebuffer> m_Target;

        RGTextureHandle m_SkinDiffuseTexture{};
        RGTextureHandle m_SceneDepthTexture{};
        RGFramebufferHandle m_ScratchFramebuffer{};
        RGTextureHandle m_ScratchTexture{};
        RGFramebufferHandle m_SceneColorFramebuffer{};

        SkinDiffusionSettings m_Settings{};
        SkinDiffusionUBOData m_GPUData{};

        // Kernel cache, one entry per profile slot. `m_CachedValid` is separate
        // from comparing against a default-constructed parameter set because a
        // profile whose authored values ARE the defaults is a real profile.
        std::array<SkinProfileParameters, kMaxSkinProfileSlots> m_CachedParameters{};
        std::array<SkinDiffusionKernel, kMaxSkinProfileSlots> m_CachedKernels{};
        std::array<bool, kMaxSkinProfileSlots> m_CachedValid{};
        SkinDiffusionQuality m_CachedQuality = SkinDiffusionQuality::Count;

        f32 m_ProjectionScaleY = 1.0f;
        f32 m_DepthLinearizeA = -1.0f;
        f32 m_DepthLinearizeB = -0.2f;
        u32 m_ActiveProfileCount = 0;
    };
} // namespace OloEngine
