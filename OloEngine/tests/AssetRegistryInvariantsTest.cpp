// =============================================================================
// AssetRegistryInvariantsTest.cpp
//
// Catches subtle bugs in `AssetRegistry`'s Add/Remove/Update path that
// surface as "the editor's asset panel shows the file but `GetAsset(path)`
// returns null" — the registry's two indices (handle→metadata and
// path→metadata) drift out of sync because a code path updated one but
// not the other.
//
// What this exercises
// -------------------
//   For each public mutation method (`AddAsset`, `RemoveAsset`,
//   `UpdateMetadata`, `Clear`, `GenerateHandle`), build a scenario and
//   assert all derived query methods (`Exists(handle)`,
//   `Exists(path)`, `GetMetadata(handle)`, `GetMetadata(path)`,
//   `GetHandleFromPath`, `GetAssetsOfType`, `GetAssetCount`) return
//   results consistent with the mutation.
//
// Why this isn't covered already
// ------------------------------
//   `AssetCreationTest.cpp` exercises constructing individual asset
//   types (MeshColliderAsset, ScriptFileAsset, …) but doesn't go
//   through the registry's bookkeeping. The
//   `SandboxAssetRegistryDeserialisesAndPathsResolve` test reads the
//   on-disk .oar but doesn't mutate it. This file covers the in-memory
//   mutation path.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetMetadata.h"

