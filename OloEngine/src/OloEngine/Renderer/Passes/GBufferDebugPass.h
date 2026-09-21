#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Debug/DebugViewProvenance.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    // @brief Extracts one G-Buffer channel into the scene colour target for
    // the deferred debug views (issue #1329).
    //
    // WHY THIS IS A GRAPH NODE AND NOT A TAIL OF ScenePass, which is where it
    // used to live. The G-Buffer has four writers, and ScenePass is only the
    // first of them: `VirtualGeometryPass`, `DeferredGPUOcclusionPass` (the
    // two-phase occlusion cull's phase 2) and `DeferredOpaqueDecalPass` all
    // overwrite its attachments afterwards, as separate nodes the scheduler
    // runs after ScenePass has returned. A blit at the end of ScenePass::
    // Execute therefore captured a G-Buffer that no lighting consumer ever
    // saw — an albedo view with the decals missing, a normal view without the
    // clusters — and it did so silently, in the one mode whose entire purpose
    // is to be trusted while diagnosing something else.
    //
    // Registered immediately before `DeferredLightingPass`, which is the last
    // point at which the G-Buffer is still the version lighting will consume,
    // and which itself early-outs whenever a debug channel is selected (so
    // this pass's image is what reaches the viewport). Registration is
    // unconditional on the deferred path: the node's declarations must not
    // depend on the debug channel, because graph topology is fingerprinted and
    // cached, and a channel-gated declaration would be culled for the whole
    // session (`technique-selection-seams.md`, issue #1315). Execute() is
    // where the channel is read, and channel 0 no-ops.
    //
    // MSAA. The per-sample path leaves the colour attachments multisample for
    // `DeferredLighting_MSAA`, so the single-sample copy this pass blits from
    // has to be refreshed after the late writes. That resolve is
    // `DeferredOpaqueDecalPass`'s — the LAST G-Buffer writer — rather than this
    // pass's, because a diagnostic that writes the resource it reports on is
    // both a hazard the graph rejects and a reason the frame differs. Relying
    // on ScenePass's resolve was the other half of the original defect: it runs
    // before virtual geometry, occlusion phase 2 and the decals.
    // `GBuffer::Resolve` keeps the flags lane out of the averaging blit (issue
    // #996), so the material flags that reach the RMA view are a real sample's,
    // not an average.
    class GBufferDebugPass : public RenderGraphNode
    {
      public:
        GBufferDebugPass();
        ~GBufferDebugPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;

        void SetGBuffer(const Ref<GBuffer>& gbuffer) noexcept
        {
            m_GBuffer = gbuffer;
        }
        // DeferredSettings::DebugChannel: 0 = off, 1 = Albedo, 2 = Normal,
        // 3 = Roughness/Metallic/AO, 4 = Emissive, 5 = Velocity.
        void SetDebugChannel(u32 channel) noexcept
        {
            m_DebugChannel = channel;
        }
        void SetPerSampleLighting(bool enable) noexcept
        {
            m_PerSampleLighting = enable;
        }

        [[nodiscard]] u32 GetDebugChannel() const noexcept
        {
            return m_DebugChannel;
        }

      private:
        // The blit itself. Extracted from `SceneRenderPass::BlitGBufferDebug`
        // unchanged apart from where it reads its target from. Returns false
        // when it produced nothing — the RMA channel's shader failing to load
        // is the one way that happens — so Execute can retire the capture
        // record instead of stamping "current" on a frame it did not draw.
        [[nodiscard]] bool BlitChannel(RGCommandContext& context, u32 channel);

        Ref<GBuffer> m_GBuffer;
        Ref<Framebuffer> m_Target;
        u32 m_DebugChannel = 0;
        bool m_PerSampleLighting = false;
        // Fullscreen shader that gathers RT0.a (metallic), RT1.z (roughness),
        // RT1.w (AO) into one RGB image for channel 3. The other channels are
        // cheap single-attachment blits. Created on first use, so a session
        // that never selects the channel compiles nothing.
        Ref<Shader> m_DebugRMAShader;
        // Loud once, not once per frame: without the RMA shader the channel-3
        // view is a frozen viewport, and a per-frame error would bury it.
        bool m_DebugRMAShaderFailed = false;
    };
} // namespace OloEngine
