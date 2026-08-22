// OLO_TEST_LAYER: L3
//
// GPU round-trip for the terrain VT BC7 tile compressor (issue #715, slice 4).
//
// TerrainVTCompressBC7.comp encodes the baked RGBA8 scratch tiles to BC7
// mode-6 blocks on the GPU. This test drives the REAL kernel on a live GL 4.6
// context (SKIPs cleanly without one) and decodes every produced block on the
// CPU with the vendored bcdec — an oracle independent of the encoder, so a
// wrong mode prefix, endpoint order, P-bit placement or index packing cannot
// hide behind a matched decoder. Same shape as the BC6H encoder's oracle
// test (TextureCompressionTest.cpp) and the same layer as the sibling
// CompressedTextureVisualEvidenceTest: CPU<->GPU bit/byte identity.
//
// Content covers the three regimes that break block encoders differently:
//   - a steep 2-axis gradient (R ramps in x, G in y) — the worst case for a
//     single-segment mode, pins the PSNR floor;
//   - a hard two-colour edge deliberately OFF the 4-texel block grid, bright
//     side first — the mixed blocks force the anchor-constraint endpoint swap
//     (index 0's MSB is implicit-0 in the bitstream);
//   - two flat tiles (one with alpha != 255) — mode 6's 7+1-bit endpoints span
//     every 8-bit value, so a solid colour must round-trip within 1/255.
//
// The thresholds were measured against bcdec with a CPU transliteration of the
// kernel on this exact content: 34.5 dB overall, flat max diff 1, mode bits
// correct on all blocks including the anchor-swapped ones. A broken bit layout
// scores ~10-15 dB, so the 30 dB floor discriminates by a wide margin.
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTextureTypes.h"

// Declarations only — the single BCDEC_IMPLEMENTATION lives in
// TextureCompression.cpp, exactly as the BC6H oracle tests consume it.
#include <bcdec.h>

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include "TestTempDir.h"
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Tests;

namespace
{
    // Two tiles of 16 texels: scratch is a strip of tile slots along x, layers
    // 0 and 1 — the same addressing the bake kernel writes.
    constexpr u32 kTileTexels = 16;
    constexpr u32 kTileBlocks = kTileTexels / 4;
    constexpr u32 kTiles = 2;
    constexpr u32 kLayers = 2;
    constexpr u32 kScratchW = kTiles * kTileTexels; // 32
    constexpr u32 kScratchH = kTileTexels;          // 16
    constexpr u32 kOutW = kTiles * kTileBlocks;     // 8
    constexpr u32 kOutH = kTileBlocks;              // 4

    // CPU twin of the std430 header + request tail of the TerrainVTBake SSBO
    // (binding 80) — the same layout TerrainVirtualTexture.cpp's VTBakeHeader
    // pins with its own static_assert. The compressor reads only Config0.w
    // (requestCount) and Config1.y (tileTexels); everything else is the bake
    // kernel's business and stays zero here. The request tail reuses the REAL
    // VTBakeRequest rather than a hand copy, so a layout change there turns
    // into a compile-visible size change here instead of a silent stride
    // drift against what the kernels declare.
    struct VTBakeSsboPod
    {
        glm::uvec4 m_Config0{ 0u }; // pagesWide, pageTexels, borderTexels, requestCount
        glm::uvec4 m_Config1{ 0u }; // layerCount, tileTexels, layerResolution, unused
        glm::vec4 m_World{ 0.0f };
        glm::vec4 m_Texel{ 0.0f };
        glm::mat4 m_Model{ 1.0f };
        glm::vec4 m_Tiling0{ 0.0f };
        glm::vec4 m_Tiling1{ 0.0f };
        glm::vec4 m_Sharp0{ 0.0f };
        glm::vec4 m_Sharp1{ 0.0f };
        VTBakeRequest m_Requests[kTiles]{};
    };
    static_assert(sizeof(VTBakeSsboPod) == 192 + kTiles * sizeof(VTBakeRequest),
                  "must match the std430 TerrainVTBake header the kernels declare");

    // Layer 0: tile 0 = steep 2D gradient, tile 1 = flat opaque colour.
    // Layer 1: tile 0 = hard vertical edge at x=6 (NOT a block boundary, so
    //          block bx=1 mixes both colours; bright side first so that block's
    //          texel 0 forces the anchor swap), tile 1 = flat with alpha 160.
    std::vector<u8> MakeScratchLayer(u32 layer)
    {
        std::vector<u8> px(static_cast<sizet>(kScratchW) * kScratchH * 4, 0);
        for (u32 y = 0; y < kScratchH; ++y)
        {
            for (u32 x = 0; x < kScratchW; ++x)
            {
                u8* p = &px[(static_cast<sizet>(y) * kScratchW + x) * 4];
                const bool tile1 = x >= kTileTexels;
                const u32 lx = tile1 ? x - kTileTexels : x;
                if (layer == 0)
                {
                    if (!tile1)
                    {
                        p[0] = static_cast<u8>((lx * 255) / (kTileTexels - 1));
                        p[1] = static_cast<u8>((y * 255) / (kScratchH - 1));
                        p[2] = 128;
                        p[3] = 255;
                    }
                    else
                    {
                        p[0] = 77;
                        p[1] = 149;
                        p[2] = 210;
                        p[3] = 255;
                    }
                }
                else
                {
                    if (!tile1)
                    {
                        if (lx < 6)
                        {
                            p[0] = 240;
                            p[1] = 200;
                            p[2] = 160;
                            p[3] = 255;
                        }
                        else
                        {
                            p[0] = 10;
                            p[1] = 20;
                            p[2] = 30;
                            p[3] = 255;
                        }
                    }
                    else
                    {
                        p[0] = 32;
                        p[1] = 64;
                        p[2] = 96;
                        p[3] = 160;
                    }
                }
            }
        }
        return px;
    }
} // namespace

