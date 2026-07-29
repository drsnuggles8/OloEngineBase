#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpCaptureRegionTest — unit test (headless, no GL, no live editor).
//
// Pins the shared `region: {x, y, w, h}` argument that olo_render_capture_target
// and olo_screenshot grew for issue #607's native-resolution-capture gap.
//
// Why it exists: both tools rescale the WHOLE target so its width is at most
// `maxWidth` (hard-clamped to 4096), and a pixel-scale artifact can only be
// measured on 1:1 pixels. Root-causing the GTAO "goosebumps" weave meant taking
// the spatial autocorrelation of the AO buffer to find the noise's period — but a
// 4291x2320 target only came back resampled, which moved the diagnostic 64 px
// Hilbert-LUT tile period to 61.1 px and inflated short-lag correlation, so the
// first measurement could not tell the regression from the fix.
//
// The two invariants that make a region trustworthy, and that this pins:
//   * a degenerate rect is REJECTED, not folded into "the whole image" — a caller
//     that computed an empty rect must hear about it rather than silently receive
//     a full-target capture it will then mis-measure;
//   * the reply states `nativeResolution` as a FACT derived from the encoded
//     size, not as something the caller must infer from maxWidth arithmetic.
//
// The GL readback itself (the top-left -> bottom-up row flip in
// GPUResourceInspector::CaptureTexturePng) needs a context and is covered by the
// live editor verification, not here.
// =============================================================================

#include "MCP/McpCaptureRegion.h"

#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::MCP::McpCaptureRegion;
    namespace RegionArg = OloEngine::MCP::CaptureRegionArg;
    using Json = nlohmann::json;

    Json Rect(long long x, long long y, long long w, long long h)
    {
        return Json{ { "region", { { "x", x }, { "y", y }, { "w", w }, { "h", h } } } };
    }
} // namespace

TEST(McpCaptureRegion, AbsentRegionMeansWholeImage)
{
    McpCaptureRegion region;
    EXPECT_FALSE(RegionArg::Parse(Json::object(), region).has_value());
    EXPECT_TRUE(region.IsWholeImage());
    EXPECT_EQ(region, McpCaptureRegion{});

    // An explicit null is the same as absent (clients that spell out every key).
    McpCaptureRegion nulled;
    EXPECT_FALSE(RegionArg::Parse(Json{ { "region", nullptr } }, nulled).has_value());
    EXPECT_TRUE(nulled.IsWholeImage());
}

TEST(McpCaptureRegion, ParsesAWellFormedRect)
{
    McpCaptureRegion region;
    ASSERT_FALSE(RegionArg::Parse(Rect(128, 64, 512, 256), region).has_value());
    EXPECT_EQ(region.X, 128u);
    EXPECT_EQ(region.Y, 64u);
    EXPECT_EQ(region.Width, 512u);
    EXPECT_EQ(region.Height, 256u);
    EXPECT_FALSE(region.IsWholeImage());
}

TEST(McpCaptureRegion, RejectsDegenerateAndNegativeRects)
{
    McpCaptureRegion region;

    // Zero extent must NOT quietly mean "the whole image" — see the header note.
    EXPECT_TRUE(RegionArg::Parse(Rect(0, 0, 0, 64), region).has_value());
    EXPECT_TRUE(RegionArg::Parse(Rect(0, 0, 64, 0), region).has_value());
    EXPECT_TRUE(RegionArg::Parse(Rect(0, 0, -8, 64), region).has_value());
    EXPECT_TRUE(RegionArg::Parse(Rect(-1, 0, 64, 64), region).has_value());
    EXPECT_TRUE(RegionArg::Parse(Rect(0, -1, 64, 64), region).has_value());

    // Every rejection leaves the caller's region untouched (whole image).
    EXPECT_TRUE(region.IsWholeImage());
}

