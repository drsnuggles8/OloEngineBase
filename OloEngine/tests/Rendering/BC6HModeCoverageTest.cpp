// OLO_TEST_LAYER: L3
//
// Bit-layout conformance for every BC6H block mode (#624).
//
// The encoder writes 14 different block layouts. Each one is a hand-verifiable inverse of
// bcdec's per-mode read sequence, and a single misplaced field in any of them would ship
// corrupt blocks for whichever inputs choose that mode — while every *other* mode, and
// therefore the aggregate PSNR tests, stayed perfectly green.
//
// So this file checks each mode on its own, and checks it against an oracle rather than
// against the encoder's own beliefs:
//
//   1. `EncodeBlockForModeForTest` packs a block using only that mode AND reports the 48
//      half-float values the encoder expects a conformant decoder to produce.
//   2. The vendored bcdec decoder (via DecodeToRGBAFloat) decodes the packed bytes.
//   3. The two must agree exactly. A shifted field changes what bcdec reads back but not
//      what the encoder predicted, so the comparison fails.
//
// The mode field itself is re-derived here straight from the block bytes, independently
// of the encoder's tables, so "mode 7 encoded as mode 6" is caught too.

#include "BC6HBlockModeReader.h"

#include "OloEngine/Renderer/BC6HEncoder.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include <gtest/gtest.h>

#include <glm/gtc/packing.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace OloEngine;

namespace
{
    // Wrap a single packed block as a 4x4 CompressedTextureImage so the shared
    // bcdec-backed decode path can be used verbatim.
    CompressedTextureImage AsSingleBlockImage(const std::array<u8, 16>& block, bool isSigned)
    {
        CompressedTextureImage image;
        image.Format = isSigned ? TextureCompressionFormat::BC6HSigned : TextureCompressionFormat::BC6H;
        image.Width = 4;
        image.Height = 4;
        image.Mips.emplace_back(block.begin(), block.end());
        return image;
    }

    // A 4x4 block with two well-separated colour clusters split along the diagonal, so
    // both one-subset and two-subset modes have something meaningful to encode. Values
    // stay well above the half-float denormal threshold (~6e-5) so the exact comparison
    // below is never decided by denormal rounding.
    std::array<f32, 48> MakeMixedBlock(bool withNegatives)
    {
        std::array<f32, 48> block{};
        for (u32 t = 0; t < 16; ++t)
        {
            const u32 x = t % 4;
            const u32 y = t / 4;
            const bool lower = (x + y) >= 3;
            const f32 ramp = static_cast<f32>(t) / 15.0f;
            f32* p = &block[t * 3];
            if (lower)
            {
                p[0] = 2.50f + ramp * 1.5f;
                p[1] = 1.75f + ramp * 0.5f;
                p[2] = 0.90f + ramp * 0.25f;
            }
            else
            {
                p[0] = 0.20f + ramp * 0.10f;
                p[1] = 0.45f + ramp * 0.05f;
                p[2] = 0.75f + ramp * 0.15f;
            }
            if (withNegatives && (t % 3) == 0)
            {
                p[0] = -p[0];
                p[2] = -p[2];
            }
        }
        return block;
    }

    void CheckMode(u32 modeIndex, bool isSigned)
    {
        SCOPED_TRACE(::testing::Message() << "mode " << modeIndex << (isSigned ? " (signed)" : " (unsigned)"));

        const std::array<f32, 48> source = MakeMixedBlock(isSigned);
        std::array<u8, 16> block{};
        std::array<u16, 48> predicted{};
        ASSERT_TRUE(BC6H::EncodeBlockForModeForTest(block.data(), source.data(), isSigned, modeIndex, predicted.data()));

        EXPECT_EQ(Tests::ReadBC6HModeIndex(block.data()), static_cast<i32>(modeIndex))
            << "packed block does not carry the mode it was encoded for";

        std::vector<f32> decoded;
        u32 dw = 0;
        u32 dh = 0;
        ASSERT_TRUE(TextureCompression::DecodeToRGBAFloat(AsSingleBlockImage(block, isSigned), 0, decoded, dw, dh));
        ASSERT_EQ(decoded.size(), 4u * 4u * 4u);

        for (u32 t = 0; t < 16; ++t)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                // Compare the raw float bit patterns: half -> float is exact for every
                // value these blocks can produce, so agreement must be exact. A tolerance
                // here would let a one-bit field shift slip through on smooth data.
                const f32 expected = ::glm::unpackHalf1x16(predicted[t * 3 + c]);
                const f32 actual = decoded[t * 4 + c];
                u32 expectedBits = 0;
                u32 actualBits = 0;
                std::memcpy(&expectedBits, &expected, sizeof(expectedBits));
                std::memcpy(&actualBits, &actual, sizeof(actualBits));
                EXPECT_EQ(expectedBits, actualBits)
                    << "texel " << t << " channel " << c << ": encoder predicted " << expected
                    << " but bcdec decoded " << actual;
            }
        }
    }
} // namespace

TEST(BC6HModeCoverage, EveryUnsignedModePacksExactlyWhatBcdecReads)
{
    for (u32 modeIndex = 0; modeIndex < BC6H::kModeCount; ++modeIndex)
        CheckMode(modeIndex, /*isSigned*/ false);
}

TEST(BC6HModeCoverage, EverySignedModePacksExactlyWhatBcdecReads)
{
    for (u32 modeIndex = 0; modeIndex < BC6H::kModeCount; ++modeIndex)
        CheckMode(modeIndex, /*isSigned*/ true);
}

TEST(BC6HModeCoverage, RejectsAnOutOfRangeModeIndex)
{
    const std::array<f32, 48> source = MakeMixedBlock(false);
    std::array<u8, 16> block{};
    std::array<u16, 48> predicted{};
    EXPECT_FALSE(BC6H::EncodeBlockForModeForTest(block.data(), source.data(), false, BC6H::kModeCount, predicted.data()));
}

TEST(BC6HModeCoverage, TheAutomaticEncoderPicksAModeThatBeatsModeTenAlone)
{
    // Mode 10 (one subset, explicit 10-bit endpoints) is what #440 shipped and is still a
    // candidate, so the full search can never do worse on a block. On a two-cluster block
    // it should do strictly better — that is the whole point of the multi-mode search.
    const std::array<f32, 48> source = MakeMixedBlock(false);

    const auto blockError = [&source](const std::array<u8, 16>& block) -> double
    {
        std::vector<f32> decoded;
        u32 dw = 0;
        u32 dh = 0;
        EXPECT_TRUE(TextureCompression::DecodeToRGBAFloat(AsSingleBlockImage(block, false), 0, decoded, dw, dh));
        double total = 0.0;
        for (u32 t = 0; t < 16; ++t)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                const double d = static_cast<double>(source[t * 3 + c]) - static_cast<double>(decoded[t * 4 + c]);
                total += d * d;
            }
        }
        return total;
    };

    std::array<u8, 16> modeTenBlock{};
    std::array<u16, 48> predicted{};
    ASSERT_TRUE(BC6H::EncodeBlockForModeForTest(modeTenBlock.data(), source.data(), false, 10, predicted.data()));

    std::array<u8, 16> autoBlock{};
    BC6H::EncodeBlock(autoBlock.data(), source.data(), false);

    EXPECT_LT(blockError(autoBlock), blockError(modeTenBlock))
        << "the multi-mode search did not beat mode 10 on a two-cluster block";
}
