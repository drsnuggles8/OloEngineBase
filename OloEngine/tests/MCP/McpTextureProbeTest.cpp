// OLO_TEST_LAYER: unit
//
// Request parsing, read planning and result shaping behind `olo_texture_probe`
// (issue #607, gap logged from #1078). The read itself needs a live GPU and an
// asset manager; what an agent acts on is decided in MCP/McpTextureProbe.h:
//
//   * BCn is refused by name rather than read as zeros;
//   * an 8-bit texel says which value it is (raw / encoded / linear);
//   * the region is bounded and checked against the mip;
//   * image origin flips rows back, because the loaders store them bottom-up.

#include "OloEnginePCH.h"

#include "MCP/McpTextureProbe.h"

#include <gtest/gtest.h>

namespace Probe = OloEngine::MCP::TextureProbe;
using OloEngine::ImageFormat;
using Json = nlohmann::json;

namespace
{
    Probe::Request Parse(const Json& args)
    {
        Probe::Request request;
        const auto error = Probe::ParseRequest(args, request);
        EXPECT_FALSE(error.has_value()) << *error;
        return request;
    }

    std::string ParseError(const Json& args)
    {
        Probe::Request request;
        const auto error = Probe::ParseRequest(args, request);
        EXPECT_TRUE(error.has_value()) << "expected a refusal for " << args.dump();
        return error.value_or("");
    }
} // namespace

TEST(McpTextureProbe, ParsesHandleOrPathAndDefaultsToOneTexel)
{
    const auto byHandle = Parse(Json{ { "handle", "12345" } });
    EXPECT_EQ(byHandle.Handle, 12345u);
    EXPECT_EQ(byHandle.Width, 1u);
    EXPECT_EQ(byHandle.Height, 1u);
    EXPECT_EQ(byHandle.Mip, 0u);
    EXPECT_FALSE(byHandle.RequestedOrigin.has_value());

    const auto byPath = Parse(Json{ { "path", "Textures/a.png" }, { "x", 3 }, { "y", 4 }, { "w", 2 }, { "h", 8 }, { "mip", 1 }, { "origin", "storage" } });
    EXPECT_EQ(byPath.Path, "Textures/a.png");
    EXPECT_EQ(byPath.X, 3u);
    EXPECT_EQ(byPath.Y, 4u);
    EXPECT_EQ(byPath.Width, 2u);
    EXPECT_EQ(byPath.Height, 8u);
    EXPECT_EQ(byPath.Mip, 1u);
    EXPECT_EQ(byPath.RequestedOrigin, Probe::Origin::Storage);
}

TEST(McpTextureProbe, RejectsAmbiguousOrMalformedRequests)
{
    EXPECT_NE(ParseError(Json::object()).find("exactly one"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "handle", "1" }, { "path", "a.png" } }).find("exactly one"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "handle", "0" } }).find("handle"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "handle", "1" }, { "w", 0 } }).find("'w'"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "handle", "1" }, { "x", -1 } }).find("'x'"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "handle", "1" }, { "x", "3" } }).find("'x'"), std::string::npos)
        << "a string where a number belongs must not silently become 0";
    EXPECT_NE(ParseError(Json{ { "handle", "1" }, { "origin", "top" } }).find("origin"), std::string::npos);
}

TEST(McpTextureProbe, RegionIsBounded)
{
    (void)Parse(Json{ { "handle", "1" }, { "w", 16 }, { "h", 16 } });
    const auto error = ParseError(Json{ { "handle", "1" }, { "w", 17 }, { "h", 16 } });
    EXPECT_NE(error.find("at most 256"), std::string::npos);
}

// THE REFUSAL THAT MATTERS: neither backend's readback decodes BCn, and a probe
// that answered zeros would report a black texture.
TEST(McpTextureProbe, BlockCompressedFormatsAreRefusedByName)
{
    for (const ImageFormat format : { ImageFormat::BC4, ImageFormat::BC5, ImageFormat::BC6H, ImageFormat::BC6HS, ImageFormat::BC7 })
    {
        const auto plan = Probe::PlanRead(format);
        EXPECT_FALSE(plan.Ok) << Probe::FormatToken(format);
        EXPECT_NE(plan.Refusal.find(Probe::FormatToken(format)), std::string::npos) << plan.Refusal;
        EXPECT_NE(plan.Refusal.find("block-compressed"), std::string::npos) << plan.Refusal;
    }
    EXPECT_FALSE(Probe::PlanRead(ImageFormat::DEPTH24STENCIL8).Ok);
    EXPECT_FALSE(Probe::PlanRead(ImageFormat::RGBA32UI).Ok);
    EXPECT_FALSE(Probe::PlanRead(ImageFormat::None).Ok);
}

TEST(McpTextureProbe, PlansMatchTheFormat)
{
    const auto rgba8 = Probe::PlanRead(ImageFormat::RGBA8);
    EXPECT_TRUE(rgba8.Ok);
    EXPECT_EQ(rgba8.Kind, Probe::ValueKind::UNorm8);
    EXPECT_EQ(rgba8.Channels, 4u);
    EXPECT_EQ(rgba8.ReadChannels, 4u);

    // Three channels read through the four-channel destination.
    const auto rgb8 = Probe::PlanRead(ImageFormat::RGB8);
    EXPECT_EQ(rgb8.Channels, 3u);
    EXPECT_EQ(rgb8.ReadChannels, 4u);

    const auto rg16f = Probe::PlanRead(ImageFormat::RG16F);
    EXPECT_EQ(rg16f.Kind, Probe::ValueKind::Float);
    EXPECT_EQ(rg16f.ReadChannels, 2u);

    const auto r32ui = Probe::PlanRead(ImageFormat::R32UI);
    EXPECT_EQ(r32ui.Kind, Probe::ValueKind::Int);
    EXPECT_EQ(r32ui.ReadChannels, 1u);
}

