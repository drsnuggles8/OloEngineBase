#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <vector>

namespace OloEngine
{
    // Captures the current viewport framebuffer as a PNG thumbnail.
    class ThumbnailCapture
    {
      public:
        // Capture the viewport framebuffer's color attachment 0 as a PNG.
        // Downscales to maxWidth x maxHeight if the framebuffer is larger.
        // Returns empty vector on failure.
        static std::vector<u8> CaptureViewport(const Ref<Framebuffer>& framebuffer,
                                               u32 maxWidth = 320,
                                               u32 maxHeight = 180);

      private:
        // Simple box-filter downscale of RGBA data
        static std::vector<u8> Downscale(const std::vector<u8>& srcData,
                                         u32 srcWidth, u32 srcHeight,
                                         u32 dstWidth, u32 dstHeight);
    };

} // namespace OloEngine
