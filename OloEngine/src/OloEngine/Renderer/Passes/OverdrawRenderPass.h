#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine
{
    class SceneRenderPass;
    class Shader;

    // @brief Overdraw heatmap debug view (issue #519).
    //
    // A GPU perf-diagnostics visualisation, sibling of the SSAO/SSR/SSGI debug
    // views: it answers "how many layers deep is this frame" directly instead of
    // by A/B toggling passes. Only runs when PostProcessSettings::OverdrawDebugView
    // is on; otherwise it self-skips and its OverdrawColor resource is never
    // declared, so the normal composite reaches the viewport untouched.
    //
    // How it works (path-agnostic — Forward / Forward+ / Deferred):
    //   1. ScenePass batches the opaque command bucket and renders the main view.
    //   2. This pass binds an owned single-channel accumulation target, clears it,
    //      and RE-EXECUTES ScenePass's already-batched bucket with
    //      CommandDispatch::SetOverdrawActive(true): every batchable opaque draw is
    //      swapped for its depth-only DepthPrepass* program (whose fragment stage
    //      emits 1.0), depth testing is DISABLED (so occluded fragments count too)
    //      and blending is additive (GL_ONE, GL_ONE). Each covered fragment adds 1
    //      to the target's red channel — the raw per-pixel overdraw count.
    //   3. A fullscreen heatmap composite (PostProcess_OverdrawHeatmap.glsl) maps
    //      the count to a colour ramp (black -> blue -> green -> yellow -> red) and
    //      writes it into the graph OverdrawColor target, which UICompositePass and
    //      FinalRenderPass pick up at top priority — so the heatmap replaces the
    //      viewport image the same way the other raw-buffer debug views do.
    //
    // Registered late in the post chain (after tone mapping) so the LDR heat
    // colours reach the screen undistorted. Geometry with no depth-only variant
    // (skybox / terrain / voxel / custom shaders) is skipped by the dispatch, so
    // it does not pollute the counter — an honest under-count of exotic geometry.
    class OverdrawRenderPass : public RenderGraphNode
    {
      public:
        OverdrawRenderPass();
        ~OverdrawRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        // GetTarget() is intentionally NOT overridden: it inherits
        // RenderGraphNode::GetTarget(), which returns the base m_Target member
        // that Execute() below populates. A local shadow field here would give
        // ApplyRenderViewport() (also inherited, unmodified) a DIFFERENT,
        // never-populated m_Target to resize on DRS changes — the two would
        // silently drift apart. Sharing the single base member keeps them in
        // sync (mirrors PlanarReflectionRenderPass, which does the same).
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
        // Reports false until the heatmap composite shader has finished
        // (possibly async) compilation, so PopulateBlackboard can withhold
        // declaring OverdrawColor rather than exposing a frame where Execute()
        // would otherwise early-out and leave the graph-allocated target with
        // stale or uninitialised contents. Defined out-of-line: Shader is only
        // forward-declared here (Shader::IsReady() needs the complete type).
        [[nodiscard]] bool IsReadyForExecution() const noexcept override;

        // The ScenePass whose batched opaque bucket is replayed to count overdraw.
        // Wired once at pipeline construction (mirrors PlanarReflectionRenderPass).
        void SetScenePass(SceneRenderPass* scenePass) noexcept
        {
            m_ScenePass = scenePass;
        }

      private:
        void EnsureAccumFramebuffer(u32 width, u32 height);

        bool m_Enabled = false;
        SceneRenderPass* m_ScenePass = nullptr;
        Ref<Shader> m_HeatmapShader;
        Ref<Framebuffer> m_AccumFramebuffer; ///< owned RGBA16F additive counter (count in .r), lazily created
        // Resolved graph OverdrawColor output lives in the inherited
        // RenderGraphNode::m_Target (see the GetTarget() comment above) — no
        // local field here.
    };
} // namespace OloEngine
