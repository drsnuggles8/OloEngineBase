// OLO_TEST_LAYER: L3
//
// The per-texture ".oloimport" sidecar (#624 item 2) — the signal that makes BC5
// reachable from an automatic cook without guessing from a filename.
//
// Two halves are pinned here: the sidecar format itself (round trip, defaults, and that a
// malformed or version-skewed file is REJECTED rather than half-read), and the effect on
// the cook — a source PNG with a "Format: BC5" sidecar must actually come out BC5, and the
// same PNG with no sidecar must still come out BC7, because a project with no sidecars has
// to cook exactly as it did before this existed.

#include "OloEngine/Renderer/TextureCompression.h"
#include "OloEngine/Renderer/TextureImportSettings.h"

#include <gtest/gtest.h>
#include "TestTempDir.h"
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine;

namespace
{
    // A two-channel-meaningful RGB source: R/G sweep with a constant B, i.e. what a
    // tangent-space normal map looks like once packed.
    std::vector<u8> MakeNormalLikeRGB(u32 width, u32 height)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 3);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 3];
                p[0] = static_cast<u8>((x * 255) / std::max(1u, width - 1));
                p[1] = static_cast<u8>((y * 255) / std::max(1u, height - 1));
                p[2] = 255;
            }
        }
        return pixels;
    }

    // Write a source PNG into the shared temp dir and return its path.
    std::filesystem::path WriteSourcePng(const char* suffix, u32 width, u32 height)
    {
        const std::filesystem::path path = OloEngine::Tests::TempFile(std::string("import") + suffix + ".png");
        const std::vector<u8> pixels = MakeNormalLikeRGB(width, height);
        EXPECT_NE(::stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 3,
                                   pixels.data(), static_cast<int>(width) * 3),
                  0);
        return path;
    }

    void WriteSidecar(const std::filesystem::path& imagePath, std::string_view body)
    {
        std::ofstream file(TextureImport::SidecarPathFor(imagePath.string()), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(static_cast<bool>(file));
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    void RemoveBoth(const std::filesystem::path& imagePath)
    {
        std::error_code ec;
        std::filesystem::remove(TextureImport::SidecarPathFor(imagePath.string()), ec);
        std::filesystem::remove(imagePath, ec);
    }
} // namespace

TEST(TextureImportSettings, SidecarPathAppendsRatherThanReplacesTheExtension)
{
    // Foo.png and Foo.tga in one directory must not share one settings file.
    EXPECT_EQ(TextureImport::SidecarPathFor("Assets/T/Foo.png"), "Assets/T/Foo.png.oloimport");
    EXPECT_EQ(TextureImport::SidecarPathFor("Assets/T/Foo.tga"), "Assets/T/Foo.tga.oloimport");
}

TEST(TextureImportSettings, EmitParseRoundTrip)
{
    TextureImportSettings original;
    original.Format = TextureImportSettings::FormatChoice::BC5;
    original.ColorSpace = TextureImportSettings::ColorSpaceChoice::Linear;
    original.GenerateMips = false;

    TextureImportSettings restored;
    ASSERT_TRUE(TextureImport::Parse(TextureImport::Emit(original), restored));
    EXPECT_EQ(restored.Format, TextureImportSettings::FormatChoice::BC5);
    EXPECT_EQ(restored.ColorSpace, TextureImportSettings::ColorSpaceChoice::Linear);
    ASSERT_TRUE(restored.GenerateMips.has_value());
    EXPECT_FALSE(*restored.GenerateMips);
}

TEST(TextureImportSettings, OmittedFieldsMeanAuto)
{
    TextureImportSettings settings;
    ASSERT_TRUE(TextureImport::Parse("TextureImportSettings:\n  Version: 1\n", settings));
    EXPECT_TRUE(settings.IsAllAuto());
}

TEST(TextureImportSettings, RejectsUnknownSpellingsAndVersions)
{
    // A typo must be a loud failure, not a silent fall back to Auto — the whole point of
    // the sidecar is that the format was chosen deliberately.
    TextureImportSettings settings;
    EXPECT_FALSE(TextureImport::Parse("TextureImportSettings:\n  Format: BC9\n", settings));
    EXPECT_FALSE(TextureImport::Parse("TextureImportSettings:\n  ColorSpace: Rec709\n", settings));
    EXPECT_FALSE(TextureImport::Parse("TextureImportSettings:\n  Version: 99\n  Format: BC5\n", settings));
    EXPECT_FALSE(TextureImport::Parse("NotOurRoot:\n  Format: BC5\n", settings));
    EXPECT_FALSE(TextureImport::Parse("this: [is: not: yaml", settings));
}

TEST(TextureImportSettings, MissingSidecarIsNotAnError)
{
    TextureImportSettings settings;
    // Reported by the return value; no sidecar is the normal case for most textures.
    EXPECT_FALSE(TextureImport::LoadForImage("Assets/does/not/exist.png", settings));
    EXPECT_TRUE(settings.IsAllAuto());
}

TEST(TextureImportSettings, SaveThenLoadForImage)
{
    const std::filesystem::path png = WriteSourcePng("_saveload", 16, 16);

    TextureImportSettings written;
    written.Format = TextureImportSettings::FormatChoice::BC6HSigned;
    ASSERT_TRUE(TextureImport::SaveForImage(png.string(), written));

    TextureImportSettings read;
    ASSERT_TRUE(TextureImport::LoadForImage(png.string(), read));
    EXPECT_EQ(read.Format, TextureImportSettings::FormatChoice::BC6HSigned);

    RemoveBoth(png);
}

TEST(TextureImportSettings, SidecarSteersTheCookToBC5)
{
    // The item-2 behaviour: an automatic cook (Format = None) picks BC7 for an LDR PNG,
    // and only an explicit per-texture setting can make it BC5.
    const std::filesystem::path png = WriteSourcePng("_bc5", 32, 32);

    TextureCompression::CompressOptions autoOptions; // Format = None
    CompressedTextureImage withoutSidecar;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), autoOptions, withoutSidecar));
    EXPECT_EQ(withoutSidecar.Format, TextureCompressionFormat::BC7)
        << "a texture with no sidecar must cook exactly as it did before sidecars existed";

    WriteSidecar(png, "TextureImportSettings:\n  Version: 1\n  Format: BC5\n");
    CompressedTextureImage withSidecar;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), autoOptions, withSidecar));
    EXPECT_EQ(withSidecar.Format, TextureCompressionFormat::BC5);
    EXPECT_FALSE(withSidecar.SRGB);

    RemoveBoth(png);
}

