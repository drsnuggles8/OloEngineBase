#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

// Vendored encoders/decoders (bc7enc_rdo, MIT / public domain). Only this TU pulls
// them in, keeping the header renderer-agnostic. bc7enc: BC7 encode; rgbcx: BC5
// encode + decode; bc7decomp: BC7 decode.
#include <bc7enc.h>
#include <bc7decomp.h>
#include <rgbcx.h>

// stb_image is only *declared* here — STB_IMAGE_IMPLEMENTATION lives in
// Platform/OpenGL/OpenGLTexture.cpp, which provides the definitions at link time.
#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace OloEngine
{
    namespace
    {
        // One-time init for both encoders. bc7enc builds its mode tables; rgbcx builds
        // its BC1/4/5 tables. Both are process-global and idempotent behind call_once.
        void EnsureEncodersInitialized()
        {
            static std::once_flag s_InitFlag;
            std::call_once(s_InitFlag, []()
                           {
                               ::bc7enc_compress_block_init();
                               ::rgbcx::init(); });
        }

        // Expand tightly-packed `channels`-per-texel source to RGBA8 (4 bytes/texel).
        // 1ch -> R,R,R,255 (greyscale); 2ch -> R,G,0,255; 3ch -> R,G,B,255; 4ch -> as-is.
        std::vector<u8> ExpandToRGBA8(const u8* pixels, u32 width, u32 height, u32 channels)
        {
            const sizet texelCount = static_cast<sizet>(width) * height;
            std::vector<u8> rgba(texelCount * 4);
            for (sizet i = 0; i < texelCount; ++i)
            {
                const u8* src = pixels + i * channels;
                u8* dst = rgba.data() + i * 4;
                switch (channels)
                {
                    case 1:
                        dst[0] = src[0];
                        dst[1] = src[0];
                        dst[2] = src[0];
                        dst[3] = 255;
                        break;
                    case 2:
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = 0;
                        dst[3] = 255;
                        break;
                    case 3:
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = 255;
                        break;
                    default: // 4
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = src[3];
                        break;
                }
            }
            return rgba;
        }

        // Box-filter downsample an RGBA8 image to half size (each dim halved, min 1).
        // NOTE: filtering is done in the stored 8-bit space (gamma-naive for sRGB) —
        // matches glGenerateMipmap's default behaviour; a linear-space mip build is a
        // deliberate follow-up. `outW`/`outH` receive the reduced dimensions.
        std::vector<u8> DownsampleRGBA8(const std::vector<u8>& src, u32 width, u32 height, u32& outW, u32& outH)
        {
            outW = std::max(1u, width / 2);
            outH = std::max(1u, height / 2);
            std::vector<u8> dst(static_cast<sizet>(outW) * outH * 4);

            for (u32 y = 0; y < outH; ++y)
            {
                const u32 sy0 = std::min(y * 2, height - 1);
                const u32 sy1 = std::min(y * 2 + 1, height - 1);
                for (u32 x = 0; x < outW; ++x)
                {
                    const u32 sx0 = std::min(x * 2, width - 1);
                    const u32 sx1 = std::min(x * 2 + 1, width - 1);
                    const u8* p00 = &src[(static_cast<sizet>(sy0) * width + sx0) * 4];
                    const u8* p01 = &src[(static_cast<sizet>(sy0) * width + sx1) * 4];
                    const u8* p10 = &src[(static_cast<sizet>(sy1) * width + sx0) * 4];
                    const u8* p11 = &src[(static_cast<sizet>(sy1) * width + sx1) * 4];
                    u8* d = &dst[(static_cast<sizet>(y) * outW + x) * 4];
                    for (u32 c = 0; c < 4; ++c)
                    {
                        const u32 sum = static_cast<u32>(p00[c]) + p01[c] + p10[c] + p11[c];
                        d[c] = static_cast<u8>((sum + 2) / 4);
                    }
                }
            }
            return dst;
        }

        // Gather the 4x4 block at (blockX, blockY) from an RGBA8 image into a 64-byte
        // (16 texel x RGBA) contiguous buffer, clamping to the edge for partial blocks.
        void GatherBlockRGBA(const std::vector<u8>& rgba, u32 width, u32 height, u32 blockX, u32 blockY,
                             std::array<u8, 64>& outBlock)
        {
            for (u32 ry = 0; ry < 4; ++ry)
            {
                const u32 sy = std::min(blockY * 4 + ry, height - 1);
                for (u32 rx = 0; rx < 4; ++rx)
                {
                    const u32 sx = std::min(blockX * 4 + rx, width - 1);
                    const u8* src = &rgba[(static_cast<sizet>(sy) * width + sx) * 4];
                    u8* dst = &outBlock[(static_cast<sizet>(ry) * 4 + rx) * 4];
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                }
            }
        }

        // Encode a single RGBA8 mip level's blocks with the given per-block encoder.
        // `encodeBlock(dst16, block64)` fills 16 output bytes from a 64-byte RGBA block.
        template<typename EncodeBlockFn>
        std::vector<u8> EncodeLevel(const std::vector<u8>& rgba, u32 width, u32 height, EncodeBlockFn&& encodeBlock)
        {
            const u32 bx = TextureCompression::BlockCount(width);
            const u32 by = TextureCompression::BlockCount(height);
            std::vector<u8> out(static_cast<sizet>(bx) * by * 16);

            std::array<u8, 64> block{};
            for (u32 y = 0; y < by; ++y)
            {
                for (u32 x = 0; x < bx; ++x)
                {
                    GatherBlockRGBA(rgba, width, height, x, y, block);
                    u8* dst = out.data() + (static_cast<sizet>(y) * bx + x) * 16;
                    encodeBlock(dst, block.data());
                }
            }
            return out;
        }

        // Little-endian POD append/read helpers for the .olotex blob.
        void AppendU32(std::vector<u8>& out, u32 value)
        {
            out.push_back(static_cast<u8>(value & 0xFF));
            out.push_back(static_cast<u8>((value >> 8) & 0xFF));
            out.push_back(static_cast<u8>((value >> 16) & 0xFF));
            out.push_back(static_cast<u8>((value >> 24) & 0xFF));
        }

        bool ReadU32(std::span<const u8> blob, sizet& cursor, u32& outValue)
        {
            if (cursor + 4 > blob.size())
                return false;
            outValue = static_cast<u32>(blob[cursor]) | (static_cast<u32>(blob[cursor + 1]) << 8) |
                       (static_cast<u32>(blob[cursor + 2]) << 16) | (static_cast<u32>(blob[cursor + 3]) << 24);
            cursor += 4;
            return true;
        }

        constexpr std::array<u8, 4> kOloTexMagic = { 'O', 'T', 'E', 'X' };
        constexpr u32 kOloTexVersion = 1;

        // Container header flag bits (persisted in the .olotex blob).
        constexpr u32 kFlagSRGB = 0x1u;
        constexpr u32 kFlagHasAlpha = 0x2u;

        // Hard ceiling for a deserialized .olotex header — a malformed/hostile file must
        // not drive an unbounded allocation or an illegal GL level count. 16384 is well
        // above any real shipped texture and keeps block math inside u32.
        constexpr u32 kMaxTextureDimension = 16384;

        // Highest legitimate mip level count for the given dimensions: floor(log2(max))+1.
        u32 MaxMipLevels(u32 width, u32 height)
        {
            u32 dim = std::max(width, height);
            u32 levels = 1;
            while (dim > 1)
            {
                dim >>= 1;
                ++levels;
            }
            return levels;
        }
    } // namespace

    namespace TextureCompression
    {
        bool IsLikelyColorTexture(std::string_view filename)
        {
            std::string lower(filename);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });

            // Data-texture keywords trump colour keywords — e.g. "Diffuse_AO.png" is an
            // AO map even though it carries "Diffuse", because the "_AO" suffix is the
            // meaningful tag. (Kept in sync with the tests in SRGBTextureSupportTest.)
            constexpr std::string_view dataKeywords[] = {
                "normal", "_n.", "_n_", "norm", "metal", "_m.", "_m_", "metallic",
                "rough", "_r.", "_r_", "roughness", "_ao.", "_ao_", "ambient_occlusion",
                "ambientocclusion", "occlusion", "height", "_h.", "_h_", "displace",
                "disp", "spec", "_s.", "bump", "_orm.", "_arm.", "_orm_", "_arm_"
            };
            for (std::string_view kw : dataKeywords)
            {
                if (lower.find(kw) != std::string::npos)
                    return false;
            }

            constexpr std::string_view colorKeywords[] = {
                "albedo", "_a.", "_a_", "basecolor", "base_color", "diffuse", "_d.",
                "_d_", "color", "colour", "emissive", "emission", "_e.", "_e_"
            };
            for (std::string_view kw : colorKeywords)
            {
                if (lower.find(kw) != std::string::npos)
                    return true;
            }

            // Ambiguous: be conservative and treat as linear (avoid double gamma decode).
            return false;
        }

        u32 BlockSizeBytes(TextureCompressionFormat format)
        {
            switch (format)
            {
                case TextureCompressionFormat::BC7:
                case TextureCompressionFormat::BC5:
                    return 16;
                case TextureCompressionFormat::None:
                    return 0;
            }
            return 0;
        }

        u32 BlockCount(u32 dimension)
        {
            return std::max(1u, (dimension + 3) / 4);
        }

        sizet MipByteSize(TextureCompressionFormat format, u32 width, u32 height)
        {
            return static_cast<sizet>(BlockCount(width)) * BlockCount(height) * BlockSizeBytes(format);
        }

        CompressedTextureImage EncodeBC7(const u8* pixels, u32 width, u32 height, u32 channels, bool srgb, bool generateMips)
        {
            OLO_PROFILE_FUNCTION();

            CompressedTextureImage image;
            if (!pixels || width == 0 || height == 0 || channels == 0 || channels > 4)
            {
                OLO_CORE_ERROR("TextureCompression::EncodeBC7 - invalid input ({}x{}, {} ch)", width, height, channels);
                return image;
            }

            EnsureEncodersInitialized();

            // Full init first: the *_weights helpers below only set m_perceptual +
            // m_weights[] and leave mode_mask / max_partitions / uber_level etc.
            // uninitialized, so calling one WITHOUT the base init encodes from garbage
            // params. bc7enc_compress_block_params_init sets every field (and defaults
            // to perceptual weights).
            ::bc7enc_compress_block_params params;
            ::bc7enc_compress_block_params_init(&params);
            // Perceptual (YCbCr-weighted) error suits sRGB albedo; linear weights are
            // correct for non-colour data packed as BC7.
            if (!srgb)
                ::bc7enc_compress_block_params_init_linear_weights(&params);

            const auto encodeBlock = [&params](u8* dst, const u8* block64)
            {
                ::bc7enc_compress_block(dst, block64, &params);
            };

            image.Format = TextureCompressionFormat::BC7;
            image.Width = width;
            image.Height = height;
            image.SRGB = srgb;
            // A 4-channel source carries alpha; 1/3-channel sources get a constant
            // alpha=255 from ExpandToRGBA8, which is opaque — so don't report alpha for
            // those (keeps opaque BC7 albedo out of the transparent render pass).
            image.HasAlpha = (channels == 4);

            std::vector<u8> level = ExpandToRGBA8(pixels, width, height, channels);
            u32 mw = width;
            u32 mh = height;
            while (true)
            {
                image.Mips.push_back(EncodeLevel(level, mw, mh, encodeBlock));
                if (!generateMips || (mw == 1 && mh == 1))
                    break;
                u32 nw = 0;
                u32 nh = 0;
                level = DownsampleRGBA8(level, mw, mh, nw, nh);
                mw = nw;
                mh = nh;
            }
            return image;
        }

        CompressedTextureImage EncodeBC5(const u8* pixels, u32 width, u32 height, u32 channels, bool generateMips)
        {
            OLO_PROFILE_FUNCTION();

            CompressedTextureImage image;
            // BC5 encodes two channels (R,G) — a single-channel source would silently
            // duplicate R into G and produce a meaningless "normal" map, so require >= 2.
            if (!pixels || width == 0 || height == 0 || channels < 2 || channels > 4)
            {
                OLO_CORE_ERROR("TextureCompression::EncodeBC5 - invalid input ({}x{}, {} ch; needs 2-4 channels)", width, height, channels);
                return image;
            }

            EnsureEncodersInitialized();

            // rgbcx::encode_bc5 reads channels 0 and 1 (R,G) from a 4-byte-stride buffer.
            const auto encodeBlock = [](u8* dst, const u8* block64)
            {
                ::rgbcx::encode_bc5(dst, block64, 0, 1, 4);
            };

            image.Format = TextureCompressionFormat::BC5;
            image.Width = width;
            image.Height = height;
            image.SRGB = false; // BC5 is always linear (normal xy / two-channel data)

            std::vector<u8> level = ExpandToRGBA8(pixels, width, height, channels);
            u32 mw = width;
            u32 mh = height;
            while (true)
            {
                image.Mips.push_back(EncodeLevel(level, mw, mh, encodeBlock));
                if (!generateMips || (mw == 1 && mh == 1))
                    break;
                u32 nw = 0;
                u32 nh = 0;
                level = DownsampleRGBA8(level, mw, mh, nw, nh);
                mw = nw;
                mh = nh;
            }
            return image;
        }

        bool DecodeToRGBA8(const CompressedTextureImage& image, u32 mipLevel,
                           std::vector<u8>& outRGBA8, u32& outWidth, u32& outHeight)
        {
            if (!image.IsValid() || mipLevel >= image.MipLevels())
                return false;

            const u32 mw = std::max(1u, image.Width >> mipLevel);
            const u32 mh = std::max(1u, image.Height >> mipLevel);
            outWidth = mw;
            outHeight = mh;
            outRGBA8.assign(static_cast<sizet>(mw) * mh * 4, 0);

            const std::vector<u8>& blocks = image.Mips[mipLevel];
            const u32 bxCount = BlockCount(mw);
            const u32 byCount = BlockCount(mh);
            if (blocks.size() < static_cast<sizet>(bxCount) * byCount * 16)
            {
                OLO_CORE_ERROR("TextureCompression::DecodeToRGBA8 - mip {} block data truncated", mipLevel);
                return false;
            }

            std::array<u8, 64> decoded{};
            for (u32 by = 0; by < byCount; ++by)
            {
                for (u32 bx = 0; bx < bxCount; ++bx)
                {
                    const u8* blockPtr = blocks.data() + (static_cast<sizet>(by) * bxCount + bx) * 16;
                    if (image.Format == TextureCompressionFormat::BC7)
                    {
                        // bc7decomp writes 16 color_rgba (RGBA) contiguously.
                        static_assert(sizeof(::bc7decomp::color_rgba) == 4, "color_rgba must be 4 bytes");
                        ::bc7decomp::unpack_bc7(blockPtr, reinterpret_cast<::bc7decomp::color_rgba*>(decoded.data()));
                    }
                    else // BC5
                    {
                        decoded.fill(0);
                        ::rgbcx::unpack_bc5(blockPtr, decoded.data(), 0, 1, 4);
                        for (u32 i = 0; i < 16; ++i)
                            decoded[i * 4 + 3] = 255; // opaque alpha; b stays 0
                    }

                    // Scatter the 4x4 block into the output, cropping partial edge blocks.
                    for (u32 ry = 0; ry < 4; ++ry)
                    {
                        const u32 dy = by * 4 + ry;
                        if (dy >= mh)
                            break;
                        for (u32 rx = 0; rx < 4; ++rx)
                        {
                            const u32 dx = bx * 4 + rx;
                            if (dx >= mw)
                                break;
                            const u8* s = &decoded[(static_cast<sizet>(ry) * 4 + rx) * 4];
                            u8* d = &outRGBA8[(static_cast<sizet>(dy) * mw + dx) * 4];
                            d[0] = s[0];
                            d[1] = s[1];
                            d[2] = s[2];
                            d[3] = s[3];
                        }
                    }
                }
            }
            return true;
        }

        std::vector<u8> SerializeToBlob(const CompressedTextureImage& image)
        {
            std::vector<u8> blob;
            if (!image.IsValid())
                return blob;

            // Total size is fully known up front (28-byte header + per-mip 4-byte length
            // + block bytes); reserve once so the appends don't repeatedly realloc.
            sizet total = 4 + 6 * sizeof(u32);
            for (const std::vector<u8>& mip : image.Mips)
                total += sizeof(u32) + mip.size();
            blob.reserve(total);

            u32 flags = 0;
            if (image.SRGB)
                flags |= kFlagSRGB;
            if (image.HasAlpha)
                flags |= kFlagHasAlpha;

            blob.insert(blob.end(), kOloTexMagic.begin(), kOloTexMagic.end());
            AppendU32(blob, kOloTexVersion);
            AppendU32(blob, static_cast<u32>(image.Format));
            AppendU32(blob, image.Width);
            AppendU32(blob, image.Height);
            AppendU32(blob, flags);
            AppendU32(blob, image.MipLevels());
            for (const std::vector<u8>& mip : image.Mips)
            {
                AppendU32(blob, static_cast<u32>(mip.size()));
                blob.insert(blob.end(), mip.begin(), mip.end());
            }
            return blob;
        }

        bool DeserializeFromBlob(std::span<const u8> blob, CompressedTextureImage& out)
        {
            if (blob.size() < 4 || !std::equal(kOloTexMagic.begin(), kOloTexMagic.end(), blob.begin()))
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - bad magic / too small");
                return false;
            }

            sizet cursor = 4;
            u32 version = 0;
            u32 formatInt = 0;
            u32 width = 0;
            u32 height = 0;
            u32 flags = 0;
            u32 mipCount = 0;
            if (!ReadU32(blob, cursor, version) || !ReadU32(blob, cursor, formatInt) ||
                !ReadU32(blob, cursor, width) || !ReadU32(blob, cursor, height) ||
                !ReadU32(blob, cursor, flags) || !ReadU32(blob, cursor, mipCount))
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - header truncated");
                return false;
            }

            if (version != kOloTexVersion)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - unsupported version {}", version);
                return false;
            }
            if (formatInt != static_cast<u32>(TextureCompressionFormat::BC7) &&
                formatInt != static_cast<u32>(TextureCompressionFormat::BC5))
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - unknown format {}", formatInt);
                return false;
            }
            if (width == 0 || height == 0 || mipCount == 0)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - degenerate dimensions/mips");
                return false;
            }
            // Bound header fields BEFORE any allocation: a hostile/corrupt .olotex must
            // not drive an OOM (unbounded Mips.reserve) or an over-long mip chain that
            // later trips glTextureStorage2D. width/height are capped, and mipCount can't
            // exceed the chain length the dimensions allow.
            if (width > kMaxTextureDimension || height > kMaxTextureDimension)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - dimensions {}x{} exceed max {}", width, height, kMaxTextureDimension);
                return false;
            }
            if (mipCount > MaxMipLevels(width, height))
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - mipCount {} exceeds max {} for {}x{}",
                               mipCount, MaxMipLevels(width, height), width, height);
                return false;
            }

            CompressedTextureImage image;
            image.Format = static_cast<TextureCompressionFormat>(formatInt);
            image.Width = width;
            image.Height = height;
            image.SRGB = (flags & kFlagSRGB) != 0;
            image.HasAlpha = (flags & kFlagHasAlpha) != 0;
            image.Mips.reserve(mipCount); // now bounded by MaxMipLevels above

            for (u32 i = 0; i < mipCount; ++i)
            {
                u32 mipSize = 0;
                if (!ReadU32(blob, cursor, mipSize))
                {
                    OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - mip {} size truncated", i);
                    return false;
                }
                if (cursor + mipSize > blob.size())
                {
                    OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - mip {} data truncated", i);
                    return false;
                }
                const u32 mw = std::max(1u, width >> i);
                const u32 mh = std::max(1u, height >> i);
                if (mipSize != MipByteSize(image.Format, mw, mh))
                {
                    OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - mip {} size mismatch (got {}, expected {})",
                                   i, mipSize, MipByteSize(image.Format, mw, mh));
                    return false;
                }
                image.Mips.emplace_back(blob.begin() + cursor, blob.begin() + cursor + mipSize);
                cursor += mipSize;
            }

            out = std::move(image);
            return true;
        }

        bool WriteFile(const std::string& path, const CompressedTextureImage& image)
        {
            const std::vector<u8> blob = SerializeToBlob(image);
            if (blob.empty())
            {
                OLO_CORE_ERROR("TextureCompression::WriteFile - nothing to write for '{}'", path);
                return false;
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                OLO_CORE_ERROR("TextureCompression::WriteFile - cannot open '{}'", path);
                return false;
            }
            file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
            return static_cast<bool>(file);
        }

        bool ReadFile(const std::string& path, CompressedTextureImage& out)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                OLO_CORE_ERROR("TextureCompression::ReadFile - cannot open '{}'", path);
                return false;
            }
            const std::streamsize size = file.tellg();
            if (size <= 0)
            {
                OLO_CORE_ERROR("TextureCompression::ReadFile - empty file '{}'", path);
                return false;
            }
            std::vector<u8> blob(static_cast<sizet>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(blob.data()), size);
            if (!file)
            {
                OLO_CORE_ERROR("TextureCompression::ReadFile - short read '{}'", path);
                return false;
            }
            return DeserializeFromBlob(blob, out);
        }

        bool CompressTextureFile(const std::string& srcImagePath, const std::string& dstOlotexPath,
                                 const CompressOptions& options)
        {
            OLO_PROFILE_FUNCTION();

            TextureCompressionFormat format = options.Format;
            if (format == TextureCompressionFormat::None)
                format = TextureCompressionFormat::BC7;

            bool srgb = options.SRGB;
            if (options.AutoSRGBFromName && format == TextureCompressionFormat::BC7)
            {
                const std::string filename = std::filesystem::path(srcImagePath).filename().string();
                srgb = IsLikelyColorTexture(filename);
            }

            // Match the runtime texture loader's vertical flip so the stored blocks
            // upload without re-flipping (see OpenGLTexture2D path-load ctor).
            ::stbi_set_flip_vertically_on_load_thread(1);
            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_uc* data = ::stbi_load(srcImagePath.c_str(), &width, &height, &channels, 0);
            ::stbi_set_flip_vertically_on_load_thread(0);

            if (!data)
            {
                OLO_CORE_ERROR("TextureCompression::CompressTextureFile - failed to load '{}'", srcImagePath);
                return false;
            }

            CompressedTextureImage image;
            if (format == TextureCompressionFormat::BC5)
                image = EncodeBC5(data, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(channels), options.GenerateMips);
            else
                image = EncodeBC7(data, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(channels), srgb, options.GenerateMips);

            ::stbi_image_free(data);

            if (!image.IsValid())
            {
                OLO_CORE_ERROR("TextureCompression::CompressTextureFile - encode failed for '{}'", srcImagePath);
                return false;
            }

            return WriteFile(dstOlotexPath, image);
        }
    } // namespace TextureCompression
} // namespace OloEngine