TEST(McpTextureProbe, RegionMustFitTheMip)
{
    const auto request = Parse(Json{ { "handle", "1" }, { "x", 6 }, { "y", 0 }, { "w", 3 }, { "h", 1 }, { "mip", 2 } });
    const auto mapped = Probe::MapRegion(request, Probe::Origin::Storage, 8, 8);
    EXPECT_FALSE(mapped.Ok);
    EXPECT_NE(mapped.Error.find("8x8"), std::string::npos);
    EXPECT_NE(mapped.Error.find("mip 2"), std::string::npos);
}

// Loaders store rows bottom-up, so image row 0 is the last storage row. A 2-row
// region at image y=1 of a 4-row mip covers storage rows 1..2.
TEST(McpTextureProbe, ImageOriginFlipsRows)
{
    const auto request = Parse(Json{ { "handle", "1" }, { "y", 1 }, { "h", 2 } });
    EXPECT_EQ(Probe::MapRegion(request, Probe::Origin::Image, 4, 4).StorageY, 1u);

    const auto top = Parse(Json{ { "handle", "1" }, { "y", 0 }, { "h", 1 } });
    EXPECT_EQ(Probe::MapRegion(top, Probe::Origin::Image, 4, 4).StorageY, 3u);
    EXPECT_EQ(Probe::MapRegion(top, Probe::Origin::Storage, 4, 4).StorageY, 0u);
}

TEST(McpTextureProbe, ImageOriginReportsRowsTopDown)
{
    // Two rows read from storage: storage row 0 = 0.1, storage row 1 = 0.9.
    // In image origin the first reported row is the higher storage row.
    const auto request = Parse(Json{ { "handle", "1" }, { "h", 2 } });
    const auto plan = Probe::PlanRead(ImageFormat::R32F);
    Probe::Texture texture;
    texture.Format = ImageFormat::R32F;
    const std::vector<f32> floats{ 0.1f, 0.9f };

    const auto report = Probe::BuildReport(texture, request, plan, Probe::Origin::Image, 1, 2, floats, {});
    ASSERT_EQ(report.at("texels").size(), 2u);
    EXPECT_EQ(report.at("texels")[0].at("y").get<u32>(), 0u);
    EXPECT_FLOAT_EQ(report.at("texels")[0].at("value")[0].get<f32>(), 0.9f);
    EXPECT_FLOAT_EQ(report.at("texels")[1].at("value")[0].get<f32>(), 0.1f);
}

// An 8-bit texel names all three readings, and the linear one only when the
// texture is sRGB. Alpha is never decoded.
TEST(McpTextureProbe, SrgbTexelCarriesRawEncodedAndLinear)
{
    const auto request = Parse(Json{ { "handle", "1" } });
    const auto plan = Probe::PlanRead(ImageFormat::RGBA8);
    Probe::Texture texture;
    texture.Format = ImageFormat::RGBA8;
    texture.SRGB = true;
    const std::vector<f32> floats{ 188.0f / 255.0f, 0.0f, 1.0f, 128.0f / 255.0f };

    const auto report = Probe::BuildReport(texture, request, plan, Probe::Origin::Image, 1, 1, floats, {});
    EXPECT_EQ(report.at("valueKind").get<std::string>(), "unorm8");
    EXPECT_NE(report.at("reads").get<std::string>().find("sRGB-encoded"), std::string::npos);
    const auto& texel = report.at("texels")[0];
    EXPECT_EQ(texel.at("raw"), Json::array({ 188, 0, 255, 128 }));
    EXPECT_NEAR(texel.at("linear")[0].get<f32>(), 0.5029f, 1e-3f); // sRGB 188 is ~0.503 linear
    EXPECT_FLOAT_EQ(texel.at("linear")[2].get<f32>(), 1.0f);
    EXPECT_FLOAT_EQ(texel.at("linear")[3].get<f32>(), 128.0f / 255.0f) << "alpha is stored linear";

    texture.SRGB = false;
    const auto linearTexture = Probe::BuildReport(texture, request, plan, Probe::Origin::Image, 1, 1, floats, {});
    EXPECT_FALSE(linearTexture.at("texels")[0].contains("linear"));
    EXPECT_NE(linearTexture.at("reads").get<std::string>().find("not sRGB"), std::string::npos);
}

TEST(McpTextureProbe, UnsignedIntegerTexelsKeepTheirBits)
{
    const auto request = Parse(Json{ { "handle", "1" } });
    Probe::Texture texture;
    texture.Format = ImageFormat::R32UI;
    const std::vector<i32> ints{ static_cast<i32>(0xFFFFFFF0u) };

    const auto report = Probe::BuildReport(texture, request, Probe::PlanRead(ImageFormat::R32UI), Probe::Origin::Storage, 1, 1, {}, ints);
    EXPECT_EQ(report.at("texels")[0].at("value").get<u32>(), 0xFFFFFFF0u);
}
