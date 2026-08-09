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
        // facade (RenderCommand / render-graph execution) and finishes with a
        // barrier to RHI::Access::Present. Returning true means "rendered and
        // left the backbuffer in Present state"; false falls back to the
        // backend's clear-only frame. Backends where presentation shows
        // whatever the default framebuffer already holds (GL) ignore it.
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

        static Scope<GraphicsContext> Create(void* window);
    };
} // namespace OloEngine
