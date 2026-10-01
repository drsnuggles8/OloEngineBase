#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Texture.h"

#include <algorithm>
#include <optional>

// Format-estimate byte sizes for the memory report (issue #1342): one table for every
// place that books or estimates renderer texture bytes from an ImageFormat or a
// FramebufferTextureFormat. (The render-graph planner sizes RGResourceFormat
// descriptors with its own table, which covers every value of that enum.) Before this there were three
// private tables that disagreed — the GL framebuffer counted 4 bytes for every colour
// attachment and ignored samples, the GL texture ignored its mip chain, and the transient
// pool returned 0 for RG8 and R16F — so the same texture had three sizes depending on who
// was asked.
//
// These are FORMAT ESTIMATES: texels x bytes-per-texel x layers x samples over the mip
// chain. A driver's padding, tiling and compression metadata are invisible here; on
// Vulkan the committed size from VMA supersedes them.
namespace OloEngine::RendererMemoryFormat
{
    // Bytes per texel of an uncompressed format; nullopt for None and for block-compressed
    // formats, whose size is per 4x4 block (see BlockBytes).
    [[nodiscard]] constexpr std::optional<u32> BytesPerTexel(const ImageFormat format)
    {
        switch (format)
        {
            case ImageFormat::R8:
            case ImageFormat::R8UI:
                return 1u;
            case ImageFormat::R16UI:
            case ImageFormat::RG8:
                return 2u;
            case ImageFormat::RGB8:
                return 3u;
            case ImageFormat::RGBA8:
            case ImageFormat::RG16UI:
            case ImageFormat::RG16F:
            case ImageFormat::R32F:
            case ImageFormat::R32I:
            case ImageFormat::R32UI:
            case ImageFormat::DEPTH24STENCIL8:
                return 4u;
            case ImageFormat::RGBA16F:
            case ImageFormat::RG32F:
                return 8u;
            case ImageFormat::RGB32F:
                return 12u;
            case ImageFormat::RGBA32F:
            case ImageFormat::RGBA32UI:
                return 16u;
            case ImageFormat::None:
            case ImageFormat::BC4:
            case ImageFormat::BC5:
            case ImageFormat::BC6H:
            case ImageFormat::BC6HS:
            case ImageFormat::BC7:
                return std::nullopt;
        }
        return std::nullopt;
    }

    // Bytes per 4x4 block of a block-compressed format; nullopt for everything else.
    [[nodiscard]] constexpr std::optional<u32> BlockBytes(const ImageFormat format)
    {
        switch (format)
        {
            case ImageFormat::BC4:
                return 8u;
            case ImageFormat::BC5:
            case ImageFormat::BC6H:
            case ImageFormat::BC6HS:
            case ImageFormat::BC7:
                return 16u;
            default:
                return std::nullopt;
        }
    }

    // Bytes per texel of a framebuffer attachment format; nullopt for None.
    [[nodiscard]] constexpr std::optional<u32> BytesPerTexel(const FramebufferTextureFormat format)
    {
        switch (format)
        {
            case FramebufferTextureFormat::RGBA8:
            case FramebufferTextureFormat::RG16F:
            case FramebufferTextureFormat::R32F:
            case FramebufferTextureFormat::RED_INTEGER:
            case FramebufferTextureFormat::DEPTH24STENCIL8:
            case FramebufferTextureFormat::DEPTH_COMPONENT32F:
                return 4u;
            case FramebufferTextureFormat::RGB16F:
                return 6u;
            case FramebufferTextureFormat::RGBA16F:
            case FramebufferTextureFormat::RG32F:
                return 8u;
            case FramebufferTextureFormat::RGB32F:
                return 12u;
            case FramebufferTextureFormat::RGBA32F:
                return 16u;
            case FramebufferTextureFormat::None:
                return std::nullopt;
        }
        return std::nullopt;
    }

    // floor(log2(max(width, height))) + 1: the level count of a full chain.
    [[nodiscard]] constexpr u32 FullMipCount(const u32 width, const u32 height)
    {
        u32 levels = 1u;
        for (u32 extent = std::max(width, height); extent > 1u; extent /= 2u)
            ++levels;
        return levels;
    }

    // Texels over a mip chain of `mipLevels` levels starting at width x height.
    [[nodiscard]] constexpr u64 MipChainTexels(const u32 width, const u32 height, const u32 mipLevels)
    {
        u64 texels = 0;
        u32 w = std::max(width, 1u);
        u32 h = std::max(height, 1u);
        for (u32 level = 0; level < std::max(mipLevels, 1u); ++level)
        {
            texels += static_cast<u64>(w) * h;
            w = std::max(w / 2u, 1u);
            h = std::max(h / 2u, 1u);
        }
        return texels;
    }

    // 4x4 blocks over a mip chain (each level rounds up to whole blocks).
    [[nodiscard]] constexpr u64 MipChainBlocks(const u32 width, const u32 height, const u32 mipLevels)
    {
        u64 blocks = 0;
        u32 w = std::max(width, 1u);
        u32 h = std::max(height, 1u);
        for (u32 level = 0; level < std::max(mipLevels, 1u); ++level)
        {
            blocks += static_cast<u64>((w + 3u) / 4u) * ((h + 3u) / 4u);
            w = std::max(w / 2u, 1u);
            h = std::max(h / 2u, 1u);
        }
        return blocks;
    }

    // Storage of a 2D (array) image: every mip level, every layer, every sample.
    // nullopt when the format has no known size — the caller must say "unknown", not 0.
    [[nodiscard]] constexpr std::optional<u64> ImageBytes(const ImageFormat format, const u32 width, const u32 height,
                                                          const u32 mipLevels = 1u, const u32 layers = 1u,
                                                          const u32 samples = 1u)
    {
        const u64 layerCount = std::max(layers, 1u);
        if (const auto texel = BytesPerTexel(format))
        {
            return MipChainTexels(width, height, mipLevels) * *texel * layerCount * std::max(samples, 1u);
        }
        if (const auto block = BlockBytes(format))
        {
            return MipChainBlocks(width, height, mipLevels) * *block * layerCount;
        }
        return std::nullopt;
    }

    // One framebuffer attachment (single level; MSAA attachments hold every sample).
    [[nodiscard]] constexpr std::optional<u64> AttachmentBytes(const FramebufferTextureFormat format, const u32 width,
                                                               const u32 height, const u32 samples = 1u)
    {
        if (const auto texel = BytesPerTexel(format))
        {
            return static_cast<u64>(std::max(width, 1u)) * std::max(height, 1u) * *texel * std::max(samples, 1u);
        }
        return std::nullopt;
    }

    // Every attachment of a framebuffer, each at the framebuffer's sample count. nullopt
    // when an attachment's format has no known size.
    [[nodiscard]] inline std::optional<u64> FramebufferBytes(const FramebufferSpecification& spec)
    {
        u64 total = 0;
        for (const auto& attachment : spec.Attachments.Attachments)
        {
            if (attachment.TextureFormat == FramebufferTextureFormat::None)
                continue;
            const auto bytes = AttachmentBytes(attachment.TextureFormat, spec.Width, spec.Height, spec.Samples);
            if (!bytes)
                return std::nullopt;
            total += *bytes;
        }
        return total;
    }
} // namespace OloEngine::RendererMemoryFormat
