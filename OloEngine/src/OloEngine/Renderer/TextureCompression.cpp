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

// bcdec: independent single-header BC6H reference decoder (used to validate our
// from-scratch BC6H encoder and as the CPU HDR fallback). The single-TU implementation
// is emitted here; the SYSTEM include suppresses its warnings, as with stb below.
#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

// stb_image is only *declared* here — STB_IMAGE_IMPLEMENTATION lives in
// Platform/OpenGL/OpenGLTexture.cpp, which provides the definitions at link time.
#include <stb_image/stb_image.h>

// glm::packHalf1x16 — float -> IEEE half bit pattern, used to build BC6H encode targets.
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

        // ---- BC6H (unsigned, mode 11) cook helpers ---------------------------
        // BC6H packs RGB half-float into 16-byte 4x4 blocks. We emit only "mode 11":
        // 1 subset, two 10-bit endpoints stored raw (no delta), 4-bit interpolation
        // indices. The decode chain (matched to the vendored bcdec reference, which the
        // round-trip test cross-checks) is: unquantize each 10-bit endpoint component to
        // 16-bit, interpolate with the 4-bit index, then scale by 31/64 to get the
        // half-float bit pattern. The encoder inverts that chain.

        // BC6H 4-bit interpolation weights (index -> weight, /64). Exactly the bcdec /
        // D3D "aWeight4" table — note index 13 is 55, NOT 56 (the table is not perfectly
        // symmetric, which is why the anchor swap below re-selects indices instead of
        // just inverting them).
        constexpr std::array<i32, 16> kBC6HWeights4 = {
            0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64
        };

        constexpr i32 kBC6HEndpointMax = 1023; // 10-bit endpoint precision
        constexpr f32 kMaxFiniteHalf = 65504.0f;

        // Unsigned unquantize of a 10-bit endpoint component to 16-bit (matches
        // bcdec__unquantize with bits==10, isSigned==0).
        i32 Bc6hUnquantizeUnsigned(i32 comp)
        {
            if (comp <= 0)
                return 0;
            if (comp >= kBC6HEndpointMax)
                return 0xFFFF;
            return ((comp << 16) + 0x8000) >> 10;
        }

        // Nearest 10-bit endpoint whose unquantized value best matches `target16` (a
        // 16-bit interpolation-space value). unquantize is ~linear (comp*64), so start
        // from target/64 and check the 3-wide neighbourhood for the exact nearest.
        i32 Bc6hQuantizeEndpointUnsigned(i32 target16)
        {
            target16 = std::clamp(target16, 0, 0xFFFF);
            const i32 guess = std::clamp(target16 >> 6, 0, kBC6HEndpointMax);
            i32 best = guess;
            i32 bestErr = std::abs(Bc6hUnquantizeUnsigned(guess) - target16);
            for (i32 c = std::max(0, guess - 1); c <= std::min(kBC6HEndpointMax, guess + 1); ++c)
            {
                const i32 err = std::abs(Bc6hUnquantizeUnsigned(c) - target16);
                if (err < bestErr)
                {
                    bestErr = err;
                    best = c;
                }
            }
            return best;
        }

        // Interpolate two unquantized 16-bit endpoints with a 4-bit index (matches
        // bcdec__interpolate).
        i32 Bc6hInterpolate(i32 a, i32 b, i32 index)
        {
            return (a * (64 - kBC6HWeights4[index]) + b * kBC6HWeights4[index] + 32) >> 6;
        }

        // Convert a source HDR float to the 16-bit interpolation-space target the decoder
        // works in: clamp to non-negative (unsigned BC6H) and to the max finite half, take
        // its half-float bit pattern (the value finish_unquantize must reproduce), then
        // invert finish_unquantize ( half = (interp*31)>>6 ).
        i32 Bc6hFloatToInterpTarget(f32 value)
        {
            if (!std::isfinite(value))
                value = value > 0.0f ? kMaxFiniteHalf : 0.0f;
            value = std::clamp(value, 0.0f, kMaxFiniteHalf);
            const u32 halfBits = static_cast<u32>(::glm::packHalf1x16(value)) & 0xFFFFu; // -> [0, 0x7BFF]
            const i32 interp = static_cast<i32>((static_cast<i64>(halfBits) * 64 + 15) / 31);
            return std::clamp(interp, 0, 0xFFFF);
        }

        // Little-endian, LSB-first bit writer over a fixed 16-byte block.
        struct Bc6hBitWriter
        {
            std::array<u8, 16>& Data;
            u32 Pos = 0;
            void Put(u32 value, u32 bits)
            {
                for (u32 i = 0; i < bits; ++i)
                {
                    if ((value >> i) & 1u)
                        Data[Pos >> 3] |= static_cast<u8>(1u << (Pos & 7u));
                    ++Pos;
                }
            }
        };

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

        // Encode one 4x4 block of RGB floats (48 contiguous floats, row-major R,G,B) to a
        // 16-byte unsigned-BC6H mode-11 block.
        void EncodeBC6HBlockUnsigned(u8* dst, const f32* blockRGB)
        {
            // 1. Source -> interpolation-space targets (16 texels x RGB).
            std::array<std::array<i32, 3>, 16> target{};
            for (u32 t = 0; t < 16; ++t)
                for (u32 c = 0; c < 3; ++c)
                    target[t][c] = Bc6hFloatToInterpTarget(blockRGB[t * 3 + c]);

            // 2. Endpoints: bounding box, then tightened to the extreme projections along
            //    the box diagonal (a cheap PCA-lite that fits smooth HDR gradients well).
            std::array<i32, 3> bbMin = { 0xFFFF, 0xFFFF, 0xFFFF };
            std::array<i32, 3> bbMax = { 0, 0, 0 };
            for (const auto& tx : target)
                for (u32 c = 0; c < 3; ++c)
                {
                    bbMin[c] = std::min(bbMin[c], tx[c]);
                    bbMax[c] = std::max(bbMax[c], tx[c]);
                }
            const std::array<i64, 3> axis = { bbMax[0] - bbMin[0], bbMax[1] - bbMin[1], bbMax[2] - bbMin[2] };
            std::array<i32, 3> ep0 = bbMin;
            std::array<i32, 3> ep1 = bbMax;
            if (axis[0] != 0 || axis[1] != 0 || axis[2] != 0)
            {
                i64 minProj = std::numeric_limits<i64>::max();
                i64 maxProj = std::numeric_limits<i64>::min();
                u32 minTexel = 0;
                u32 maxTexel = 0;
                for (u32 t = 0; t < 16; ++t)
                {
                    i64 proj = 0;
                    for (u32 c = 0; c < 3; ++c)
                        proj += axis[c] * (target[t][c] - bbMin[c]);
                    if (proj < minProj)
                    {
                        minProj = proj;
                        minTexel = t;
                    }
                    if (proj > maxProj)
                    {
                        maxProj = proj;
                        maxTexel = t;
                    }
                }
                ep0 = target[minTexel];
                ep1 = target[maxTexel];
            }

            // 3. Per-texel index selection minimizing squared error across RGB (the index
            //    is shared by the three channels); returns the block's total error so the
            //    refinement loop can compare candidates. Reused by the anchor fixup.
            const auto selectIndices = [&](const std::array<i32, 3>& a, const std::array<i32, 3>& b,
                                           std::array<i32, 16>& out) -> i64
            {
                i64 total = 0;
                for (u32 t = 0; t < 16; ++t)
                {
                    i64 bestErr = std::numeric_limits<i64>::max();
                    i32 bestIdx = 0;
                    for (i32 w = 0; w < 16; ++w)
                    {
                        i64 err = 0;
                        for (u32 c = 0; c < 3; ++c)
                        {
                            const i64 d = static_cast<i64>(Bc6hInterpolate(a[c], b[c], w)) - target[t][c];
                            err += d * d;
                        }
                        if (err < bestErr)
                        {
                            bestErr = err;
                            bestIdx = w;
                        }
                    }
                    out[t] = bestIdx;
                    total += bestErr;
                }
                return total;
            };

            // Quantize an interp-space endpoint pair to 10-bit and recompute the values the
            // decoder will actually interpolate between.
            const auto quantizePair = [](const std::array<i32, 3>& e0, const std::array<i32, 3>& e1,
                                         std::array<i32, 3>& q0, std::array<i32, 3>& q1,
                                         std::array<i32, 3>& u0, std::array<i32, 3>& u1)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    q0[c] = Bc6hQuantizeEndpointUnsigned(e0[c]);
                    q1[c] = Bc6hQuantizeEndpointUnsigned(e1[c]);
                    u0[c] = Bc6hUnquantizeUnsigned(q0[c]);
                    u1[c] = Bc6hUnquantizeUnsigned(q1[c]);
                }
            };

            std::array<i32, 3> q0{}, q1{}, u0{}, u1{};
            quantizePair(ep0, ep1, q0, q1, u0, u1);
            std::array<i32, 16> indices{};
            i64 bestTotal = selectIndices(u0, u1, indices);

            // 4. Refine: alternate a least-squares endpoint fit (given the current indices)
            //    with index re-selection. interp = A*(1 - w/64) + B*(w/64) is linear in the
            //    two endpoints, so per channel this is a 2x2 normal-equation solve. Keep a
            //    step only if it lowers total error, so refinement can never regress a block
            //    (this closes most of the gap a single-segment mode-11 fit leaves on curved
            //    HDR gradients; a multi-mode / PCA encoder is a deferred follow-up).
            for (u32 iter = 0; iter < 2; ++iter)
            {
                std::array<i32, 3> rep0{}, rep1{};
                for (u32 c = 0; c < 3; ++c)
                {
                    f64 a00 = 0.0, a01 = 0.0, a11 = 0.0, rhs0 = 0.0, rhs1 = 0.0;
                    for (u32 t = 0; t < 16; ++t)
                    {
                        const f64 wgt = static_cast<f64>(kBC6HWeights4[indices[t]]) / 64.0;
                        const f64 s = 1.0 - wgt;
                        a00 += s * s;
                        a01 += s * wgt;
                        a11 += wgt * wgt;
                        rhs0 += s * static_cast<f64>(target[t][c]);
                        rhs1 += wgt * static_cast<f64>(target[t][c]);
                    }
                    const f64 det = a00 * a11 - a01 * a01;
                    if (std::abs(det) < 1e-6)
                    {
                        // Degenerate (every index identical): keep the current endpoints.
                        rep0[c] = u0[c];
                        rep1[c] = u1[c];
                    }
                    else
                    {
                        const f64 endA = (rhs0 * a11 - rhs1 * a01) / det;
                        const f64 endB = (rhs1 * a00 - rhs0 * a01) / det;
                        rep0[c] = std::clamp(static_cast<i32>(std::lround(endA)), 0, 0xFFFF);
                        rep1[c] = std::clamp(static_cast<i32>(std::lround(endB)), 0, 0xFFFF);
                    }
                }
                std::array<i32, 3> nq0{}, nq1{}, nu0{}, nu1{};
                quantizePair(rep0, rep1, nq0, nq1, nu0, nu1);
                std::array<i32, 16> nindices{};
                const i64 total = selectIndices(nu0, nu1, nindices);
                if (total >= bestTotal)
                    break; // converged / no improvement
                bestTotal = total;
                q0 = nq0;
                q1 = nq1;
                u0 = nu0;
                u1 = nu1;
                indices = nindices;
            }

            // 5. Anchor fixup: index[0]'s MSB is implicit-0, so it must be < 8. If not,
            //    swap endpoints and re-select (see the weight-table asymmetry note above).
            if (indices[0] >= 8)
            {
                std::swap(q0, q1);
                std::swap(u0, u1);
                selectIndices(u0, u1, indices);
                if (indices[0] >= 8)
                    indices[0] = 7; // defensive: guarantee a legal anchor on a rare tie
            }

            // 6. Pack the mode-11 bitstream: mode(5)=0b00011, then rw,gw,bw,rx,gx,bx (each
            //    10 bits), then indices (index 0 = 3 bits, indices 1..15 = 4 bits each).
            std::array<u8, 16> block{};
            Bc6hBitWriter bw{ block };
            bw.Put(0b00011u, 5);
            bw.Put(static_cast<u32>(q0[0]), 10);
            bw.Put(static_cast<u32>(q0[1]), 10);
            bw.Put(static_cast<u32>(q0[2]), 10);
            bw.Put(static_cast<u32>(q1[0]), 10);
            bw.Put(static_cast<u32>(q1[1]), 10);
            bw.Put(static_cast<u32>(q1[2]), 10);
            bw.Put(static_cast<u32>(indices[0]), 3);
            for (u32 t = 1; t < 16; ++t)
                bw.Put(static_cast<u32>(indices[t]), 4);
            std::memcpy(dst, block.data(), 16);
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

        CompressedTextureImage EncodeBC6H(const f32* pixels, u32 width, u32 height, u32 channels, bool generateMips)
        {
            OLO_PROFILE_FUNCTION();

            CompressedTextureImage image;
            // BC6H is an HDR RGB format; require at least three channels of float source.
            if (!pixels || width == 0 || height == 0 || channels < 3 || channels > 4)
            {
                OLO_CORE_ERROR("TextureCompression::EncodeBC6H - invalid input ({}x{}, {} ch; needs 3-4 channels)", width, height, channels);
                return image;
            }

            image.Format = TextureCompressionFormat::BC6H;
            image.Width = width;
            image.Height = height;
            image.SRGB = false;     // HDR is linear — never sRGB
            image.HasAlpha = false; // BC6H is RGB only

            const auto encodeLevel = [](const std::vector<f32>& rgb, u32 w, u32 h)
            {
                const u32 bx = BlockCount(w);
                const u32 by = BlockCount(h);
                std::vector<u8> out(static_cast<sizet>(bx) * by * 16);
                std::array<f32, 48> block{};
                for (u32 y = 0; y < by; ++y)
                {
                    for (u32 x = 0; x < bx; ++x)
                    {
                        GatherBlockRGBFloat(rgb, w, h, x, y, block);
                        EncodeBC6HBlockUnsigned(out.data() + (static_cast<sizet>(y) * bx + x) * 16, block.data());
                    }
                }
                return out;
            };

            std::vector<f32> level = ExpandToRGBFloat(pixels, width, height, channels);
            u32 mw = width;
            u32 mh = height;
            while (true)
            {
                image.Mips.push_back(encodeLevel(level, mw, mh));
                if (!generateMips || (mw == 1 && mh == 1))
                    break;
                u32 nw = 0;
                u32 nh = 0;
                level = DownsampleRGBFloat(level, mw, mh, nw, nh);
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
            // BC6H is HDR (half-float); it has no meaningful 8-bit representation. Callers
            // that need its pixels use DecodeToRGBAFloat instead.
            if (image.Format == TextureCompressionFormat::BC6H)
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

        bool DecodeToRGBAFloat(const CompressedTextureImage& image, u32 mipLevel,
                               std::vector<f32>& outRGBA, u32& outWidth, u32& outHeight)
        {
            if (!image.IsValid() || image.Format != TextureCompressionFormat::BC6H || mipLevel >= image.MipLevels())
                return false;

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
                    ::bcdec_bc6h_float(blockPtr, decoded.data(), 4 * 3, 0 /* unsigned */);

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
                formatInt != static_cast<u32>(TextureCompressionFormat::BC6H))
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
            if ((blobFormat == TextureCompressionFormat::BC5 || blobFormat == TextureCompressionFormat::BC6H) &&
                (flags & (kFlagSRGB | kFlagHasAlpha)) != 0)
            {
                OLO_CORE_ERROR("TextureCompression::DeserializeFromBlob - {} must not set sRGB/alpha flags ({:#x})",
                               blobFormat == TextureCompressionFormat::BC5 ? "BC5" : "BC6H", flags);
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
            if (format == TextureCompressionFormat::None)
            {
                // An HDR source (.hdr / .exr with float data) auto-selects BC6H; everything
                // else defaults to BC7. BC5 is never auto-chosen (deliberate normal-map opt-in).
                format = ::stbi_is_hdr(srcImagePath.c_str()) ? TextureCompressionFormat::BC6H
                                                             : TextureCompressionFormat::BC7;
            }

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
            CompressedTextureImage image;

            if (format == TextureCompressionFormat::BC6H)
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
                image = EncodeBC6H(data, static_cast<u32>(width), static_cast<u32>(height), 3, options.GenerateMips);
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
                if (format == TextureCompressionFormat::BC5)
                    image = EncodeBC5(data, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(channels), options.GenerateMips);
                else
                    image = EncodeBC7(data, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(channels), srgb, options.GenerateMips);
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
