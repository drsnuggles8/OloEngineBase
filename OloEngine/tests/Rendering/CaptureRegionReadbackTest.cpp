#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// CaptureRegionReadbackTest — GPU contract test (skips cleanly without GL 4.5+).
//
// Pins the COORDINATE MATH of GPUResourceInspector::CaptureTexturePng's `region`
// argument (issue #607's native-resolution capture gap): the top-left-origin rect
// an agent asks for must land on exactly those texels.
//
// Why this needs a synthetic pattern rather than a live scene. The obvious live
// check — capture the whole target 1:1, capture a region, compare the region
// against the corresponding sub-rect — CANNOT discriminate on a real scene: two
// captures taken moments apart differ by up to ~185/255 per channel on an
// animated scene (FFT water, temporal AA), which swamps the signal. Measured
// against a live editor, the "correct" position scored maxDiff=166 and the
// deliberately MIRRORED position scored 70 — i.e. the mirrored one looked
// *better*. A test built that way would have exonerated a broken flip.
//
// So the texture here is a deterministic gradient with a distinct value per
// texel (R encodes x, G encodes y), read back through the real capture path and
// checked value-by-value. A wrong row flip, a swapped x/y, or an off-by-one
// offset each produce a specific, unmistakable failure.
//
// The flip itself is the subtle part: `region` is top-left origin (matching the
// returned PNG, olo_render_target_stats' `rect` and olo_render_probe_pixel's
// texel space) while GL rows run bottom-up, so CaptureTexturePng reads GL rows
// [H - y - h, H - y) and then reverses them. Getting that wrong returns a
// perfectly plausible crop of the WRONG rows — the "plausible but wrong" failure
// a diagnostic tool must never have, because the next investigation trusts it.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <vector>

// OLO_TEST_LAYER: L1

namespace
{
    using OloEngine::GPUResourceInspector;

    constexpr u32 kWidth = 64;
    constexpr u32 kHeight = 48;

    // R = x / (W-1), G = y / (H-1) with y measured TOP-DOWN, B = 0.25 constant.
    // Encoding both axes means a swapped x/y or a mirrored row range is a
    // numeric failure, not a subjective "looks off".
    [[nodiscard]] u8 ExpectedR(u32 x)
    {
        const f32 v = static_cast<f32>(x) / static_cast<f32>(kWidth - 1);
        return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    [[nodiscard]] u8 ExpectedG(u32 yTopDown)
    {
        const f32 v = static_cast<f32>(yTopDown) / static_cast<f32>(kHeight - 1);
        return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    // Build the gradient as GL expects it: row 0 of the upload is the BOTTOM row,
    // so a texel whose top-down row is `yTopDown` is uploaded at kHeight-1-yTopDown.
    [[nodiscard]] std::vector<f32> BuildGradient()
    {
        std::vector<f32> pixels(static_cast<sizet>(kWidth) * kHeight * 4, 0.0f);
        for (u32 glRow = 0; glRow < kHeight; ++glRow)
        {
            const u32 yTopDown = kHeight - 1 - glRow;
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(glRow) * kWidth + x) * 4;
                pixels[i + 0] = static_cast<f32>(x) / static_cast<f32>(kWidth - 1);
                pixels[i + 1] = static_cast<f32>(yTopDown) / static_cast<f32>(kHeight - 1);
                pixels[i + 2] = 0.25f;
                pixels[i + 3] = 1.0f;
            }
        }
        return pixels;
    }

    // Decode a captured PNG to 8-bit RGBA rows, top-down (stb's natural order,
    // which is the orientation CaptureTexturePng already flipped into).
    struct DecodedPng
    {
        std::vector<u8> Pixels;
        int Width = 0;
        int Height = 0;
        int Channels = 0;
    };

    [[nodiscard]] DecodedPng DecodePng(const std::vector<u8>& bytes)
    {
        DecodedPng out;
        stbi_uc* data = ::stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                &out.Width, &out.Height, &out.Channels, 0);
        if (data == nullptr)
            return out;
        out.Pixels.assign(data, data + static_cast<sizet>(out.Width) * out.Height * out.Channels);
        ::stbi_image_free(data);
        return out;
    }
} // namespace

