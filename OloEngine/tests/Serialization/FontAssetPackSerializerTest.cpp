#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// FontAssetPackSerializerTest — font asset-pack round-trip.
//
// Before this was wired up, FontSerializer::SerializeToAssetPack wrote only the
// font's name and DeserializeFromAssetPack returned nullptr with a "not yet
// fully implemented" warning. The practical consequence: any shipped .olopack
// game that used a custom font silently failed to load it at runtime.
//
// The fix embeds the original font-file bytes (plus name + codepoint ranges) in
// the pack and reconstructs the Font from those bytes via the new in-memory
// load path (Font::Create(name, span, ranges)). These tests pin both halves:
//
//   1. MemoryLoadMatchesFileLoad — the in-memory load path produces the same
//      glyph data as loading the same bytes from a file. This is the riskiest
//      new code (LoadFromMemory was extracted from LoadFromFile).
//   2. AssetPackRoundTrip — a real Font goes through SerializeToAssetPack and
//      DeserializeFromAssetPack and comes back loaded, with the same name,
//      ranges and glyph coverage.
//
// All headless: SlugFontProcessor::Process does a metrics-only load (no GPU
// textures) when no GL context is bound, and Font sets m_IsLoaded regardless,
// so glyph metrics round-trip without a renderer. The tests SKIP cleanly if the
// reference .ttf isn't on disk.
// =============================================================================

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/SlugData.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    // Resolve the reference font from one of the working-directory-relative
    // candidates the other font tests use. Returns an absolute path, or empty
    // if none exist (so the caller can SKIP).
    fs::path ResolveReferenceFont()
    {
        const std::array<const char*, 3> candidates = {
            "OloEditor/assets/fonts/opensans/OpenSans-Regular.ttf",
            "../OloEditor/assets/fonts/opensans/OpenSans-Regular.ttf",
            "assets/fonts/opensans/OpenSans-Regular.ttf",
        };
        for (const char* candidate : candidates)
        {
            std::error_code ec;
            if (fs::exists(candidate, ec))
                return fs::weakly_canonical(candidate, ec);
        }
        return {};
    }

    std::vector<u8> ReadFileBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return {};
        const std::streamoff size = file.tellg();
        if (size <= 0)
            return {};
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytes(static_cast<sizet>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (file.fail())
            return {};
        return bytes;
    }

    [[nodiscard]] sizet GlyphCount(const Ref<Font>& font)
    {
        const SlugFontData* data = font->GetSlugData();
        return data ? data->Glyphs.size() : 0u;
    }

    [[nodiscard]] bool RangesEqual(const std::vector<FontCodepointRange>& a, const std::vector<FontCodepointRange>& b)
    {
        const sizet count = a.size();
        if (count != b.size())
            return false;
        for (sizet i = 0; i < count; ++i)
        {
            if (a[i].First != b[i].First || a[i].Last != b[i].Last)
                return false;
        }
        return true;
    }
} // namespace

// -----------------------------------------------------------------------------
// 1. In-memory load equivalence (no AssetManager, no GL).
// -----------------------------------------------------------------------------
TEST(FontMemoryLoadTest, MemoryLoadMatchesFileLoad)
{
    const fs::path fontPath = ResolveReferenceFont();
    if (fontPath.empty())
        GTEST_SKIP() << "OpenSans-Regular.ttf not found — skipping font memory-load test";

    const std::vector<FontCodepointRange> ranges{ FontCodepointRanges::Latin1 };

    // Ranged Create() is uncached, so each call yields a fresh instance — no
    // cross-test sharing through the path cache.
    Ref<Font> fromFile = Font::Create(fontPath, ranges);
    ASSERT_TRUE(fromFile && fromFile->IsLoaded()) << "File load failed for " << fontPath.string();

    const std::vector<u8> bytes = ReadFileBytes(fontPath);
    ASSERT_FALSE(bytes.empty()) << "Failed to read font bytes from " << fontPath.string();

    Ref<Font> fromMemory = Font::Create("OpenSans-Regular", std::span<const u8>(bytes), ranges);
    ASSERT_TRUE(fromMemory && fromMemory->IsLoaded()) << "In-memory load failed";

    EXPECT_EQ(fromMemory->GetName(), "OpenSans-Regular");
    EXPECT_TRUE(fromMemory->GetPath().empty()) << "Memory-loaded font must not invent a file path";
    EXPECT_TRUE(RangesEqual(fromMemory->GetRanges(), ranges));

    // Same bytes → same glyph coverage and metrics, regardless of source.
    EXPECT_GT(GlyphCount(fromFile), 0u);
    EXPECT_EQ(GlyphCount(fromMemory), GlyphCount(fromFile));

    const SlugFontData* fileData = fromFile->GetSlugData();
    const SlugFontData* memData = fromMemory->GetSlugData();
    ASSERT_TRUE(fileData && memData);
    EXPECT_NEAR(memData->Metrics.AscenderY, fileData->Metrics.AscenderY, 1e-6f);
    EXPECT_NEAR(memData->Metrics.DescenderY, fileData->Metrics.DescenderY, 1e-6f);
    EXPECT_NEAR(memData->Metrics.LineHeight, fileData->Metrics.LineHeight, 1e-6f);

    // Spot-check a concrete glyph advance ('A' = U+0041).
    const auto fileGlyph = fileData->Glyphs.find(static_cast<u32>('A'));
    const auto memGlyph = memData->Glyphs.find(static_cast<u32>('A'));
    ASSERT_NE(fileGlyph, fileData->Glyphs.end());
    ASSERT_NE(memGlyph, memData->Glyphs.end());
    EXPECT_NEAR(memGlyph->second.AdvanceWidth, fileGlyph->second.AdvanceWidth, 1e-6f);
}