#include <filesystem>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        AssetMetadata MakeMetadata(AssetHandle handle, AssetType type, const char* path)
        {
            return AssetMetadata{ handle, type, std::filesystem::path{ path } };
        }
    } // namespace

    // -------------------------------------------------------------------------
    // AddAsset → both indices updated, all query methods agree.
    // -------------------------------------------------------------------------
    TEST(AssetRegistryInvariants, AddAssetMakesBothIndicesAgree)
    {
        AssetRegistry registry;
        EXPECT_EQ(registry.GetAssetCount(), 0u);

        const AssetHandle h{ 12345ULL };
        const std::filesystem::path p{ "Assets/Textures/Foo.png" };
        registry.AddAsset(MakeMetadata(h, AssetType::Texture2D, "Assets/Textures/Foo.png"));

        EXPECT_EQ(registry.GetAssetCount(), 1u);
        EXPECT_TRUE(registry.Exists(h));
        EXPECT_TRUE(registry.Exists(p));
        EXPECT_EQ(static_cast<u64>(registry.GetHandleFromPath(p)), static_cast<u64>(h));
        EXPECT_EQ(registry.GetMetadata(h).FilePath, p);
        EXPECT_EQ(static_cast<u64>(registry.GetMetadata(p).Handle), static_cast<u64>(h));
    }

    // -------------------------------------------------------------------------
    // RemoveAsset → both indices clean up.
    // -------------------------------------------------------------------------
    TEST(AssetRegistryInvariants, RemoveAssetClearsBothIndices)
    {
        AssetRegistry registry;
        const AssetHandle h{ 67890ULL };
        const std::filesystem::path p{ "Assets/Meshes/Bar.olomesh" };
        registry.AddAsset(MakeMetadata(h, AssetType::Mesh, "Assets/Meshes/Bar.olomesh"));
        ASSERT_TRUE(registry.Exists(h));

        EXPECT_TRUE(registry.RemoveAsset(h));
        EXPECT_FALSE(registry.Exists(h));
        EXPECT_FALSE(registry.Exists(p));
        EXPECT_EQ(static_cast<u64>(registry.GetHandleFromPath(p)), 0u);
        EXPECT_EQ(registry.GetAssetCount(), 0u);

        // Removing again is a no-op (the bool return tells you it
        // wasn't there). Notably: it doesn't crash.
        EXPECT_FALSE(registry.RemoveAsset(h));
    }

    // -------------------------------------------------------------------------
    // UpdateMetadata → handle preserved, but new path/type take effect
    // in both indices.
    // -------------------------------------------------------------------------
    TEST(AssetRegistryInvariants, UpdateMetadataKeepsHandleSwapsPath)
    {
        AssetRegistry registry;
        const AssetHandle h{ 1111ULL };
        const std::filesystem::path oldPath{ "Assets/Old/File.olomesh" };
        const std::filesystem::path newPath{ "Assets/New/File.olomesh" };
        registry.AddAsset(MakeMetadata(h, AssetType::Mesh, "Assets/Old/File.olomesh"));

        AssetMetadata updated{ h, AssetType::Mesh, newPath };
        registry.UpdateMetadata(h, updated);

        EXPECT_EQ(registry.GetAssetCount(), 1u);
        EXPECT_TRUE(registry.Exists(h));
        EXPECT_TRUE(registry.Exists(newPath));
        EXPECT_FALSE(registry.Exists(oldPath))
            << "Old path is still indexed after UpdateMetadata — the path index "
               "would resolve a stale GUID for any caller still using oldPath.";
        EXPECT_EQ(registry.GetMetadata(h).FilePath, newPath);
        EXPECT_EQ(static_cast<u64>(registry.GetHandleFromPath(newPath)), static_cast<u64>(h));
        EXPECT_EQ(static_cast<u64>(registry.GetHandleFromPath(oldPath)), 0u);
    }

    // -------------------------------------------------------------------------
    // Clear → empty registry, no stale entries.
    // -------------------------------------------------------------------------
    TEST(AssetRegistryInvariants, ClearEmptiesEverything)
    {
        AssetRegistry registry;
        registry.AddAsset(MakeMetadata(AssetHandle{ 1ULL }, AssetType::Texture2D, "a.png"));
        registry.AddAsset(MakeMetadata(AssetHandle{ 2ULL }, AssetType::Mesh, "b.olomesh"));
        registry.AddAsset(MakeMetadata(AssetHandle{ 3ULL }, AssetType::Scene, "c.olo"));
        ASSERT_EQ(registry.GetAssetCount(), 3u);

        registry.Clear();
        EXPECT_EQ(registry.GetAssetCount(), 0u);
        EXPECT_FALSE(registry.Exists(AssetHandle{ 1ULL }));
        EXPECT_FALSE(registry.Exists(AssetHandle{ 2ULL }));
        EXPECT_FALSE(registry.Exists(AssetHandle{ 3ULL }));
        EXPECT_FALSE(registry.Exists(std::filesystem::path{ "a.png" }));
    }

    // -------------------------------------------------------------------------
    // GenerateHandle → always non-zero (zero is the "uninitialised"
    // sentinel) and unique across consecutive calls.
    // -------------------------------------------------------------------------
    TEST(AssetRegistryInvariants, GenerateHandleProducesUniqueNonZeroHandles)
    {
        AssetRegistry registry;
        constexpr u32 kCount = 32;
        std::vector<AssetHandle> generated;
        generated.reserve(kCount);
        for (u32 i = 0; i < kCount; ++i)
        {
            AssetHandle h = registry.GenerateHandle();
            EXPECT_NE(static_cast<u64>(h), 0u)
                << "GenerateHandle returned 0 — reserved as the 'uninitialised' sentinel.";
            generated.push_back(h);
        }
        // Pairwise uniqueness.
        for (sizet i = 0; i < generated.size(); ++i)
            for (sizet j = i + 1; j < generated.size(); ++j)
                EXPECT_NE(static_cast<u64>(generated[i]), static_cast<u64>(generated[j]))
                    << "GenerateHandle returned a duplicate at indices "
                    << i << " and " << j;
    }

    // -------------------------------------------------------------------------
    // GetAssetsOfType — type filter works AFTER mutations.
    // -------------------------------------------------------------------------
    TEST(AssetRegistryInvariants, GetAssetsOfTypeFiltersAcrossMutations)
    {
        AssetRegistry registry;
        registry.AddAsset(MakeMetadata(AssetHandle{ 10ULL }, AssetType::Texture2D, "t1.png"));
        registry.AddAsset(MakeMetadata(AssetHandle{ 20ULL }, AssetType::Texture2D, "t2.png"));
        registry.AddAsset(MakeMetadata(AssetHandle{ 30ULL }, AssetType::Mesh, "m1.olomesh"));

        auto textures = registry.GetAssetsOfType(AssetType::Texture2D);
        EXPECT_EQ(textures.size(), 2u);
        auto meshes = registry.GetAssetsOfType(AssetType::Mesh);
        EXPECT_EQ(meshes.size(), 1u);

        // After removing one texture, the type-filtered count must
        // reflect the change immediately.
        registry.RemoveAsset(AssetHandle{ 10ULL });
        textures = registry.GetAssetsOfType(AssetType::Texture2D);
        EXPECT_EQ(textures.size(), 1u);
        EXPECT_EQ(static_cast<u64>(textures.front().Handle), 20ULL);
    }
} // namespace OloEngine::Tests