TEST(CaptureRegionReadback, RegionReadsExactlyTheRequestedTopLeftRect)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const std::vector<f32> gradient = BuildGradient();
    const u32 texture = OloEngine::Tests::CreateFloatTexture2D(kWidth, kHeight, gradient.data());
    ASSERT_NE(texture, 0u);

    // Sanity: the WHOLE capture must reproduce the gradient top-down. If this
    // fails the baseline path is broken and every region assertion below is moot.
    {
        const auto whole = GPUResourceInspector::CaptureTexturePng(texture, 0, 0,
                                                                   GPUResourceInspector::CaptureNormalizeMode::Off,
                                                                   /*maxWidth*/ 0);
        ASSERT_TRUE(whole.Error.empty()) << whole.Error;
        EXPECT_EQ(whole.Width, kWidth);
        EXPECT_EQ(whole.Height, kHeight);
        EXPECT_EQ(whole.RegionWidth, kWidth) << "a whole capture should echo the full mip as its region";
        EXPECT_EQ(whole.RegionHeight, kHeight);

        const DecodedPng png = DecodePng(whole.PngBytes);
        ASSERT_EQ(png.Width, static_cast<int>(kWidth));
        ASSERT_EQ(png.Height, static_cast<int>(kHeight));
        ASSERT_GE(png.Channels, 3);
        // Corners pin the orientation: top-left is (0,0), bottom-right is (W-1,H-1).
        const auto at = [&png](u32 x, u32 y, int c)
        { return png.Pixels[(static_cast<sizet>(y) * png.Width + x) * png.Channels + c]; };
        EXPECT_NEAR(at(0, 0, 0), ExpectedR(0), 1);
        EXPECT_NEAR(at(0, 0, 1), ExpectedG(0), 1);
        EXPECT_NEAR(at(kWidth - 1, kHeight - 1, 0), ExpectedR(kWidth - 1), 1);
        EXPECT_NEAR(at(kWidth - 1, kHeight - 1, 1), ExpectedG(kHeight - 1), 1);
    }

    // The region under test: deliberately OFF-CENTRE and in the TOP half. A
    // centred or full-height rect makes a vertical mirror bug cancel out, so such
    // a probe would exonerate a broken flip.
    constexpr u32 kRegionX = 11;
    constexpr u32 kRegionY = 7;
    constexpr u32 kRegionW = 20;
    constexpr u32 kRegionH = 13;

    const auto capture = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, /*maxWidth*/ 0,
        GPUResourceInspector::CaptureRegion{ kRegionX, kRegionY, kRegionW, kRegionH });
    ASSERT_TRUE(capture.Error.empty()) << capture.Error;

    EXPECT_EQ(capture.Width, kRegionW);
    EXPECT_EQ(capture.Height, kRegionH);
    // SourceWidth/Height stay the FULL mip's — a caller needs the parent size to
    // place the crop, and must not mistake the region for the whole target.
    EXPECT_EQ(capture.SourceWidth, kWidth);
    EXPECT_EQ(capture.SourceHeight, kHeight);
    EXPECT_EQ(capture.RegionX, kRegionX);
    EXPECT_EQ(capture.RegionY, kRegionY);
    EXPECT_EQ(capture.RegionWidth, kRegionW);
    EXPECT_EQ(capture.RegionHeight, kRegionH);

    const DecodedPng png = DecodePng(capture.PngBytes);
    ASSERT_EQ(png.Width, static_cast<int>(kRegionW));
    ASSERT_EQ(png.Height, static_cast<int>(kRegionH));
    ASSERT_GE(png.Channels, 3);

    // Every texel, not just the corners: an off-by-one in either axis, a swapped
    // x/y, or a mirrored row range all fail here with a concrete coordinate.
    for (u32 y = 0; y < kRegionH; ++y)
    {
        for (u32 x = 0; x < kRegionW; ++x)
        {
            const sizet i = (static_cast<sizet>(y) * png.Width + x) * png.Channels;
            EXPECT_NEAR(png.Pixels[i + 0], ExpectedR(kRegionX + x), 1)
                << "R (x-encoded) wrong at region texel (" << x << ", " << y << ")";
            EXPECT_NEAR(png.Pixels[i + 1], ExpectedG(kRegionY + y), 1)
                << "G (top-down-y-encoded) wrong at region texel (" << x << ", " << y << ")"
                << " — a mirrored row range is the classic failure here";
        }
    }

    ::glDeleteTextures(1, &texture);
}

