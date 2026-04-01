#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/SlugData.h"
#include "OloEngine/Renderer/SlugFontProcessor.h"

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
