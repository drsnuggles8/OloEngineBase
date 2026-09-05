#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/BC6HEncoder.h"
#include "OloEngine/Renderer/TextureImportSettings.h"

// Vendored encoders/decoders (bc7enc_rdo, MIT / public domain). Only this TU pulls
// them in, keeping the header renderer-agnostic. bc7enc: BC7 encode; rgbcx: BC5
// encode + decode; bc7decomp: BC7 decode.
#include <bc7enc.h>
#include <bc7decomp.h>
#include <rgbcx.h>

// bcdec: independent single-header BC6H reference decoder (used to validate our
// from-scratch BC6H encoder and as the CPU HDR fallback). The single-TU implementation
// is emitted here; the SYSTEM include suppresses its warnings, as with stb below.
#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

// stb_image is only *declared* here — STB_IMAGE_IMPLEMENTATION lives in
// Platform/OpenGL/OpenGLTexture.cpp, which provides the definitions at link time.
#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <limits>
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

        // sRGB transfer function, both directions (IEC 61966-2-1). Only used to build a
        // mip chain in linear light; the stored texels stay sRGB-encoded either way.
        f32 SRGBToLinear(f32 encoded)
        {
            return encoded <= 0.04045f ? (encoded / 12.92f) : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        f32 LinearToSRGB(f32 linear)
        {
            return linear <= 0.0031308f ? (linear * 12.92f) : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
        }

        // 256-entry sRGB -> linear table. Building the table once and indexing it turns
        // the per-texel std::pow into a load; the inverse still needs the real function
        // because its input is continuous.
        const std::array<f32, 256>& SRGBDecodeTable()
        {
            static const std::array<f32, 256> table = []
            {
                std::array<f32, 256> values{};
                for (u32 i = 0; i < 256; ++i)
                    values[i] = SRGBToLinear(static_cast<f32>(i) / 255.0f);
                return values;
            }();
            return table;
        }

        // Box-filter downsample an RGBA8 image to half size (each dim halved, min 1).
        //
        // `srgb` selects a GAMMA-CORRECT reduction (#624 item 4): the four source texels
        // are decoded to linear light, averaged there, and re-encoded. Averaging sRGB code
        // values directly — which is what glGenerateMipmap does by default, and what this
        // cook did before — is averaging the wrong quantity and comes out too dark: a
        // 50/50 black-and-white checkerboard reduces to 128/255 instead of the 188/255
        // that actually carries half the light. The error is largest exactly where mip
        // chains are most visible, on high-contrast albedo at a distance.
        //
        // Alpha is NEVER gamma-decoded — it is coverage, not light, and is linear in the
        // stored space already. `outW`/`outH` receive the reduced dimensions.
        std::vector<u8> DownsampleRGBA8(const std::vector<u8>& src, u32 width, u32 height, bool srgb,
                                        u32& outW, u32& outH)
        {
            outW = std::max(1u, width / 2);
            outH = std::max(1u, height / 2);
            std::vector<u8> dst(static_cast<sizet>(outW) * outH * 4);
            const std::array<f32, 256>& decode = SRGBDecodeTable();

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
                        if (srgb && c < 3)
                        {
                            const f32 average = (decode[p00[c]] + decode[p01[c]] + decode[p10[c]] + decode[p11[c]]) * 0.25f;
                            const f32 encoded = std::clamp(LinearToSRGB(average), 0.0f, 1.0f);
                            d[c] = static_cast<u8>(std::lround(encoded * 255.0f));
                        }
                        else
                        {
                            const u32 sum = static_cast<u32>(p00[c]) + p01[c] + p10[c] + p11[c];
                            d[c] = static_cast<u8>((sum + 2) / 4);
                        }
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
        // `encodeBlock(dst, block64)` fills `blockBytes` output bytes from a 64-byte RGBA
        // block. `blockBytes` is 16 for BC5/BC7 and 8 for BC4, which stores one channel.
        template<typename EncodeBlockFn>
        std::vector<u8> EncodeLevel(const std::vector<u8>& rgba, u32 width, u32 height, EncodeBlockFn&& encodeBlock,
                                    u32 blockBytes = 16)
        {
            const u32 bx = TextureCompression::BlockCount(width);
            const u32 by = TextureCompression::BlockCount(height);
            std::vector<u8> out(static_cast<sizet>(bx) * by * blockBytes);

            std::array<u8, 64> block{};
            for (u32 y = 0; y < by; ++y)
            {
                for (u32 x = 0; x < bx; ++x)
                {
                    GatherBlockRGBA(rgba, width, height, x, y, block);
                    u8* dst = out.data() + (static_cast<sizet>(y) * bx + x) * blockBytes;
                    encodeBlock(dst, block.data());
                }
            }
            return out;
        }

        // Build a compressed mip chain from a level-0 pixel buffer. `encodeLevel(level, w, h)`
        // returns the BCn blocks for one level; `downsample(level, w, h, outW, outH)` halves
        // it. Shared by all three encoders (BC7/BC5 over RGBA8, BC6H over RGB float): the
        // loop, the !generateMips / 1x1 termination, and the dimension bookkeeping are
        // identical — only the pixel type and the two callables differ.
        template<typename Pixel, typename EncodeLevelFn, typename DownsampleFn>
        std::vector<std::vector<u8>> BuildMipChain(std::vector<Pixel> level, u32 width, u32 height, bool generateMips,
                                                   EncodeLevelFn&& encodeLevel, DownsampleFn&& downsample)
        {
            std::vector<std::vector<u8>> mips;
            u32 mw = width;
            u32 mh = height;
            while (true)
            {
                mips.push_back(encodeLevel(level, mw, mh));
                if (!generateMips || (mw == 1 && mh == 1))
                    break;
                u32 nw = 0;
                u32 nh = 0;
                level = downsample(level, mw, mh, nw, nh);
                mw = nw;
                mh = nh;
            }
            return mips;
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

        // Upper bound on a legitimate serialized .olotex payload, used to reject an
        // oversized/corrupt file before allocating a read buffer from its reported size.
        // Worst case is BC7/BC5 (1 byte/texel) at the max dimension with a full mip chain
        // (<4/3 x the base level); 2 x W x H plus a small header slop covers it generously.
        constexpr sizet kMaxSerializedBlobSize =
            2ull * kMaxTextureDimension * kMaxTextureDimension + 4096ull;

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

        // ---- BC6H cook helpers ------------------------------------------------
        // The per-block BC6H encoder lives in BC6HEncoder.cpp; everything here is the
        // image-level plumbing around it (expand to RGB float, downsample, gather).

        // Expand tightly-packed `channels`-per-texel float source to RGB (3 floats/texel).
        // channels>=3 keeps R,G,B (extra dropped); 2 -> R,G,0; 1 -> R,R,R.
        std::vector<f32> ExpandToRGBFloat(const f32* pixels, u32 width, u32 height, u32 channels)
        {
            const sizet texelCount = static_cast<sizet>(width) * height;
            std::vector<f32> rgb(texelCount * 3);
            for (sizet i = 0; i < texelCount; ++i)
            {
                const f32* src = pixels + i * channels;
                f32* dst = rgb.data() + i * 3;
                dst[0] = src[0];
                dst[1] = channels >= 2 ? src[1] : src[0];
                dst[2] = channels >= 3 ? src[2] : (channels == 1 ? src[0] : 0.0f);
            }
            return rgb;
        }

        // Box-filter downsample an RGB-float image to half size. HDR data is already
        // linear, so a plain average is correct (unlike the gamma-naive 8-bit path).
        std::vector<f32> DownsampleRGBFloat(const std::vector<f32>& src, u32 width, u32 height, u32& outW, u32& outH)
        {
            outW = std::max(1u, width / 2);
            outH = std::max(1u, height / 2);
            std::vector<f32> dst(static_cast<sizet>(outW) * outH * 3);
            for (u32 y = 0; y < outH; ++y)
            {
                const u32 sy0 = std::min(y * 2, height - 1);
                const u32 sy1 = std::min(y * 2 + 1, height - 1);
                for (u32 x = 0; x < outW; ++x)
                {
                    const u32 sx0 = std::min(x * 2, width - 1);
                    const u32 sx1 = std::min(x * 2 + 1, width - 1);
                    const f32* p00 = &src[(static_cast<sizet>(sy0) * width + sx0) * 3];
                    const f32* p01 = &src[(static_cast<sizet>(sy0) * width + sx1) * 3];
                    const f32* p10 = &src[(static_cast<sizet>(sy1) * width + sx0) * 3];
                    const f32* p11 = &src[(static_cast<sizet>(sy1) * width + sx1) * 3];
                    f32* d = &dst[(static_cast<sizet>(y) * outW + x) * 3];
                    for (u32 c = 0; c < 3; ++c)
                        d[c] = (p00[c] + p01[c] + p10[c] + p11[c]) * 0.25f;
                }
            }
            return dst;
        }

        // Gather the 4x4 block at (blockX, blockY) from an RGB-float image into 48
        // contiguous floats (16 texel x RGB), clamping to the edge for partial blocks.
        void GatherBlockRGBFloat(const std::vector<f32>& rgb, u32 width, u32 height, u32 blockX, u32 blockY,
                                 std::array<f32, 48>& outBlock)
        {
            for (u32 ry = 0; ry < 4; ++ry)
            {
                const u32 sy = std::min(blockY * 4 + ry, height - 1);
                for (u32 rx = 0; rx < 4; ++rx)
                {
                    const u32 sx = std::min(blockX * 4 + rx, width - 1);
                    const f32* src = &rgb[(static_cast<sizet>(sy) * width + sx) * 3];
                    f32* dst = &outBlock[(static_cast<sizet>(ry) * 4 + rx) * 3];
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
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
                case TextureCompressionFormat::BC6H:
                case TextureCompressionFormat::BC6HSigned:
                    return 16;
                case TextureCompressionFormat::BC4:
                    return 8; // one channel, half a block
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

            image.Mips = BuildMipChain<u8>(
                ExpandToRGBA8(pixels, width, height, channels), width, height, generateMips,
                [&encodeBlock](const std::vector<u8>& lvl, u32 w, u32 h)
                { return EncodeLevel(lvl, w, h, encodeBlock); },
                [srgb](const std::vector<u8>& s, u32 w, u32 h, u32& ow, u32& oh)
                { return DownsampleRGBA8(s, w, h, srgb, ow, oh); });
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

            image.Mips = BuildMipChain<u8>(
                ExpandToRGBA8(pixels, width, height, channels), width, height, generateMips,
                [&encodeBlock](const std::vector<u8>& lvl, u32 w, u32 h)
                { return EncodeLevel(lvl, w, h, encodeBlock); },
                [](const std::vector<u8>& s, u32 w, u32 h, u32& ow, u32& oh)
                { return DownsampleRGBA8(s, w, h, /*srgb*/ false, ow, oh); });
            return image;
        }

        // The GPU encode hook and its counters. Plain atomics rather than a mutex:
        // the hook is set once by the renderer and read once per mip level.
        std::atomic<Bc6hGpuEncodeFn> s_GpuBC6HEncoder{ nullptr };
        std::atomic<u64> s_Bc6hGpuLevels{ 0 };
        std::atomic<u64> s_Bc6hCpuLevels{ 0 };

        void SetGpuBC6HEncoder(Bc6hGpuEncodeFn encoder)
        {
            s_GpuBC6HEncoder.store(encoder, std::memory_order_relaxed);
        }

        bool HasGpuBC6HEncoder()
        {
            return s_GpuBC6HEncoder.load(std::memory_order_relaxed) != nullptr;
        }

        Bc6hEncodeCounts GetBC6HEncodeCounts()
        {
            return { s_Bc6hGpuLevels.load(std::memory_order_relaxed),
                     s_Bc6hCpuLevels.load(std::memory_order_relaxed) };
        }

        void ResetBC6HEncodeCounts()
        {
            s_Bc6hGpuLevels.store(0, std::memory_order_relaxed);
            s_Bc6hCpuLevels.store(0, std::memory_order_relaxed);
        }

        CompressedTextureImage EncodeBC4(const u8* pixels, u32 width, u32 height, u32 channels, bool generateMips)
        {
            OLO_PROFILE_FUNCTION();

            CompressedTextureImage image;
            if (!pixels || width == 0 || height == 0 || channels == 0 || channels > 4)
            {
                OLO_CORE_ERROR("TextureCompression::EncodeBC4 - invalid input ({}x{}, {} ch)", width, height, channels);
                return image;
            }

            EnsureEncodersInitialized();

            // rgbcx::encode_bc4 reads channel 0 from a 4-byte-stride buffer.
            const auto encodeBlock = [](u8* dst, const u8* block64)
            {
                ::rgbcx::encode_bc4(dst, block64, 4);
            };

            image.Format = TextureCompressionFormat::BC4;
            image.Width = width;
            image.Height = height;
            image.SRGB = false; // single-channel data is linear by construction

            image.Mips = BuildMipChain<u8>(
                ExpandToRGBA8(pixels, width, height, channels), width, height, generateMips,
                [&encodeBlock](const std::vector<u8>& lvl, u32 w, u32 h)
                { return EncodeLevel(lvl, w, h, encodeBlock, 8); },
                [](const std::vector<u8>& s, u32 w, u32 h, u32& ow, u32& oh)
                { return DownsampleRGBA8(s, w, h, /*srgb*/ false, ow, oh); });
            return image;
        }

        ChannelUsage AnalyzeChannels(const u8* pixels, u32 width, u32 height, u32 channels)
        {
            ChannelUsage usage;
            if (!pixels || width == 0 || height == 0 || channels == 0 || channels > 4)
                return usage;

            const std::vector<u8> rgba = ExpandToRGBA8(pixels, width, height, channels);
            const sizet texelCount = static_cast<sizet>(width) * height;

            std::array<u8, 4> first{ rgba[0], rgba[1], rgba[2], rgba[3] };
            std::array<bool, 4> varies{ false, false, false, false };
            bool greyscale = true;
            for (sizet i = 0; i < texelCount; ++i)
            {
                const u8* texel = &rgba[i * 4];
                for (u32 c = 0; c < 4; ++c)
                {
                    if (texel[c] != first[c])
                        varies[c] = true;
                }
                if (texel[0] != texel[1] || texel[1] != texel[2])
                    greyscale = false;
            }

            // "Has alpha" must mean NOT OPAQUE, not merely "alpha is non-constant": a
            // uniformly translucent source has a constant alpha that is still alpha, and
            // calling it opaque would both mis-sort it AND let the greyscale rule narrow
            // it to BC4, which drops the channel for good. ExpandToRGBA8 writes a
            // constant 255 for a 1/3-channel source, so those stay opaque either way.
            usage.HasAlpha = varies[3] || first[3] != 255;
            usage.IsGreyscale = greyscale;
            for (u32 c = 0; c < 4; ++c)
                usage.VaryingChannels += varies[c] ? 1u : 0u;
            return usage;
        }

        bool HasNegativeComponents(const f32* pixels, u32 width, u32 height, u32 channels)
        {
            if (!pixels || width == 0 || height == 0 || channels == 0)
                return false;
            const sizet texelCount = static_cast<sizet>(width) * height;
            const u32 rgbChannels = std::min(channels, 3u);
            for (sizet i = 0; i < texelCount; ++i)
            {
                const f32* texel = pixels + i * channels;
                for (u32 c = 0; c < rgbChannels; ++c)
                {
                    // A non-finite component is clamped by the encoder in both variants,
                    // so it must not be what forces the signed one.
                    if (std::isfinite(texel[c]) && texel[c] < 0.0f)
                        return true;
                }
            }
            return false;
        }

        CompressedTextureImage EncodeBC6H(const f32* pixels, u32 width, u32 height, u32 channels,
                                          bool isSigned, bool generateMips)
        {
            OLO_PROFILE_FUNCTION();

            CompressedTextureImage image;
            // BC6H is an HDR RGB format; require at least three channels of float source.
            if (!pixels || width == 0 || height == 0 || channels < 3 || channels > 4)
            {
                OLO_CORE_ERROR("TextureCompression::EncodeBC6H - invalid input ({}x{}, {} ch; needs 3-4 channels)", width, height, channels);
                return image;
            }

            image.Format = isSigned ? TextureCompressionFormat::BC6HSigned : TextureCompressionFormat::BC6H;
            image.Width = width;
            image.Height = height;
            image.SRGB = false;     // HDR is linear, never sRGB
            image.HasAlpha = false; // BC6H is RGB only

            const auto encodeLevel = [isSigned](const std::vector<f32>& rgb, u32 w, u32 h)
            {
                const u32 bx = BlockCount(w);
                const u32 by = BlockCount(h);

                // GPU fast path (#624 item 3), when the renderer registered one. The
                // shader runs the same mode search as BC6H::EncodeBlock below; a
                // failure here is a speed regression, not a quality one, so it warns
                // and continues on the CPU rather than failing the cook.
                if (const Bc6hGpuEncodeFn gpu = s_GpuBC6HEncoder.load(std::memory_order_relaxed))
                {
                    std::vector<u8> gpuOut;
                    if (gpu(rgb.data(), w, h, isSigned, gpuOut) &&
                        gpuOut.size() == static_cast<sizet>(bx) * by * 16)
                    {
                        s_Bc6hGpuLevels.fetch_add(1, std::memory_order_relaxed);
                        return gpuOut;
                    }
                    OLO_CORE_WARN("TextureCompression::EncodeBC6H - GPU encode of the {}x{} level failed; "
                                  "falling back to the CPU encoder for it",
                                  w, h);
                }

                std::vector<u8> out(static_cast<sizet>(bx) * by * 16);
                std::array<f32, 48> block{};
                for (u32 y = 0; y < by; ++y)
                {
                    for (u32 x = 0; x < bx; ++x)
                    {
                        GatherBlockRGBFloat(rgb, w, h, x, y, block);
                        BC6H::EncodeBlock(out.data() + (static_cast<sizet>(y) * bx + x) * 16, block.data(), isSigned);
                    }
                }
                s_Bc6hCpuLevels.fetch_add(1, std::memory_order_relaxed);
                return out;
            };

            image.Mips = BuildMipChain<f32>(
                ExpandToRGBFloat(pixels, width, height, channels), width, height, generateMips,
                encodeLevel,
                [](const std::vector<f32>& s, u32 w, u32 h, u32& ow, u32& oh)
                { return DownsampleRGBFloat(s, w, h, ow, oh); });
            return image;
        }

        bool DecodeToRGBA8(const CompressedTextureImage& image, u32 mipLevel,
                           std::vector<u8>& outRGBA8, u32& outWidth, u32& outHeight)
        {
            if (!image.IsValid() || mipLevel >= image.MipLevels())
                return false;
            // BC6H is HDR (half-float); it has no meaningful 8-bit representation. Callers
            // that need its pixels use DecodeToRGBAFloat instead.
            if (IsBC6H(image.Format))
            {
                OLO_CORE_ERROR("TextureCompression::DecodeToRGBA8 - BC6H is HDR; use DecodeToRGBAFloat");
                return false;
            }

            const u32 mw = std::max(1u, image.Width >> mipLevel);
            const u32 mh = std::max(1u, image.Height >> mipLevel);
            outWidth = mw;
            outHeight = mh;
            outRGBA8.assign(static_cast<sizet>(mw) * mh * 4, 0);

            const std::vector<u8>& blocks = image.Mips[mipLevel];
            const u32 bxCount = BlockCount(mw);
            const u32 byCount = BlockCount(mh);
            // BC4 is 8 bytes per block, everything else 16 — never assume the stride.
            const u32 blockBytes = BlockSizeBytes(image.Format);
            if (blocks.size() < static_cast<sizet>(bxCount) * byCount * blockBytes)
            {
                OLO_CORE_ERROR("TextureCompression::DecodeToRGBA8 - mip {} block data truncated", mipLevel);
                return false;
            }

            std::array<u8, 64> decoded{};
            for (u32 by = 0; by < byCount; ++by)
            {
                for (u32 bx = 0; bx < bxCount; ++bx)
                {
                    const u8* blockPtr = blocks.data() + (static_cast<sizet>(by) * bxCount + bx) * blockBytes;
                    if (image.Format == TextureCompressionFormat::BC7)
                    {
                        // bc7decomp writes 16 color_rgba (RGBA) contiguously.
                        static_assert(sizeof(::bc7decomp::color_rgba) == 4, "color_rgba must be 4 bytes");
                        ::bc7decomp::unpack_bc7(blockPtr, reinterpret_cast<::bc7decomp::color_rgba*>(decoded.data()));
                    }
                    else if (image.Format == TextureCompressionFormat::BC4)
                    {
                        decoded.fill(0);
                        ::rgbcx::unpack_bc4(blockPtr, decoded.data(), 4);
                        // (R, R, R, 1) — the same swizzle the GPU upload installs, so a
                        // BC4 texture reads identically whichever path decoded it.
                        for (u32 i = 0; i < 16; ++i)
                        {
                            decoded[i * 4 + 1] = decoded[i * 4 + 0];
                            decoded[i * 4 + 2] = decoded[i * 4 + 0];
                            decoded[i * 4 + 3] = 255;
                        }
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

        bool DecodeToRGBAFloat(const CompressedTextureImage& image, u32 mipLevel,
                               std::vector<f32>& outRGBA, u32& outWidth, u32& outHeight)
        {
            if (!image.IsValid() || !IsBC6H(image.Format) || mipLevel >= image.MipLevels())
                return false;
            const int bcdecSigned = image.Format == TextureCompressionFormat::BC6HSigned ? 1 : 0;

            const u32 mw = std::max(1u, image.Width >> mipLevel);
            const u32 mh = std::max(1u, image.Height >> mipLevel);
            outWidth = mw;
            outHeight = mh;
            outRGBA.assign(static_cast<sizet>(mw) * mh * 4, 0.0f);

            const std::vector<u8>& blocks = image.Mips[mipLevel];
            const u32 bxCount = BlockCount(mw);
            const u32 byCount = BlockCount(mh);
            if (blocks.size() < static_cast<sizet>(bxCount) * byCount * 16)
            {
                OLO_CORE_ERROR("TextureCompression::DecodeToRGBAFloat - mip {} block data truncated", mipLevel);
                return false;
            }

            // bcdec writes a 4x4 block of RGB floats (row pitch in float elements = 4*3).
            std::array<f32, 16 * 3> decoded{};
            for (u32 by = 0; by < byCount; ++by)
            {
                for (u32 bx = 0; bx < bxCount; ++bx)
                {
                    const u8* blockPtr = blocks.data() + (static_cast<sizet>(by) * bxCount + bx) * 16;
                    ::bcdec_bc6h_float(blockPtr, decoded.data(), 4 * 3, bcdecSigned);

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
                            const f32* s = &decoded[(static_cast<sizet>(ry) * 4 + rx) * 3];
                            f32* d = &outRGBA[(static_cast<sizet>(dy) * mw + dx) * 4];
                            d[0] = s[0];
                            d[1] = s[1];
                            d[2] = s[2];
                            d[3] = 1.0f;
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
                formatInt != static_cast<u32>(TextureCompressionFormat::BC5) &&
                formatInt != static_cast<u32>(TextureCompressionFormat::BC6H) &&
                formatInt != static_cast<u32>(TextureCompressionFormat::BC6HSigned) &&
                formatInt != static_cast<u32>(TextureCompressionFormat::BC4))
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - unknown format {}", formatInt);
                return false;
            }
            if (width == 0 || height == 0 || mipCount == 0)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - degenerate dimensions/mips");
                return false;
            }
            // Reject non-canonical metadata: only the two defined flag bits may be set,
            // and only BC7 may carry sRGB or alpha. BC5 (two-channel normal data) and
            // BC6H (linear HDR RGB) can carry neither — a blob claiming otherwise is
            // corrupt or version-skewed, not something we produced.
            if ((flags & ~(kFlagSRGB | kFlagHasAlpha)) != 0)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - unknown flag bits set ({:#x})", flags);
                return false;
            }
            const auto blobFormat = static_cast<TextureCompressionFormat>(formatInt);
            if ((blobFormat == TextureCompressionFormat::BC5 || blobFormat == TextureCompressionFormat::BC4 ||
                 IsBC6H(blobFormat)) &&
                (flags & (kFlagSRGB | kFlagHasAlpha)) != 0)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - {} must not set sRGB/alpha flags ({:#x})",
                               blobFormat == TextureCompressionFormat::BC5   ? "BC5"
                               : blobFormat == TextureCompressionFormat::BC4 ? "BC4"
                                                                             : "BC6H",
                               flags);
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

            // The blob must be fully consumed: trailing bytes mean a malformed or
            // version-skewed payload that merely happened to parse up to the last mip.
            if (cursor != blob.size())
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - {} trailing byte(s) after final mip", blob.size() - cursor);
                return false;
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
            // Reject an implausibly large file BEFORE allocating from its size — a corrupt
            // or hostile .olotex must not drive a multi-gigabyte allocation off tellg().
            if (static_cast<u64>(size) > kMaxSerializedBlobSize)
            {
                OLO_CORE_ERROR("TextureCompression::ReadFile - file '{}' size {} exceeds max serialized size {}",
                               path, static_cast<u64>(size), kMaxSerializedBlobSize);
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

        bool CompressImageFile(const std::string& srcImagePath, const CompressOptions& options, CompressedTextureImage& out)
        {
            OLO_PROFILE_FUNCTION();

            TextureCompressionFormat format = options.Format;
            bool srgb = options.SRGB;
            bool autoSRGBFromName = options.AutoSRGBFromName;
            bool generateMips = options.GenerateMips;

            // A "<image>.oloimport" sidecar is the only reliable per-texture signal the
            // cook has (#624 item 2). It is what makes BC5 reachable automatically: no
            // property of the pixels separates a two-channel tangent-space normal from a
            // roughness or AO map, so guessing would silently drop a channel from
            // someone's data.
            //
            // Precedence, and it differs per field because only one of them has a way to
            // spell "unset": an explicit `Format` beats the sidecar (None means auto),
            // while `ColorSpace` and `GenerateMips` beat the caller's, because a `false`
            // in CompressOptions is indistinguishable from a default. A caller that must
            // have exactly what it passed sets UseImportSettings = false.
            if (options.UseImportSettings)
            {
                if (TextureImportSettings settings; TextureImport::LoadForImage(srcImagePath, settings))
                {
                    if (format == TextureCompressionFormat::None)
                    {
                        switch (settings.Format)
                        {
                            case TextureImportSettings::FormatChoice::BC7:
                                format = TextureCompressionFormat::BC7;
                                break;
                            case TextureImportSettings::FormatChoice::BC5:
                                format = TextureCompressionFormat::BC5;
                                break;
                            case TextureImportSettings::FormatChoice::BC4:
                                format = TextureCompressionFormat::BC4;
                                break;
                            case TextureImportSettings::FormatChoice::BC6H:
                                format = TextureCompressionFormat::BC6H;
                                break;
                            case TextureImportSettings::FormatChoice::BC6HSigned:
                                format = TextureCompressionFormat::BC6HSigned;
                                break;
                            case TextureImportSettings::FormatChoice::Auto:
                                break;
                        }
                    }
                    if (settings.ColorSpace != TextureImportSettings::ColorSpaceChoice::Auto)
                    {
                        srgb = settings.ColorSpace == TextureImportSettings::ColorSpaceChoice::SRGB;
                        autoSRGBFromName = false;
                    }
                    if (settings.GenerateMips.has_value())
                        generateMips = *settings.GenerateMips;
                }
            }

            // Auto: an HDR source (.hdr / .exr with float data) becomes BC6H, everything
            // else BC7. Whether that BC6H is the signed variant, and whether an LDR source
            // narrows to BC4, are both decided from the decoded pixels below.
            const bool autoSelectFormat = format == TextureCompressionFormat::None;
            bool autoSelectBC6HVariant = false;
            if (format == TextureCompressionFormat::None)
            {
                const bool isHDR = ::stbi_is_hdr(srcImagePath.c_str()) != 0;
                format = isHDR ? TextureCompressionFormat::BC6H : TextureCompressionFormat::BC7;
                autoSelectBC6HVariant = isHDR;
            }

            if (autoSRGBFromName && format == TextureCompressionFormat::BC7)
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
            CompressedTextureImage image;

            if (IsBC6H(format))
            {
                // Load HDR as float, forcing 3 components (RGB) so a 1/4-channel HDR source
                // still feeds EncodeBC6H a well-defined RGB buffer.
                f32* data = ::stbi_loadf(srcImagePath.c_str(), &width, &height, &channels, 3);
                ::stbi_set_flip_vertically_on_load_thread(0);
                if (!data)
                {
                    OLO_CORE_ERROR("TextureCompression::CompressImageFile - failed to load HDR '{}'", srcImagePath);
                    return false;
                }
                // Signed BC6H costs nothing in size but one bit of endpoint magnitude, so
                // it is only chosen when the data actually needs it: any negative
                // component in the decoded pixels. An explicitly requested variant is
                // honoured as-is, including "unsigned, and clamp my negatives away".
                bool isSigned = format == TextureCompressionFormat::BC6HSigned;
                if (autoSelectBC6HVariant)
                    isSigned = HasNegativeComponents(data, static_cast<u32>(width), static_cast<u32>(height), 3);
                image = EncodeBC6H(data, static_cast<u32>(width), static_cast<u32>(height), 3, isSigned, generateMips);
                ::stbi_image_free(data);
            }
            else
            {
                stbi_uc* data = ::stbi_load(srcImagePath.c_str(), &width, &height, &channels, 0);
                ::stbi_set_flip_vertically_on_load_thread(0);
                if (!data)
                {
                    OLO_CORE_ERROR("TextureCompression::CompressImageFile - failed to load '{}'", srcImagePath);
                    return false;
                }
                const u32 texelWidth = static_cast<u32>(width);
                const u32 texelHeight = static_cast<u32>(height);
                const u32 sourceChannels = static_cast<u32>(channels);

                // Per-channel format selection (#624 item 5), from the PIXELS. A greyscale
                // source — one whose R, G and B are equal at every texel, which includes
                // every 1-channel file — was previously widened to R,R,R,255 and stored as
                // BC7 at 16 bytes per block. BC4 stores exactly the one channel that
                // carries anything, in 8, and this engine samples BC4 as (R,R,R,1), so
                // nothing downstream can tell the difference.
                //
                // This is safe to do automatically precisely because it is a measurement:
                // no channel that carries information is dropped. The neighbouring case
                // that ISN'T safe, and so is left to the sidecar, is narrowing an RGB
                // source to two channels — BC5 decodes blue as 0, which is a change unless
                // the source's blue happened to be 0 too.
                const ChannelUsage usage = AnalyzeChannels(data, texelWidth, texelHeight, sourceChannels);
                if (autoSelectFormat && !usage.HasAlpha && usage.IsGreyscale && !srgb)
                    format = TextureCompressionFormat::BC4;

                if (format == TextureCompressionFormat::BC5)
                    image = EncodeBC5(data, texelWidth, texelHeight, sourceChannels, generateMips);
                else if (format == TextureCompressionFormat::BC4)
                    image = EncodeBC4(data, texelWidth, texelHeight, sourceChannels, generateMips);
                else
                {
                    image = EncodeBC7(data, texelWidth, texelHeight, sourceChannels, srgb, generateMips);
                    // Alpha is MEASURED, not inferred from the channel count: a 4-channel
                    // PNG whose alpha is a constant 255 is opaque, and reporting it as
                    // transparent puts an opaque albedo in the transparent render pass.
                    image.HasAlpha = usage.HasAlpha;
                }
                ::stbi_image_free(data);
            }

            if (!image.IsValid())
            {
                OLO_CORE_ERROR("TextureCompression::CompressImageFile - encode failed for '{}'", srcImagePath);
                return false;
            }

            out = std::move(image);
            return true;
        }

        bool CompressTextureFile(const std::string& srcImagePath, const std::string& dstOlotexPath,
                                 const CompressOptions& options)
        {
            OLO_PROFILE_FUNCTION();

            CompressedTextureImage image;
            if (!CompressImageFile(srcImagePath, options, image))
                return false;

            return WriteFile(dstOlotexPath, image);
        }
    } // namespace TextureCompression
} // namespace OloEngine
