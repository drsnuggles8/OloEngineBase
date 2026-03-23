#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"

#include <functional>

namespace OloEngine
{
    // @brief Render pass that composites UI and 2D overlays on top of the post-processed scene.
    //
    // Sits between PostProcessPass and FinalPass in the render graph:
    //   PostProcess -> UIComposite -> Final
    //
    // On Execute():
    //   1. Blits the input framebuffer (post-processed scene) as background
    //   2. Calls a per-frame render callback set by the Scene for 2D overlays and screen-space UI
    //
    // This centralises all UI/overlay rendering into a single, well-defined pass, replacing
    // the ad-hoc framebuffer binding that was previously scattered through Scene.cpp.
    class UICompositeRenderPass : public RenderPass
    {
      public:
        using RenderCallback = std::function<void()>;

        UICompositeRenderPass();
        ~UICompositeRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
        void SetInputFramebuffer(const Ref<Framebuffer>& input) override;

        // Set a one-shot render callback invoked during Execute() after the background blit.
        // The callback is cleared after each frame. Scene sets this before RenderScene3D().
        void SetRenderCallback(RenderCallback callback);

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Framebuffer> m_InputFramebuffer;
        Ref<Shader> m_BlitShader;
        RenderCallback m_RenderCallback;
        u32 m_NoInputWarningCount = 0;
        u32 m_NoCallbackWarningCount = 0;
    };
} // namespace OloEngine
