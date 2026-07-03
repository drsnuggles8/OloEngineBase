#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/SlugData.h"
#include "OloEngine/Renderer/SlugFontProcessor.h"

// GL-context fixture for the deferred-upload test below (OLO_ENSURE_GPU_OR_SKIP).
#include "Rendering/PropertyTests/RenderPropertyTest.h"

#include <stb_image/stb_truetype.h>

#include <cmath>
#include <fstream>
#include <vector>

using namespace OloEngine;

// ---------------------------------------------------------------------------
// SlugData unit tests
// ---------------------------------------------------------------------------

TEST(SlugDataTest, GlyphLookupReturnsNullForMissing)
{
    SlugFontData data;
    EXPECT_EQ(data.GetGlyph(0x41), nullptr);
}

TEST(SlugDataTest, GlyphLookupFindsInserted)
{
    SlugFontData data;
    SlugGlyphData glyph;
    glyph.AdvanceWidth = 0.5f;
    data.Glyphs[0x41] = glyph;

    const auto* found = data.GetGlyph(0x41);
    ASSERT_NE(found, nullptr);
    EXPECT_FLOAT_EQ(found->AdvanceWidth, 0.5f);
}

TEST(SlugDataTest, GetAdvanceUsesKerning)
{
    SlugFontData data;
    SlugGlyphData g;
    g.AdvanceWidth = 0.6f;
    data.Glyphs[0x41] = g; // 'A'
    data.Glyphs[0x56] = g; // 'V'

    const u64 key = (static_cast<u64>(0x41) << 32) | 0x56;
    data.KerningPairs[key] = -0.05f;

    EXPECT_FLOAT_EQ(data.GetAdvance(0x41, 0x56), 0.55f);
}

TEST(SlugDataTest, GetAdvanceWithoutKerning)
{
    SlugFontData data;
    SlugGlyphData g;
    g.AdvanceWidth = 0.6f;
    data.Glyphs[0x41] = g;

    EXPECT_FLOAT_EQ(data.GetAdvance(0x41, 0x42), 0.6f);
}

// ---------------------------------------------------------------------------
// SlugFontProcessor unit tests
// ---------------------------------------------------------------------------

// Helper: load a TTF font from the standard asset path.
// Returns empty vector if file not found (test will be skipped).
static std::vector<u8> LoadTestFont()
{
    // Try the editor font path (test runner cwd is typically the repo root).
    const char* paths[] = {
        "OloEditor/assets/fonts/opensans/OpenSans-Regular.ttf",
        "../OloEditor/assets/fonts/opensans/OpenSans-Regular.ttf",
        "assets/fonts/opensans/OpenSans-Regular.ttf",
    };

    for (const auto* path : paths)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            auto size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<u8> buffer(static_cast<sizet>(size));
            file.read(reinterpret_cast<char*>(buffer.data()), size);
            if (file.fail() || file.gcount() != static_cast<std::streamsize>(size))
            {
                continue;
            }
            return buffer;
        }
    }
    return {};
}

class SlugFontProcessorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_FontBuffer = LoadTestFont();
        if (m_FontBuffer.empty())
        {
            GTEST_SKIP() << "OpenSans-Regular.ttf not found — skipping font processor tests";
        }

        ASSERT_TRUE(stbtt_InitFont(&m_FontInfo, m_FontBuffer.data(),
                                   stbtt_GetFontOffsetForIndex(m_FontBuffer.data(), 0)));

        int ascent{};
        int descent{};
        int lineGap{};
        stbtt_GetFontVMetrics(&m_FontInfo, &ascent, &descent, &lineGap);
        if (ascent == descent)
        {
            GTEST_SKIP() << "Font has degenerate metrics (ascent == descent)";
        }
        m_EmScale = 1.0f / static_cast<f32>(ascent - descent);
    }

    stbtt_fontinfo m_FontInfo{};
    std::vector<u8> m_FontBuffer;
    f32 m_EmScale = 0.0f;
};