TEST(TerrainVTCompressBC7, GpuMode6BlocksDecodeWithTheBcdecOracle)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // ---- inputs -------------------------------------------------------------
    const std::vector<u8> layer0 = MakeScratchLayer(0);
    const std::vector<u8> layer1 = MakeScratchLayer(1);

    Texture2DArraySpecification scratchSpec;
    scratchSpec.Width = kScratchW;
    scratchSpec.Height = kScratchH;
    scratchSpec.Layers = kLayers;
    scratchSpec.Format = Texture2DArrayFormat::RGBA8;
    scratchSpec.GenerateMipmaps = false;
    Ref<Texture2DArray> scratch = Texture2DArray::Create(scratchSpec);
    ASSERT_TRUE(scratch);
    scratch->SetLayerData(0, layer0.data(), kScratchW, kScratchH);
    scratch->SetLayerData(1, layer1.data(), kScratchW, kScratchH);

    Texture2DArraySpecification stagingSpec;
    stagingSpec.Width = kOutW;
    stagingSpec.Height = kOutH;
    stagingSpec.Layers = kLayers;
    stagingSpec.Format = Texture2DArrayFormat::RGBA32UI;
    stagingSpec.GenerateMipmaps = false;
    Ref<Texture2DArray> staging = Texture2DArray::Create(stagingSpec);
    ASSERT_TRUE(staging);

    VTBakeSsboPod header;
    header.m_Config0.w = kTiles;      // requestCount
    header.m_Config1.y = kTileTexels; // tileTexels
    Ref<StorageBuffer> bakeBuffer = StorageBuffer::Create(
        sizeof(VTBakeSsboPod), ShaderBindingLayout::SSBO_TERRAIN_VT_BAKE, StorageBufferUsage::DynamicDraw);
    ASSERT_TRUE(bakeBuffer);
    bakeBuffer->SetData(&header, sizeof(VTBakeSsboPod), 0);

    Ref<ComputeShader> compress = ComputeShader::Create("assets/shaders/compute/TerrainVTCompressBC7.comp");
    ASSERT_TRUE(compress && compress->IsValid()) << "TerrainVTCompressBC7.comp failed to compile/link";

    while (glGetError() != GL_NO_ERROR)
    {
    } // drain any leaked errors

    // ---- dispatch: PROGRAM FIRST, then resources ----------------------------
    // Same seam discipline as TerrainVirtualTexture::BakeTiles: the HeapBinding
    // fork asks Shader::IsBoundProgramBindless() about the program IN FLIGHT,
    // and this kernel is slot-based (no OLO_BINDLESS token), so with it bound
    // the seam takes the fallback and issues real binds. Bound in any other
    // order, a bindless answer would stage an offset and bind nothing.
    compress->Bind();
    RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_TERRAIN_VT_BAKE, bakeBuffer->GetRHIHandle());
    using enum RHI::HeapSlotLifetime;
    HeapBinding::BindImageOrOffset(0, scratch->GetRHIHandle(), 0, /*layered*/ true, 0,
                                   RHI::Access::StorageRead, RHI::Format::RGBA8UNorm, Persistent);
    HeapBinding::BindImageOrOffset(1, staging->GetRHIHandle(), 0, /*layered*/ true, 0,
                                   RHI::Access::StorageWrite, RHI::Format::RGBA32UInt, Persistent);

    // 8x8 local size covers the 4x4 blocks of one tile in one group;
    // z = slot * 2 + layer.
    RenderCommand::DispatchCompute(1, 1, kTiles * kLayers);
    // The CPU reads the image back via glGetTextureImage next.
    RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureUpdate);

    // ---- readback -----------------------------------------------------------
    std::vector<u32> words(static_cast<sizet>(kOutW) * kOutH * kLayers * 4, 0u);
    glGetTextureImage(staging->GetRendererID(), 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                      static_cast<GLsizei>(words.size() * sizeof(u32)), words.data());
    ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glGetTextureImage (RGBA32UI readback) failed";

    // Unbind everything before the Refs release their objects — a binding left
    // dangling on the shared process-wide context surfaces as a spurious GL
    // error in an unrelated later GPU test (see GPUOcclusionCullGPUTest).
    ::glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    ::glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32UI);
    ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_TERRAIN_VT_BAKE, 0);
    ::glUseProgram(0);

    // ---- decode with bcdec and score ---------------------------------------
    const std::vector<u8>* layers[kLayers] = { &layer0, &layer1 };
    f64 mse = 0.0;
    sizet samples = 0;
    u32 wrongModeBlocks = 0;
    i32 flatMaxDiff = 0;
    std::vector<u8> decodedEvidence(static_cast<sizet>(kScratchW) * kScratchH * kLayers * 4, 0);

    for (u32 layer = 0; layer < kLayers; ++layer)
    {
        const std::vector<u8>& src = *layers[layer];
        for (u32 slot = 0; slot < kTiles; ++slot)
        {
            for (u32 by = 0; by < kTileBlocks; ++by)
            {
                for (u32 bx = 0; bx < kTileBlocks; ++bx)
                {
                    const sizet base =
                        ((static_cast<sizet>(layer) * kOutH + by) * kOutW + (slot * kTileBlocks + bx)) * 4;

                    // (a) Mode prefix: bits 0..6 must read 0b1000000 — six zero
                    // bits then the set bit that says "mode 6".
                    if ((words[base] & 0x7Fu) != 0x40u)
                    {
                        ++wrongModeBlocks;
                        ADD_FAILURE() << "block (slot " << slot << ", " << bx << "," << by << ", layer "
                                      << layer << ") mode bits are 0x" << std::hex << (words[base] & 0x7Fu)
                                      << std::dec << ", expected 0x40 (mode 6)";
                        continue;
                    }

                    // uvec4 (x,y,z,w) -> the 16 little-endian block bytes bcdec
                    // consumes; x = bits 0..31, byte 0 = the low byte of x.
                    u8 blockBytes[16];
                    std::memcpy(blockBytes, &words[base], 16);
                    u8 decoded[16 * 4];
                    ::bcdec_bc7(blockBytes, decoded, 4 * 4);

                    for (u32 j = 0; j < 4; ++j)
                    {
                        for (u32 i = 0; i < 4; ++i)
                        {
                            const u32 x = slot * kTileTexels + bx * 4 + i;
                            const u32 y = by * 4 + j;
                            const u8* s = &src[(static_cast<sizet>(y) * kScratchW + x) * 4];
                            const u8* d = &decoded[(j * 4 + i) * 4];
                            u8* e = &decodedEvidence[((static_cast<sizet>(layer) * kScratchH + y) * kScratchW + x) * 4];
                            for (u32 c = 0; c < 4; ++c)
                            {
                                const i32 diff = static_cast<i32>(d[c]) - static_cast<i32>(s[c]);
                                mse += static_cast<f64>(diff) * diff;
                                ++samples;
                                // (c) slot 1 is the flat tile on both layers.
                                if (slot == 1)
                                    flatMaxDiff = std::max(flatMaxDiff, std::abs(diff));
                                e[c] = d[c];
                            }
                        }
                    }
                }
            }
        }
    }

    EXPECT_EQ(wrongModeBlocks, 0u) << wrongModeBlocks << " of " << (kOutW * kOutH * kLayers)
                                   << " blocks did not carry the mode-6 prefix";
    ASSERT_GT(samples, 0u);

    // (b) Overall fidelity across gradient + edge + flat content. Measured
    // 34.5 dB for this encoder on this exact content (see file header); a
    // wrong bit layout lands at ~10-15 dB, so 30 dB discriminates with margin.
    mse /= static_cast<f64>(samples);
    const f64 psnr = (mse < 1e-9) ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    EXPECT_GT(psnr, 30.0) << "bcdec-decoded BC7 PSNR too low: " << psnr << " dB (mse " << mse << ")";

    // (c) The flat tiles must round-trip near-exactly: mode 6's 7-bit + P-bit
    // endpoints reach every 8-bit value, so a solid colour (alpha included)
    // survives with at most the interpolator's rounding.
    EXPECT_LE(flatMaxDiff, 1) << "a solid-colour tile decoded " << flatMaxDiff
                              << "/255 off — flat blocks must be near-lossless in mode 6";

    // Pixel evidence per the project's visual-verification rule: the decoded
    // layers, stacked, as a human-inspectable artifact.
    const std::filesystem::path pngPath = TempFile("TerrainVTCompressBC7_decoded.png");
    const int wrote = ::stbi_write_png(pngPath.string().c_str(), static_cast<int>(kScratchW),
                                       static_cast<int>(kScratchH * kLayers), 4, decodedEvidence.data(),
                                       static_cast<int>(kScratchW) * 4);
    EXPECT_NE(wrote, 0) << "failed to write evidence PNG";
    if (wrote != 0)
    {
        OLO_CORE_INFO("TerrainVTCompressBC7: wrote bcdec-decoded evidence to {}", pngPath.string());
    }
}