TEST(McpCaptureRegion, RejectsValuesTooLargeForU32)
{
    McpCaptureRegion region;
    constexpr long long kU32Max = 4294967295LL;
    constexpr long long kOverflow = kU32Max + 1; // 2^32

    // The dangerous one: 2^32 narrows to 0, and a Width/Height of 0 reads as
    // IsWholeImage() — so without the bound check a bogus request silently
    // becomes a full-target capture the caller then mis-measures.
    for (const char* field : { "x", "y", "w", "h" })
    {
        Json rect = Json{ { "x", 0 }, { "y", 0 }, { "w", 8 }, { "h", 8 } };
        rect[field] = kOverflow;
        const auto error = RegionArg::Parse(Json{ { "region", rect } }, region);
        ASSERT_TRUE(error.has_value()) << field << " = 2^32 was accepted";
        EXPECT_NE(error->find(field), std::string::npos) << "the error should name the offending field";
        EXPECT_TRUE(region.IsWholeImage()) << "a rejected parse must leave the caller's region untouched";
    }

    // u32::max() itself is in range and must still parse (the boundary is
    // inclusive — an off-by-one here would reject a legal maximum).
    ASSERT_FALSE(RegionArg::Parse(Rect(kU32Max, kU32Max, kU32Max, kU32Max), region).has_value());
    EXPECT_EQ(region.Width, 4294967295u);
    EXPECT_EQ(region.Height, 4294967295u);

    // A JSON *unsigned* beyond signed range must be caught on its own terms,
    // not round-tripped through a signed get<> first.
    region = McpCaptureRegion{};
    Json huge = Json{ { "x", 0 }, { "y", 0 }, { "w", 8 }, { "h", 8 } };
    huge["w"] = 18446744073709551615ull;
    EXPECT_TRUE(RegionArg::Parse(Json{ { "region", huge } }, region).has_value());
    EXPECT_TRUE(region.IsWholeImage());
}

TEST(McpCaptureRegion, RejectsMalformedShapes)
{
    McpCaptureRegion region;

    EXPECT_TRUE(RegionArg::Parse(Json{ { "region", 42 } }, region).has_value());
    EXPECT_TRUE(RegionArg::Parse(Json{ { "region", Json::array({ 0, 0, 8, 8 }) } }, region).has_value());
    // A missing key is named in the error so the caller can fix it directly.
    const auto missing = RegionArg::Parse(Json{ { "region", { { "x", 0 }, { "y", 0 }, { "w", 8 } } } }, region);
    ASSERT_TRUE(missing.has_value());
    EXPECT_NE(missing->find("'h'"), std::string::npos);
    // Floats are not texel coordinates.
    EXPECT_TRUE(RegionArg::Parse(Json{ { "region", { { "x", 0.5 }, { "y", 0 }, { "w", 8 }, { "h", 8 } } } }, region).has_value());
}

TEST(McpCaptureRegion, MetaReportsWhetherThePngIsOneToOne)
{
    const McpCaptureRegion region{ 10, 20, 512, 256 };

    // Encoded at the region's own size => genuinely native resolution.
    const Json native = RegionArg::MetaJson(region, 512, 256);
    EXPECT_EQ(native["x"], 10u);
    EXPECT_EQ(native["y"], 20u);
    EXPECT_EQ(native["w"], 512u);
    EXPECT_EQ(native["h"], 256u);
    EXPECT_EQ(native["nativeResolution"], Json(true));

    // Downscaled (region wider than maxWidth) => the caller must NOT treat the
    // pixels as 1:1; this flag is the only honest signal of that.
    const Json scaled = RegionArg::MetaJson(region, 256, 128);
    EXPECT_EQ(scaled["nativeResolution"], Json(false));
}

TEST(McpCaptureRegion, SchemaIsAClosedObjectRequiringAllFourFields)
{
    const Json schema = RegionArg::SchemaNode();
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["additionalProperties"], false);
    for (const char* field : { "x", "y", "w", "h" })
    {
        ASSERT_TRUE(schema["properties"].contains(field));
        EXPECT_EQ(schema["properties"][field]["type"], "integer");
    }
    // w/h are exclusive of zero at the schema level too, so a well-behaved client
    // is stopped before the request is even sent.
    EXPECT_EQ(schema["properties"]["w"]["minimum"], 1);
    EXPECT_EQ(schema["properties"]["h"]["minimum"], 1);
    EXPECT_EQ(schema["properties"]["x"]["minimum"], 0);
    ASSERT_TRUE(schema.contains("required"));
    EXPECT_EQ(schema["required"].size(), 4u);
}