TEST_F(SlugFontProcessorTest, GlyphBoundsAreValid)
{
    // Build minimal SlugFontData with a few glyphs and verify bounding boxes.
    SlugFontData fontData;
    fontData.Metrics.AscenderY = 1.0f;
    fontData.Metrics.DescenderY = -0.3f;
    fontData.Metrics.LineHeight = 1.3f;
    fontData.Metrics.UnitsPerEm = 1.0f / m_EmScale;

    // Add 'A', 'B', 'C'.
    for (u32 cp : { 0x41u, 0x42u, 0x43u })
    {
        const int gi = stbtt_FindGlyphIndex(&m_FontInfo, static_cast<int>(cp));
        ASSERT_NE(gi, 0) << "Glyph for codepoint " << cp << " not found";

        int advW{};
        int lsb{};
        stbtt_GetGlyphHMetrics(&m_FontInfo, gi, &advW, &lsb);

        int x0{};
        int y0{};
        int x1{};
        int y1{};
        stbtt_GetGlyphBox(&m_FontInfo, gi, &x0, &y0, &x1, &y1);

        SlugGlyphData glyph;
        glyph.AdvanceWidth = static_cast<f32>(advW) * m_EmScale;
        glyph.PlaneBoundsLeft = static_cast<f32>(x0) * m_EmScale;
        glyph.PlaneBoundsBottom = static_cast<f32>(y0) * m_EmScale;
        glyph.PlaneBoundsRight = static_cast<f32>(x1) * m_EmScale;
        glyph.PlaneBoundsTop = static_cast<f32>(y1) * m_EmScale;
        fontData.Glyphs[cp] = glyph;
    }

    // Verify glyphs have valid bounding boxes.
    for (const auto& [cp, glyph] : fontData.Glyphs)
    {
        EXPECT_LT(glyph.PlaneBoundsLeft, glyph.PlaneBoundsRight)
            << "Glyph " << cp << " has degenerate x bounds";
        EXPECT_LT(glyph.PlaneBoundsBottom, glyph.PlaneBoundsTop)
            << "Glyph " << cp << " has degenerate y bounds";
    }
}

TEST_F(SlugFontProcessorTest, ExtractCurvesProducesNonEmptyForVisibleGlyphs)
{
    // We can't call the private ExtractCurves directly, but we can verify
    // that stb_truetype reports shapes for standard glyphs.
    for (u32 cp : { 0x41u, 0x48u, 0x4Fu }) // A, H, O
    {
        const int gi = stbtt_FindGlyphIndex(&m_FontInfo, static_cast<int>(cp));
        ASSERT_NE(gi, 0);

        stbtt_vertex* vertices = nullptr;
        const int vertexCount = stbtt_GetGlyphShape(&m_FontInfo, gi, &vertices);
        EXPECT_GT(vertexCount, 0) << "Glyph " << cp << " should have outline data";

        // Verify at least one MOVE and one CURVE/LINE.
        bool hasMove = false;
        bool hasCurve = false;
        for (int i = 0; i < vertexCount; ++i)
        {
            if (vertices[i].type == STBTT_vmove)
                hasMove = true;
            if (vertices[i].type == STBTT_vcurve || vertices[i].type == STBTT_vline)
                hasCurve = true;
        }
        EXPECT_TRUE(hasMove) << "Glyph " << cp << " should have MOVE vertices";
        EXPECT_TRUE(hasCurve) << "Glyph " << cp << " should have CURVE/LINE vertices";

        stbtt_FreeShape(&m_FontInfo, vertices);
    }
}

TEST_F(SlugFontProcessorTest, SpaceGlyphHasNoCurves)
{
    const int gi = stbtt_FindGlyphIndex(&m_FontInfo, ' ');
    stbtt_vertex* vertices = nullptr;
    const int vertexCount = stbtt_GetGlyphShape(&m_FontInfo, gi, &vertices);
    // Space typically has no outline.
    EXPECT_EQ(vertexCount, 0);
    if (vertices)
    {
        stbtt_FreeShape(&m_FontInfo, vertices);
    }
}

// ---------------------------------------------------------------------------
// SlugGlyphRenderData validation tests
// ---------------------------------------------------------------------------

TEST(SlugGlyphRenderDataTest, DefaultValuesAreZero)
{
    SlugGlyphRenderData rd;
    EXPECT_EQ(rd.BandTextureX, 0u);
    EXPECT_EQ(rd.BandTextureY, 0u);
    EXPECT_EQ(rd.HBandCount, 0u);
    EXPECT_EQ(rd.VBandCount, 0u);
    EXPECT_FLOAT_EQ(rd.BandScaleX, 0.0f);
    EXPECT_FLOAT_EQ(rd.BandScaleY, 0.0f);
}

TEST(SlugCurveTest, ControlPointStorage)
{
    SlugCurve curve;
    curve.P1 = { 0.0f, 0.0f };
    curve.P2 = { 0.5f, 1.0f };
    curve.P3 = { 1.0f, 0.0f };

    EXPECT_FLOAT_EQ(curve.P1.x, 0.0f);
    EXPECT_FLOAT_EQ(curve.P2.y, 1.0f);
    EXPECT_FLOAT_EQ(curve.P3.x, 1.0f);
}

