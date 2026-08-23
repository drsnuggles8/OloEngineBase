#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief FSR2 temporal upscale (#684) — the Temporal sibling of EASURenderPass.
    //
    // Occupies exactly the slot EASU does, for exactly the same reason: it turns
    // the reduced-resolution pre-Bloom HDR scene colour into a display-resolution
    // one, EARLY, so every later post stage (Bloom/DOF/ToneMap) runs at full
    // resolution. The two never run in the same frame — PostProcessSettings
    // ::Technique picks one — and both publish into the same downstream position,
    // EASU through Post.EASUColor and this pass through Post.FSR2Color.
    //
    // WHAT MAKES IT DIFFERENT FROM EASU, and therefore what can go wrong:
    //
    //   * It has HISTORY. EASU is a function of one frame; this accumulates
    //     jittered frames through a motion-vector reprojection. Every failure
    //     mode that matters — ghosting, disocclusion trails, a jitter or
    //     motion-vector sign error — produces a completely plausible still image
    //     and only shows up in MOTION. A screenshot is not evidence here.
    //   * It needs DEPTH and VELOCITY at render resolution, so it reads the
    //     REDUCED scene depth / G-Buffer velocity, before DepthVelocityUpscalePass
    //     turns them into display-res copies for the post band.
    //   * It OWNS the jitter. RenderPipeline drives its projection jitter from
    //     TemporalUpscaler::GetJitterOffset rather than the engine's Halton-16
    //     TAA sequence, and hands the same offset back here — the upscaler
    //     subtracts what it believes it asked for, so the two must not disagree.
    //   * It SUBSUMES engine TAA, which the pipeline forces off while this runs.
    //
    // Passthrough semantics match EASURenderPass exactly: when it is disabled or
    // the upscaler is unavailable it declares no output and leaves m_Target null,
    // so downstream name resolution falls past the absent FSR2Color.
    class FSR2RenderPass : public RenderGraphNode
    {
      public:
        FSR2RenderPass();
        ~FSR2RenderPass() override = default;

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

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }

        // Render-scale this pass upscales FROM — the same value
        // Renderer3D::SetRenderScale and EASURenderPass get, so all three derive
        // renderW = floor(displayW * scale) identically.
        void SetRenderScale(f32 scale) noexcept
        {
            m_RenderScale = scale;
        }

        // The sub-pixel jitter RenderPipeline baked into THIS frame's projection,
        // in render-resolution pixels. Not recomputed here on purpose: recomputing
        // it would let the two derivations drift silently.
        void SetJitterPixels(glm::vec2 jitterPixels) noexcept
        {
            m_JitterPixels = jitterPixels;
        }

        // Camera parameters FSR2 uses for its depth reconstruction.
        void SetCameraParams(f32 nearPlane, f32 farPlane, f32 verticalFovRadians) noexcept
        {
            m_NearPlane = nearPlane;
            m_FarPlane = farPlane;
            m_VerticalFovRadians = verticalFovRadians;
        }

        void SetDeltaTimeSeconds(f32 dt) noexcept
        {
            m_DeltaTimeSeconds = dt;
        }

        // Drop the accumulated history on the next dispatch. Set it after a camera
        // cut or any discontinuity the motion vectors cannot describe.
        void RequestHistoryReset() noexcept
        {
            m_ResetHistoryRequested = true;
        }

        // Whether the upscaler can actually run. The pipeline asks BEFORE deciding
        // to disable engine TAA and EASU, because falling back with TAA already
        // switched off would be worse than not trying.
        [[nodiscard]] bool IsUpscalerAvailable() const noexcept
        {
            return m_Upscaler && m_Upscaler->IsAvailable();
        }

        [[nodiscard]] TemporalUpscalerStatus GetUpscalerStatus() const noexcept
        {
            return m_Upscaler ? m_Upscaler->GetStatus() : TemporalUpscalerStatus::NotConfigured;
        }

        // The jitter sequence belongs to the upscaler — see the class comment.
        [[nodiscard]] i32 GetJitterPhaseCount(u32 renderWidth, u32 displayWidth) const;
        [[nodiscard]] glm::vec2 GetJitterOffset(i32 phaseIndex, i32 phaseCount) const;

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return IsUpscalerAvailable();
        }

      private:
        bool m_Enabled = false;
        f32 m_RenderScale = 1.0f;
        PostProcessSettings m_Settings;

        glm::vec2 m_JitterPixels = glm::vec2(0.0f);
        f32 m_NearPlane = 0.1f;
        f32 m_FarPlane = 1000.0f;
        f32 m_VerticalFovRadians = 1.0f;
        f32 m_DeltaTimeSeconds = 0.0f;
        bool m_ResetHistoryRequested = true;

        // The render extent the last dispatch actually ran at. A change means the
        // accumulated history was built on a different pixel grid — and the frame
        // it changes on is jittered against the OLD grid, because the pipeline
        // decides the jitter at BeginScene and re-sizes the scene band at
        // EndScene. Reprojecting across that is a frame of visible swim, so the
        // history is dropped instead.
        u32 m_LastRenderWidth = 0;
        u32 m_LastRenderHeight = 0;

        // Which inputs were missing the last time Execute bailed, so the warning
        // is emitted once per distinct combination instead of every frame.
        u32 m_LastMissingInputMask = 0;

        // Reduced-resolution inputs captured during Setup, resolved in Execute —
        // the same capture-then-resolve shape DepthVelocityUpscalePass uses.
        RGTextureHandle m_SceneDepth;
        RGTextureHandle m_Velocity;

        Ref<TemporalUpscaler> m_Upscaler;
    };
} // namespace OloEngine