TEST(CaptureRegionReadback, OutOfBoundsRegionIsAnErrorNotASilentClamp)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const std::vector<f32> gradient = BuildGradient();
    const u32 texture = OloEngine::Tests::CreateFloatTexture2D(kWidth, kHeight, gradient.data());
    ASSERT_NE(texture, 0u);

    // A silently shrunk rect makes a 1:1 measurement report the wrong spatial
    // period without ever saying so — precisely what this argument exists to
    // prevent, so every out-of-range form must fail loudly.
    const auto tooWide = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, 0,
        GPUResourceInspector::CaptureRegion{ 0, 0, kWidth + 1, kHeight });
    EXPECT_FALSE(tooWide.Error.empty());
    EXPECT_TRUE(tooWide.PngBytes.empty());

    const auto offRight = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, 0,
        GPUResourceInspector::CaptureRegion{ kWidth - 4, 0, 8, 8 });
    EXPECT_FALSE(offRight.Error.empty()) << "a rect that starts inside but runs past the right edge must fail";

    const auto offBottom = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, 0,
        GPUResourceInspector::CaptureRegion{ 0, kHeight - 2, 8, 8 });
    EXPECT_FALSE(offBottom.Error.empty()) << "a rect that runs past the bottom edge must fail";

    const auto originOutside = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, 0,
        GPUResourceInspector::CaptureRegion{ kWidth, 0, 1, 1 });
    EXPECT_FALSE(originOutside.Error.empty());

    // Huge offset + huge extent must not wrap the u32 addition into a "valid" rect.
    const auto wrapping = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, 0,
        GPUResourceInspector::CaptureRegion{ 0xFFFFFF00u, 0, 0x200u, 1 });
    EXPECT_FALSE(wrapping.Error.empty()) << "an overflowing offset+extent pair must be rejected, not wrapped";

    ::glDeleteTextures(1, &texture);
}

TEST(CaptureRegionReadback, RegionUnderMaxWidthStaysNativeAndOverItDownscales)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const std::vector<f32> gradient = BuildGradient();
    const u32 texture = OloEngine::Tests::CreateFloatTexture2D(kWidth, kHeight, gradient.data());
    ASSERT_NE(texture, 0u);

    // The whole point of the argument: a region narrower than maxWidth is NOT
    // resampled, so a pixel-scale measurement is meaningful.
    const auto native = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, /*maxWidth*/ 32,
        GPUResourceInspector::CaptureRegion{ 4, 4, 16, 16 });
    ASSERT_TRUE(native.Error.empty()) << native.Error;
    EXPECT_EQ(native.Width, 16u);
    EXPECT_EQ(native.Height, 16u);
    EXPECT_EQ(native.Width, native.RegionWidth) << "width == region width is what 'nativeResolution' reports";

    // A region wider than maxWidth still downscales — and the reply's dimensions
    // are what tells the caller the pixels are no longer 1:1.
    const auto scaled = GPUResourceInspector::CaptureTexturePng(
        texture, 0, 0, GPUResourceInspector::CaptureNormalizeMode::Off, /*maxWidth*/ 8,
        GPUResourceInspector::CaptureRegion{ 0, 0, 32, 16 });
    ASSERT_TRUE(scaled.Error.empty()) << scaled.Error;
    EXPECT_EQ(scaled.Width, 8u);
    EXPECT_LT(scaled.Width, scaled.RegionWidth);
    EXPECT_EQ(scaled.RegionWidth, 32u) << "the region echo must stay the REQUESTED rect, not the encoded size";

    ::glDeleteTextures(1, &texture);
}
