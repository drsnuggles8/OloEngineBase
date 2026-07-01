#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief FSR1 depth + velocity upscale (#480). Companion to EASURenderPass.
    //
    // When the scene renders below display resolution, EASU upscales the HDR
    // COLOUR to display res but the depth and motion-vector buffers stay at the
    // reduced scene resolution. The display-res post band (DOF, Fog, MotionBlur,
    // TAA, and ToneMap's underwater fog) reads those buffers, so it would sample a
    // bilinear-blended reduced depth/velocity — inventing intermediate depths
    // across silhouettes (DOF halos, bad TAA reprojection). This pass runs right
    // after EASU and NEAREST-upscales the reduced depth + velocity to full display
    // res (correct per-texel values at full sample density), then swaps the
    // blackboard SceneDepth / Velocity handles so every post-EASU consumer picks
    // up the full-res versions. Passes BEFORE it (SSAO, deferred lighting, the SS
    // band) keep the reduced handles — Setup runs in execution order.
    //
    // Output: UpscaledDepthVelocity framebuffer (RT0 = R32F depth, RT1 = RG16F
    // velocity), full display resolution. No-ops when Upscale == Off.
    class DepthVelocityUpscalePass : public RenderGraphNode
    {
      public:
        DepthVelocityUpscalePass();
        ~DepthVelocityUpscalePass() override = default;

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

        // Render-scale this pass upsamples FROM (== EASU's). Set per-frame so the
        // nearest-tap snaps to the actual reduced texel grid.
        void SetRenderScale(f32 scale) noexcept
        {
            m_RenderScale = scale;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_Shader && m_Shader->IsReady() && m_UBO;
        }

      private:
        bool m_Enabled = false;
        f32 m_RenderScale = 1.0f;

        // Reduced-resolution inputs captured during Setup (before the blackboard
        // handles are swapped to the full-res outputs), resolved + bound in Execute.
        RGTextureHandle m_ReducedDepth;
        RGTextureHandle m_ReducedVelocity;

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_UBO;
    };
} // namespace OloEngine
