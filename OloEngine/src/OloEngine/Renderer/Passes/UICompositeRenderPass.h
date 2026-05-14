#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Shader.h"

#include <functional>

namespace OloEngine
{
    // @brief Render pass that composites UI and 2D overlays on top of the post-processed scene.
    //
    // Sits between the post chain output and FinalPass in the render graph:
    //   PostChainOutput -> UIComposite -> Final
    //
    // On Execute():
    //   1. Blits the input framebuffer (post-processed scene) as background
    //   2. Calls a per-frame render callback set by the Scene for 2D overlays and screen-space UI
    //
    // This centralises all UI/overlay rendering into a single, well-defined pass, replacing
    // the ad-hoc framebuffer binding that was previously scattered through Scene.cpp.
    // The current-frame `UIComposite` target is graph-owned and now publishes
    // a producer-owned explicit version during setup; `Execute()` resolves that
    // setup-selected output handle and `GetTarget()` reports the last resolved
    // runtime surface for debug/editor consumers.
    class UICompositeRenderPass : public RenderGraphNode
    {
      public:
        using RenderCallback = std::function<void()>;

        UICompositeRenderPass();
        ~UICompositeRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
        // Phase F slice 44 — SetInputFramebufferHandle removed; Execute() self-resolves

        // Set a one-shot render callback invoked during Execute() after the background blit.
        // The callback is cleared after each frame. Scene sets this before RenderScene3D().
        void SetRenderCallback(RenderCallback callback);

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Shader> m_BlitShader;
        RenderCallback m_RenderCallback;
        u32 m_NoInputWarningCount = 0;
        u32 m_NoCallbackWarningCount = 0;
    };
} // namespace OloEngine
