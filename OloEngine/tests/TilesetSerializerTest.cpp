// OLO_TEST_LAYER: unit
// =============================================================================
// TilesetSerializerTest.cpp
//
// YAML round-trip for the `.olotileset` asset (issue #646).
//
// TilesetSerializer's disk paths need a live Project to resolve
// Project::GetProjectDirectory(), so this test exercises the string-level
// SerializeToYAML / DeserializeFromYAML pair those paths are built on — the
// same split InstancePlacementSerializer uses. What it pins:
//
//   - every authored field survives;
//   - the tile-metadata sequence is SPARSE: an atlas with 4096 tiles and two
//     solid ones writes two records, not 4096. That is the difference between a
//     readable asset file and a megabyte of near-empty maps;
//   - malformed input is refused rather than half-loaded;
//   - the loader does NOT resolve the atlas texture (see the dedicated case) —
//     doing so deadlocks the editor, which cost a live-editor debugging session
//     to find because every unit test passed.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Tilemap/Tileset.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        Ref<Tileset> RoundTrip(const Ref<Tileset>& source, std::string* outYaml = nullptr)
        {
            const TilesetSerializer serializer;
            const std::string yaml = serializer.SerializeToYAML(source);
            if (outYaml)
                *outYaml = yaml;

            auto loaded = Ref<Tileset>::Create();
            if (!serializer.DeserializeFromYAML(yaml, loaded))
                return nullptr;
            return loaded;
        }
    } // namespace

    TEST(TilesetSerializer, AuthoredFieldsSurviveAYamlRoundTrip)
    {
        auto source = Ref<Tileset>::Create();
        source->SetTextureHandle(AssetHandle(0x00ABCDEF12345678ull));
        source->SetTileSize(24, 32);
        source->SetSpacing(2);
        source->SetMargin(3);

        TileInfo solid;
        solid.Solid = true;
        solid.Flags = 0b1011;
        solid.Type = "ladder";
        source->SetTileInfo(7, solid);

        auto loaded = RoundTrip(source);
        ASSERT_TRUE(loaded);

        EXPECT_EQ(static_cast<u64>(loaded->GetTextureHandle()), 0x00ABCDEF12345678ull);
        EXPECT_EQ(loaded->GetTileWidth(), 24u);
        EXPECT_EQ(loaded->GetTileHeight(), 32u);
        EXPECT_EQ(loaded->GetSpacing(), 2u);
        EXPECT_EQ(loaded->GetMargin(), 3u);

        const TileInfo restored = loaded->GetTileInfo(7);
        EXPECT_TRUE(restored.Solid);
        EXPECT_EQ(restored.Flags, 0b1011u);
        EXPECT_EQ(restored.Type, "ladder");
        // The gap below index 7 comes back as defaults, not as garbage.
        EXPECT_FALSE(loaded->GetTileInfo(6).Solid);
    }

    TEST(TilesetSerializer, TheTextureSizeIsNotSerializedAndTheLoaderDoesNotResolveIt)
    {
        // Two contracts in one:
        //
        // 1. The atlas pixel size belongs to the texture, not the tileset:
        //    re-exporting the atlas at a different size must re-slice rather than
        //    keep a stale count, so the size is never written to the file.
        // 2. The loader must leave it at zero rather than resolving the texture.
        //    Resolving would mean calling AssetManager::GetAsset from inside
        //    TilesetSerializer::TryLoadData, which re-enters
        //    AssetImporter::TryLoadData and deadlocks the editor on the first frame
        //    that draws a tilemap. The render path slices against the live texture
        //    (GetTileUVForAtlas) and never reads this field.
        auto source = Ref<Tileset>::Create();
        source->SetTextureSize(256, 128);

        auto loaded = RoundTrip(source);
        ASSERT_TRUE(loaded);
        EXPECT_EQ(loaded->GetTextureWidth(), 0u);
        EXPECT_EQ(loaded->GetTextureHeight(), 0u);
    }

    TEST(TilesetSerializer, DefaultTileRecordsAreOmittedFromTheFile)
    {
        auto source = Ref<Tileset>::Create();
        source->SetTileSize(16, 16);
        TileInfo solid;
        solid.Solid = true;
        source->SetTileInfo(4095, solid); // grows the dense vector to 4096 entries
        ASSERT_EQ(source->GetTiles().size(), 4096u);

        std::string yaml;
        auto loaded = RoundTrip(source, &yaml);
        ASSERT_TRUE(loaded);

        // Exactly one record written, so the file stays a few hundred bytes.
        EXPECT_LT(std::count(yaml.begin(), yaml.end(), '\n'), 40)
            << "Sparse emission regressed - the file grew one map per tile:\n"
            << yaml.substr(0, 400);
        EXPECT_TRUE(loaded->IsTileSolid(4095));
        EXPECT_FALSE(loaded->IsTileSolid(4094));
    }

    TEST(TilesetSerializer, RejectsInputWithNoTilesetSection)
    {
        const TilesetSerializer serializer;
        auto loaded = Ref<Tileset>::Create();
        EXPECT_FALSE(serializer.DeserializeFromYAML("SomethingElse:\n  Foo: 1\n", loaded));
    }

    TEST(TilesetSerializer, RejectsAZeroTileSizeRatherThanLoadingAnUnusableAtlas)
    {
        // A zero tile dimension makes the slicer report an empty atlas, so every
        // GetTileUV call would fail for a tileset that otherwise looks loaded.
        const TilesetSerializer serializer;
        auto loaded = Ref<Tileset>::Create();
        EXPECT_FALSE(serializer.DeserializeFromYAML(
            "Tileset:\n  TileWidth: 0\n  TileHeight: 16\n", loaded));
    }

    TEST(TilesetSerializer, SkipsATileRecordWithNoIndex)
    {
        const TilesetSerializer serializer;
        auto loaded = Ref<Tileset>::Create();
        ASSERT_TRUE(serializer.DeserializeFromYAML(
            "Tileset:\n"
            "  TileWidth: 16\n"
            "  TileHeight: 16\n"
            "  Tiles:\n"
            "    - Solid: true\n" // no Index - unaddressable, dropped
            "    - Index: 2\n"
            "      Solid: true\n",
            loaded));
        EXPECT_TRUE(loaded->IsTileSolid(2));
        EXPECT_FALSE(loaded->IsTileSolid(0));
    }

    TEST(TilesetSerializer, DropsATileIndexFarBeyondAnyRealAtlas)
    {
        // The metadata vector is dense up to the highest index, so an unbounded
        // index in a corrupt file is an allocation bomb rather than a bad tile.
        const TilesetSerializer serializer;
        auto loaded = Ref<Tileset>::Create();
        ASSERT_TRUE(serializer.DeserializeFromYAML(
            "Tileset:\n"
            "  TileWidth: 16\n"
            "  TileHeight: 16\n"
            "  Tiles:\n"
            "    - Index: 4000000000\n"
            "      Solid: true\n",
            loaded));
        EXPECT_TRUE(loaded->GetTiles().empty());
    }
} // namespace OloEngine::Tests
