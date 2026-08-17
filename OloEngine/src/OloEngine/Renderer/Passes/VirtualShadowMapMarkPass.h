#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    class ShadowMap;

    // @brief Virtual Shadow Map page marking (issue #702) — step 9 of the VSM
    // frame, and the only part of it that does not live in ShadowRenderPass.
    //
    // It is a separate node for one reason: page marking projects the SCENE DEPTH
    // buffer into the clip levels to decide which pages are worth backing, and
    // ShadowPass is the FIRST node in the graph, so that buffer does not exist
    // yet. Marking therefore runs late — after lighting, when depth is final — and
    // the pages it marks are consumed by the NEXT frame's shadow pass.
    //
    // The one-frame lag is real and is handled where it shows: the sampler falls
    // back to coarser clip levels, which are large and almost always resident, so
    // a surface disoccluded this frame gets one frame of blurrier shadow rather
    // than none. Marking each texel's level AND the next coarser one is what keeps
    // that fallback resident.
    //
    // No render target, no framebuffer: a single compute dispatch that writes the
    // page-table SSBO. It self-disables unless the directional VSM is active.
    class VirtualShadowMapMarkPass : public RenderGraphNode
    {
      public:
        VirtualShadowMapMarkPass();
        ~VirtualShadowMapMarkPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;

        void SetShadowMap(ShadowMap* shadowMap) noexcept
        {
            m_ShadowMap = shadowMap;
        }

        // TRUE whenever a shadow map is attached — deliberately NOT
        // "is VSM active".
        //
        // The graph decides which nodes to register when it builds its topology,
        // and toggling VSM at runtime does not rebuild it. An IsEnabled() that
        // tracked VSM's live state therefore left this node CULLED for the whole
        // session after an enable: page marking never ran, so nothing was ever
        // requested, so nothing was ever allocated — a completely unshadowed
        // frame with no error anywhere. Registering unconditionally and
        // self-disabling in EXECUTE is the same shape DDGIProbeUpdatePass uses,
        // and it costs a no-op node when VSM is off.
        //
        // Execute, and NOT Setup: the same trap bites one layer down, because
        // BuildFrameGraph caches the compiled frame behind a fingerprint that
        // does not include the VSM setting either. See the long comment on
        // Setup() in the .cpp, and agent-rules/virtual-shadow-map-page-cache.md
        // §5 — the rule generalises to any pass with a runtime toggle.
        // Both halves are guarded headlessly by
        // tests/Rendering/VirtualShadowMapMarkPassTest.cpp.
        [[nodiscard]] bool IsEnabled() const noexcept override;

      private:
        // The runtime question, asked per frame inside Setup/Execute.
        [[nodiscard]] bool VirtualShadowMapActive() const noexcept;

        ShadowMap* m_ShadowMap = nullptr;
        RGTextureHandle m_SceneDepth{};
    };
} // namespace OloEngine
