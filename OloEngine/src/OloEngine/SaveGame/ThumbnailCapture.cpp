#include "OloEnginePCH.h"
#include "ThumbnailCapture.h"

#include <glad/gl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image/stb_image_write.h>

namespace OloEngine
{
    // stbi_write callback that appends to a std::vector<u8>
    static void StbiWriteCallback(void* context, void* data, int size)
    {
        auto* vec = static_cast<std::vector<u8>*>(context);
        auto* bytes = static_cast<const u8*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    }

    std::vector<u8> ThumbnailCapture::CaptureViewport(const Ref<Framebuffer>& framebuffer,
                                                      u32 maxWidth,
                                                      u32 maxHeight)
    {
        OLO_PROFILE_FUNCTION();

        if (!framebuffer)
        {
            OLO_CORE_ERROR("[ThumbnailCapture] Null framebuffer");
            return {};
        }

        const auto& spec = framebuffer->GetSpecification();
        u32 fbWidth = spec.Width;
        u32 fbHeight = spec.Height;

        if (fbWidth == 0 || fbHeight == 0)
        {
            OLO_CORE_ERROR("[ThumbnailCapture] Framebuffer has zero dimensions");
            return {};
        }

        // Read the color attachment (index 0) pixels via GL
        u32 texID = framebuffer->GetColorAttachmentRendererID(0);
        if (texID == 0)
        {
            OLO_CORE_ERROR("[ThumbnailCapture] No color attachment");
            return {};
        }

        std::vector<u8> pixelData(fbWidth * fbHeight * 4);
        glGetTextureImage(texID, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          static_cast<GLsizei>(pixelData.size()), pixelData.data());

        GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("[ThumbnailCapture] GL error reading framebuffer: {}", err);
            return {};
        }

        // Determine thumbnail dimensions (maintain aspect ratio)
        u32 thumbWidth = fbWidth;
        u32 thumbHeight = fbHeight;

        if (thumbWidth > maxWidth || thumbHeight > maxHeight)
        {
            f32 scaleX = static_cast<f32>(maxWidth) / static_cast<f32>(thumbWidth);
            f32 scaleY = static_cast<f32>(maxHeight) / static_cast<f32>(thumbHeight);
            f32 scale = std::min(scaleX, scaleY);
            thumbWidth = static_cast<u32>(static_cast<f32>(thumbWidth) * scale);
            thumbHeight = static_cast<u32>(static_cast<f32>(thumbHeight) * scale);
            thumbWidth = std::max(thumbWidth, 1u);
            thumbHeight = std::max(thumbHeight, 1u);
        }

        // Downscale if needed
        std::vector<u8> thumbData = (thumbWidth != fbWidth || thumbHeight != fbHeight)
                                        ? Downscale(pixelData, fbWidth, fbHeight, thumbWidth, thumbHeight)
                                        : pixelData;

        // Flip vertically (OpenGL convention: origin at bottom-left)
        std::vector<u8> flipped(thumbData.size());
        u32 rowBytes = thumbWidth * 4;
        for (u32 y = 0; y < thumbHeight; ++y)
        {
            std::memcpy(flipped.data() + y * rowBytes,
                        thumbData.data() + (thumbHeight - 1 - y) * rowBytes,
                        rowBytes);
        }

        // Encode as PNG to memory
        std::vector<u8> pngData;
        pngData.reserve(thumbWidth * thumbHeight); // rough estimate
        stbi_write_png_to_func(StbiWriteCallback, &pngData,
                               static_cast<int>(thumbWidth),
                               static_cast<int>(thumbHeight),
                               4, // RGBA
                               flipped.data(),
                               static_cast<int>(rowBytes));

        if (pngData.empty())
        {
            OLO_CORE_ERROR("[ThumbnailCapture] PNG encoding failed");
            return {};
        }

        OLO_CORE_TRACE("[ThumbnailCapture] Captured {}x{} thumbnail ({} bytes PNG)",
                       thumbWidth, thumbHeight, pngData.size());

        return pngData;
    }

    std::vector<u8> ThumbnailCapture::Downscale(const std::vector<u8>& srcData,
                                                u32 srcWidth, u32 srcHeight,
                                                u32 dstWidth, u32 dstHeight)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> dst(dstWidth * dstHeight * 4);

        f32 xRatio = static_cast<f32>(srcWidth) / static_cast<f32>(dstWidth);
        f32 yRatio = static_cast<f32>(srcHeight) / static_cast<f32>(dstHeight);

        for (u32 dy = 0; dy < dstHeight; ++dy)
        {
            for (u32 dx = 0; dx < dstWidth; ++dx)
            {
                // Box filter: average all source pixels that map to this destination pixel
                u32 sx0 = static_cast<u32>(static_cast<f32>(dx) * xRatio);
                u32 sy0 = static_cast<u32>(static_cast<f32>(dy) * yRatio);
                u32 sx1 = static_cast<u32>(static_cast<f32>(dx + 1) * xRatio);
                u32 sy1 = static_cast<u32>(static_cast<f32>(dy + 1) * yRatio);

                sx1 = std::min(sx1, srcWidth);
                sy1 = std::min(sy1, srcHeight);
                if (sx0 == sx1)
                    sx1 = std::min(sx0 + 1, srcWidth);
                if (sy0 == sy1)
                    sy1 = std::min(sy0 + 1, srcHeight);

                u32 r = 0, g = 0, b = 0, a = 0;
                u32 count = 0;

                for (u32 sy = sy0; sy < sy1; ++sy)
                {
                    for (u32 sx = sx0; sx < sx1; ++sx)
                    {
                        u32 srcIdx = (sy * srcWidth + sx) * 4;
                        r += srcData[srcIdx + 0];
                        g += srcData[srcIdx + 1];
                        b += srcData[srcIdx + 2];
                        a += srcData[srcIdx + 3];
                        ++count;
                    }
                }

                u32 dstIdx = (dy * dstWidth + dx) * 4;
                dst[dstIdx + 0] = static_cast<u8>(r / count);
                dst[dstIdx + 1] = static_cast<u8>(g / count);
                dst[dstIdx + 2] = static_cast<u8>(b / count);
                dst[dstIdx + 3] = static_cast<u8>(a / count);
            }
        }

        return dst;
    }

} // namespace OloEngine