TEST(TextureImportSettings, ExplicitOptionsOutrankTheSidecar)
{
    const std::filesystem::path png = WriteSourcePng("_explicit", 16, 16);
    WriteSidecar(png, "TextureImportSettings:\n  Version: 1\n  Format: BC5\n");

    TextureCompression::CompressOptions options;
    options.Format = TextureCompressionFormat::BC7; // caller was explicit
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC7);

    RemoveBoth(png);
}

TEST(TextureImportSettings, SidecarGenerateMipsBeatsTheCallersValue)
{
    // Precedence differs per field and the reason is worth pinning: `Format` has a real
    // "unset" spelling (None), so an explicit one wins; `GenerateMips` does not, so a
    // caller-passed false is indistinguishable from a default and the sidecar — an
    // authored, per-asset statement — is taken as the more specific one.
    const std::filesystem::path png = WriteSourcePng("_mips", 16, 16);
    WriteSidecar(png, "TextureImportSettings:\n  Version: 1\n  GenerateMips: false\n");

    TextureCompression::CompressOptions options;
    options.GenerateMips = true; // caller asked for a chain; the sidecar says no
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.MipLevels(), 1u);

    RemoveBoth(png);
}

TEST(TextureImportSettings, UseImportSettingsFalseIgnoresTheSidecar)
{
    const std::filesystem::path png = WriteSourcePng("_ignored", 16, 16);
    WriteSidecar(png, "TextureImportSettings:\n  Version: 1\n  Format: BC5\n  GenerateMips: false\n");

    TextureCompression::CompressOptions options;
    options.UseImportSettings = false;
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC7);
    EXPECT_GT(image.MipLevels(), 1u);

    RemoveBoth(png);
}

TEST(TextureImportSettings, SidecarOverridesColorSpaceAgainstTheFilenameHeuristic)
{
    // "Foo_Normal.png" reads as linear data by filename; an explicit sRGB setting must
    // win, because the sidecar is a statement about the asset and the filename is a guess.
    const std::filesystem::path png = OloEngine::Tests::TempFile("import_normal_Normal.png");
    const std::vector<u8> pixels = MakeNormalLikeRGB(16, 16);
    ASSERT_NE(::stbi_write_png(png.string().c_str(), 16, 16, 3, pixels.data(), 16 * 3), 0);

    TextureCompression::CompressOptions options;
    CompressedTextureImage linearByName;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, linearByName));
    EXPECT_FALSE(linearByName.SRGB);

    WriteSidecar(png, "TextureImportSettings:\n  Version: 1\n  ColorSpace: sRGB\n");
    CompressedTextureImage srgbBySidecar;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, srgbBySidecar));
    EXPECT_TRUE(srgbBySidecar.SRGB);

    RemoveBoth(png);
}

TEST(TextureImportSettings, MalformedSidecarFallsBackToAutoAndDoesNotCookTheWrongFormat)
{
    // A malformed sidecar is logged as an error and the cook proceeds automatically — it
    // must never half-apply a broken file (e.g. take Format but drop ColorSpace).
    const std::filesystem::path png = WriteSourcePng("_malformed", 16, 16);
    WriteSidecar(png, "TextureImportSettings:\n  Version: 1\n  Format: BC5\n  ColorSpace: Nonsense\n");

    TextureCompression::CompressOptions options;
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC7);

    RemoveBoth(png);
}