// ---------------------------------------------------------------------------
// Font::MeasureLine — UTF-8-aware line-width measurement.
// Mirrors Renderer2D::DrawString's advancement logic. The previous in-house
// helper in UIRenderer iterated bytes instead of codepoints, which silently
// double-charged the width of every non-ASCII character — these tests pin
// the UTF-8 / fallback / whitespace contract that fix relies on.
// ---------------------------------------------------------------------------

namespace
{
    // Try to load the default font. Returns null if no on-disk candidate
    // path matches the current working directory.
    Ref<Font> TryLoadDefaultFont()
    {
        // Font::GetDefault searches a small set of candidate paths internally
        // (the same set as the engine uses), so we can rely on it for the
        // test runner whether it's launched from the repo root or another
        // working directory.
        auto font = Font::GetDefault();
        return (font && font->IsLoaded()) ? font : Ref<Font>{};
    }

    // The fsScale factor (em → local space) used by Renderer2D::DrawString.
    f32 FsScaleFromMetrics(const SlugFontMetrics& metrics)
    {
        const double span = static_cast<double>(metrics.AscenderY) - static_cast<double>(metrics.DescenderY);
        return std::abs(span) > 1e-6 ? static_cast<f32>(1.0 / span) : 1.0f;
    }
} // namespace

TEST(FontMeasureLineTest, EmptyStringReturnsZero)
{
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    EXPECT_FLOAT_EQ(font->MeasureLine("", 1.0f, 0.0f), 0.0f);
}

TEST(FontMeasureLineTest, AsciiMeasurementIsPositive)
{
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    const auto* data = font->GetSlugData();
    ASSERT_NE(data, nullptr);
    const f32 fsScale = FsScaleFromMetrics(data->Metrics);

    EXPECT_GT(font->MeasureLine("hello", fsScale, 0.0f), 0.0f);
}

TEST(FontMeasureLineTest, Utf8CharCountsAsOneGlyph)
{
    // Regression guard for the UIRenderer byte-iteration bug:
    // 'é' is U+00E9, encoded as 0xC3 0xA9 in UTF-8. A byte-iterating
    // measurer charges TWO Latin-1 glyphs (Ã + ©) instead of one.
    // The codepoint-aware path must measure roughly one glyph wide.
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    const auto* data = font->GetSlugData();
    ASSERT_NE(data, nullptr);
    const f32 fsScale = FsScaleFromMetrics(data->Metrics);

    const f32 widthAccented = font->MeasureLine("\xC3\xA9", fsScale, 0.0f); // "é"
    const f32 widthTwoLatin = font->MeasureLine("ee", fsScale, 0.0f);
    ASSERT_GT(widthAccented, 0.0f);
    ASSERT_GT(widthTwoLatin, 0.0f);

    // One 'é' must be clearly narrower than two ASCII glyphs.
    EXPECT_LT(widthAccented, widthTwoLatin * 0.75f)
        << "'é' (2 bytes UTF-8) measured as " << widthAccented
        << ", expected < " << (widthTwoLatin * 0.75f)
        << " — looks like a byte-iteration regression";
}

TEST(FontMeasureLineTest, CarriageReturnIsSkipped)
{
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    const auto* data = font->GetSlugData();
    ASSERT_NE(data, nullptr);
    const f32 fsScale = FsScaleFromMetrics(data->Metrics);

    EXPECT_FLOAT_EQ(font->MeasureLine("hi", fsScale, 0.0f),
                    font->MeasureLine("h\ri", fsScale, 0.0f));
}

TEST(FontMeasureLineTest, TabExpandsToFourSpaces)
{
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    const auto* data = font->GetSlugData();
    ASSERT_NE(data, nullptr);
    const f32 fsScale = FsScaleFromMetrics(data->Metrics);

    const f32 widthTab = font->MeasureLine("\t", fsScale, 0.0f);
    const f32 widthFourSpaces = font->MeasureLine("    ", fsScale, 0.0f);
    // Tab is defined as exactly 4 × (fsScale * spaceAdvance + kerning).
    // With kerning=0 and a fixed space advance these should match closely;
    // a small absolute tolerance covers any FP rounding difference.
    EXPECT_NEAR(widthTab, widthFourSpaces, 1e-4f);
}

