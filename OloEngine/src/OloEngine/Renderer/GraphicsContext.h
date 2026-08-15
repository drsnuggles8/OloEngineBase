#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <functional>

namespace OloEngine
{
    class GraphicsContext
    {
      public:
        virtual ~GraphicsContext() = default;

        virtual void Init() = 0;
        virtual void SwapBuffers() = 0;

        // #691 Phase 7 (Stage 1.6b) — the seam that replaced the Phase 6
        // pilot pass. A backend whose swap path OWNS frame recording (Vulkan:
        // acquire → record → submit → present) hands the acquired backbuffer
        // to this callback, inside the backend's recording bracket, as the
        // neutral handle currency — the callback renders through the ordinary
        // facade (RenderCommand / render-graph execution), targeting the
        // backbuffer as the DEFAULT framebuffer (RenderCommand::
        // BindDefaultFramebuffer resolves to it for the duration).
        //
        // Returning true means "this frame's content is on the backbuffer";
        // false declines the frame and falls back to the backend's clear-only
        // present. The transition to RHI::Access::Present is the BACKEND's
        // (#691 Phase 7 Final): only the presenting backend knows the layout
        // its swap path needs, and it is also what lets the fallback stay
        // safe — it runs only when nothing touched the image. Backends where
        // presentation shows whatever the default framebuffer already holds
        // (GL) ignore this seam entirely.
        struct FrameRenderTarget
        {
            RHI::ResourceHandle Backbuffer;
            u32 Width = 0;
            u32 Height = 0;
        };
        using FrameRenderCallback = std::function<bool(const FrameRenderTarget&)>;
        virtual void SetFrameRenderCallback(FrameRenderCallback callback)
        {
            (void)callback;
        }

        // True when SwapBuffers OWNS frame recording (the callback seam
        // above). Facade draws issued outside that bracket are dropped, so a
        // caller presenting a one-off frame (the shader-warmup progress
        // screen) must route its draws through a temporarily exchanged frame
        // callback instead of the GL immediate-draws-then-swap shape.
        [[nodiscard]] virtual bool DrivesFrameRendering() const
        {
            return false;
        }

        // Swap the installed frame callback, returning the previous one so a
        // one-off presenter can restore it. The no-op default matches
        // SetFrameRenderCallback's: backends that ignore the seam hold no
        // callback to exchange.
        [[nodiscard]] virtual FrameRenderCallback ExchangeFrameRenderCallback(FrameRenderCallback callback)
        {
            (void)callback;
            return {};
        }

        static Scope<GraphicsContext> Create(void* window);
    };
} // namespace OloEngine