// Codepoint ranges can arrive from an untrusted asset pack. An unclamped Last of
// 0xFFFFFFFF would wrap `++codepoint` back to 0 and loop forever; an inverted
// range must be dropped. Before the sanitize guard, this test would hang.
TEST(FontMemoryLoadTest, SanitizesOutOfRangeCodepoints)
{
    const fs::path fontPath = ResolveReferenceFont();
    if (fontPath.empty())
        GTEST_SKIP() << "OpenSans-Regular.ttf not found — skipping range-sanitize test";

    const std::vector<u8> bytes = ReadFileBytes(fontPath);
    ASSERT_FALSE(bytes.empty());

    const std::vector<FontCodepointRange> dirty{
        { 0x10FFF0u, 0xFFFFFFFFu }, // huge Last -> clamped to 0x10FFFF (no wrap, ~16 iterations)
        { 100u, 50u },              // inverted -> dropped
    };
    Ref<Font> font = Font::Create("probe", std::span<const u8>(bytes), dirty);
    ASSERT_TRUE(font && font->IsLoaded()) << "load must complete (not hang) and succeed";

    const std::vector<FontCodepointRange>& got = font->GetRanges();
    ASSERT_EQ(got.size(), 1u) << "inverted range must be dropped";
    EXPECT_EQ(got[0].First, 0x10FFF0u);
    EXPECT_EQ(got[0].Last, 0x10FFFFu) << "Last must be clamped to the max Unicode scalar";
}

// -----------------------------------------------------------------------------
// 2. Full asset-pack round-trip through the production serializer.
// -----------------------------------------------------------------------------
class FontAssetPackSerializerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineFontAssetPack" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: FontAssetPackSerializerTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }
        ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        // Filewatcher off: its callback thread races the at-exit dtor of the
        // named-thread manager under TSan, and these tests never hot-reload.
        m_AssetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;
};

TEST_F(FontAssetPackSerializerTest, AssetPackRoundTrip)
{
    const fs::path fontPath = ResolveReferenceFont();
    if (fontPath.empty())
        GTEST_SKIP() << "OpenSans-Regular.ttf not found — skipping font asset-pack round-trip";

    const std::vector<FontCodepointRange> ranges{ FontCodepointRanges::Latin1 };
    Ref<Font> font = Font::Create(fontPath, ranges);
    ASSERT_TRUE(font && font->IsLoaded()) << "File load failed for " << fontPath.string();
    const sizet sourceGlyphs = GlyphCount(font);
    ASSERT_GT(sourceGlyphs, 0u);

    const AssetHandle handle = AssetManager::AddMemoryOnlyAsset(font);
    ASSERT_TRUE(handle) << "AddMemoryOnlyAsset returned a null handle";

    const fs::path packPath = m_TempDir / "font.pack";
    FontSerializer serializer;
    AssetSerializationInfo info{};

    {
        FileStreamWriter writer(packPath);
        ASSERT_TRUE(writer.IsStreamGood());
        ASSERT_TRUE(serializer.SerializeToAssetPack(handle, writer, info))
            << "SerializeToAssetPack should succeed for a loaded font";
    } // writer flushes/closes here

    EXPECT_GT(info.Size, 0u) << "Serialized record must carry the embedded font bytes";

    AssetPackFile::AssetInfo assetInfo{};
    // A distinct handle so we can assert the deserializer applies AssetInfo::Handle.
    assetInfo.Handle = static_cast<AssetHandle>(0xF047C0DEULL);
    assetInfo.PackedOffset = info.Offset;
    assetInfo.PackedSize = info.Size;
    assetInfo.Type = AssetType::Font;

    FileStreamReader reader(packPath);
    ASSERT_TRUE(reader.IsStreamGood());
    Ref<Asset> result = serializer.DeserializeFromAssetPack(reader, assetInfo);

    Ref<Font> roundTripped = result.As<Font>();
    ASSERT_TRUE(roundTripped) << "DeserializeFromAssetPack returned null/non-Font";
    EXPECT_TRUE(roundTripped->IsLoaded()) << "Round-tripped font failed to load from embedded bytes";
    EXPECT_EQ(roundTripped->GetName(), font->GetName());
    EXPECT_EQ(roundTripped->GetHandle(), assetInfo.Handle) << "Handle from AssetInfo must be applied";
    EXPECT_TRUE(RangesEqual(roundTripped->GetRanges(), ranges));
    EXPECT_EQ(GlyphCount(roundTripped), sourceGlyphs) << "Glyph coverage must survive the pack round-trip";
}