TEST(FontMeasureLineTest, MissingGlyphFallsBackToQuestion)
{
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    const auto* data = font->GetSlugData();
    ASSERT_NE(data, nullptr);
    if (!data->GetGlyph('?'))
    {
        GTEST_SKIP() << "Default font lacks '?' — fallback target unavailable";
    }
    const f32 fsScale = FsScaleFromMetrics(data->Metrics);

    // U+4E2D ('中', CJK) is outside the default Latin-1 range, so the font
    // has no glyph. With no fallback chain configured, Font::MeasureLine
    // must substitute '?'.
    const f32 widthCJK = font->MeasureLine("\xE4\xB8\xAD", fsScale, 0.0f);
    const f32 widthQuestion = font->MeasureLine("?", fsScale, 0.0f);
    EXPECT_FLOAT_EQ(widthCJK, widthQuestion);
}

TEST(FontMeasureLineTest, MultipleCodepointsAccumulate)
{
    auto font = TryLoadDefaultFont();
    if (!font)
    {
        GTEST_SKIP() << "Default font unavailable in CWD";
    }
    const auto* data = font->GetSlugData();
    ASSERT_NE(data, nullptr);
    const f32 fsScale = FsScaleFromMetrics(data->Metrics);

    // Longer strings must measure strictly wider than shorter prefixes
    // (assuming the font has the glyphs and no negative kerning swings it
    // negative — true for OpenSans on this test set).
    const f32 widthA = font->MeasureLine("A", fsScale, 0.0f);
    const f32 widthAB = font->MeasureLine("AB", fsScale, 0.0f);
    const f32 widthABC = font->MeasureLine("ABC", fsScale, 0.0f);
    EXPECT_LT(widthA, widthAB);
    EXPECT_LT(widthAB, widthABC);
}

// ---------------------------------------------------------------------------
// Deferred GPU texture upload (issue #520)
//
// A font parsed without a live GL context (a headless metrics-only load — e.g.
// Font::GetDefault() called from one of the plain FontMeasureLineTest bodies
// above, before any GPU test brings the renderer up) skips its curve/band GPU
// texture creation and retains the packed texel data instead. It must upload
// those textures lazily the first time it is rendered with a context bound.
// Without this, such a font was cached as permanently textureless and silently
// dropped ALL of its text on every later render — the cross-test dropout that
// made RebindMenuScene.RendersRebindPanelAndProducesPng fail (maxLum 0.269,
// button-fill grey with no glyphs) only in full-suite runs.
//
// This drives SlugFontProcessor::EnsureGpuTextures directly (the mechanism
// Font::GetCurveTexture()/GetBandTexture() invoke) so the guard is
// deterministic and order-independent, unlike the full-suite integration
// symptom. SKIPs cleanly on headless CI.
// ---------------------------------------------------------------------------
TEST(SlugDeferredUploadTest, EnsureGpuTexturesUploadsRetainedData)
{
    OLO_ENSURE_GPU_OR_SKIP();

    SlugFontData data;
    data.GpuUploadPending = true;
    data.PendingCurveWidth = 2;
    data.PendingCurveHeight = 1;
    data.PendingCurveTexels.assign(static_cast<sizet>(2) * 4, 0.25f); // RGBA16F: 4 floats/texel
    data.PendingBandWidth = 2;
    data.PendingBandHeight = 1;
    data.PendingBandTexels.assign(static_cast<sizet>(2) * 2, static_cast<u16>(3)); // RG16UI: 2 u16s/texel

    ASSERT_EQ(data.CurveTexture.Raw(), nullptr);
    ASSERT_EQ(data.BandTexture.Raw(), nullptr);

    SlugFontProcessor::EnsureGpuTextures(data);

    EXPECT_NE(data.CurveTexture.Raw(), nullptr) << "deferred curve texture was not uploaded once a context existed";
    EXPECT_NE(data.BandTexture.Raw(), nullptr) << "deferred band texture was not uploaded once a context existed";
    EXPECT_FALSE(data.GpuUploadPending) << "pending flag not cleared after upload";
    EXPECT_TRUE(data.PendingCurveTexels.empty()) << "retained curve texels not freed after upload";
    EXPECT_TRUE(data.PendingBandTexels.empty()) << "retained band texels not freed after upload";

    // Idempotent: a second call with nothing pending must not re-create the texture.
    const Texture2D* const curveBefore = data.CurveTexture.Raw();
    SlugFontProcessor::EnsureGpuTextures(data);
    EXPECT_EQ(data.CurveTexture.Raw(), curveBefore) << "second call re-created the texture instead of no-op";
}
