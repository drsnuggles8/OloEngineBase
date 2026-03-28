#include "OloEnginePCH.h"
#include "DirectoryTree.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace OloEngine; // NOLINT

namespace
{
    // RAII helper that creates a temp directory tree and removes it on destruction.
    struct TempDirectoryFixture
    {
        std::filesystem::path Root;

        TempDirectoryFixture()
        {
            auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
#ifdef _WIN32
            auto pid = static_cast<unsigned>(_getpid());
#else
            auto pid = static_cast<unsigned>(getpid());
#endif
            Root = std::filesystem::temp_directory_path() / ("OloEngine_DirTreeTest_" + std::to_string(tid) + "_" + std::to_string(pid));
            std::filesystem::remove_all(Root);

            // Create a small directory tree:
            // Root/
            //   textures/
            //     sky.png
            //     ground.png
            //   models/
            //     hero/
            //       hero.obj
            //     tree.fbx
            //   readme.txt
            std::filesystem::create_directories(Root / "textures");
            std::filesystem::create_directories(Root / "models" / "hero");

            auto touch = [](const std::filesystem::path& p)
            { std::ofstream f(p); };
            touch(Root / "textures" / "sky.png");
            touch(Root / "textures" / "ground.png");
            touch(Root / "models" / "hero" / "hero.obj");
            touch(Root / "models" / "tree.fbx");
            touch(Root / "readme.txt");
        }

        ~TempDirectoryFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(Root, ec);
        }

        TempDirectoryFixture(const TempDirectoryFixture&) = delete;
        TempDirectoryFixture& operator=(const TempDirectoryFixture&) = delete;
    };
} // namespace

// =========================================================================
// Build
// =========================================================================

TEST(DirectoryTree, BuildCreatesRoot)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    ASSERT_NE(tree.GetRoot(), nullptr);
    EXPECT_EQ(tree.GetAssetRoot(), fixture.Root);
    EXPECT_TRUE(tree.GetRoot()->RelativePath.empty());
}

TEST(DirectoryTree, BuildScansSubdirectories)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* root = tree.GetRoot();
    ASSERT_NE(root, nullptr);

    // Root should have 2 subdirectories (models, textures) — sorted alphabetically
    EXPECT_EQ(root->SubDirectories.size(), 2u);

    // Check sorted order
    EXPECT_EQ(root->SubDirectories[0]->Name, "models");
    EXPECT_EQ(root->SubDirectories[1]->Name, "textures");
}

TEST(DirectoryTree, BuildScansFiles)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* root = tree.GetRoot();
    ASSERT_NE(root, nullptr);

    // Root has 1 file: readme.txt
    EXPECT_EQ(root->Files.size(), 1u);
    EXPECT_EQ(root->Files[0].filename().string(), "readme.txt");
}

TEST(DirectoryTree, BuildScansRecursively)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    // models/hero/ should have hero.obj
    auto* models = tree.FindDirectory("models");
    ASSERT_NE(models, nullptr);
    EXPECT_EQ(models->SubDirectories.size(), 1u);
    EXPECT_EQ(models->Files.size(), 1u);
    EXPECT_EQ(models->Files[0].filename().string(), "tree.fbx");

    auto* hero = tree.FindDirectory("models/hero");
    ASSERT_NE(hero, nullptr);
    EXPECT_EQ(hero->Files.size(), 1u);
    EXPECT_EQ(hero->Files[0].filename().string(), "hero.obj");
}

TEST(DirectoryTree, ParentPointersAreSet)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* root = tree.GetRoot();
    EXPECT_EQ(root->Parent, nullptr);

    auto* models = tree.FindDirectory("models");
    ASSERT_NE(models, nullptr);
    EXPECT_EQ(models->Parent, root);

    auto* hero = tree.FindDirectory("models/hero");
    ASSERT_NE(hero, nullptr);
    EXPECT_EQ(hero->Parent, models);
}

// =========================================================================
// FindDirectory
// =========================================================================

TEST(DirectoryTree, FindDirectoryRoot)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    EXPECT_EQ(tree.FindDirectory(""), tree.GetRoot());
    EXPECT_EQ(tree.FindDirectory("."), tree.GetRoot());
}

TEST(DirectoryTree, FindDirectoryNested)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* hero = tree.FindDirectory("models/hero");
    ASSERT_NE(hero, nullptr);
    EXPECT_EQ(hero->Name, "hero");
}

TEST(DirectoryTree, FindDirectoryReturnsNullForMissing)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    EXPECT_EQ(tree.FindDirectory("nonexistent"), nullptr);
    EXPECT_EQ(tree.FindDirectory("models/nonexistent"), nullptr);
}

// =========================================================================
// Search
// =========================================================================

TEST(DirectoryTree, SearchEmptyQueryReturnsNothing)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto results = tree.Search("");
    EXPECT_TRUE(results.empty());
}

TEST(DirectoryTree, SearchFindsFilesByName)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto results = tree.Search("hero");
    // Should find: models/hero/ (directory) and models/hero/hero.obj (file)
    EXPECT_GE(results.size(), 2u);

    bool foundDir = false;
    bool foundFile = false;
    for (auto& r : results)
    {
        if (r.filename().string() == "hero" && std::filesystem::is_directory(r))
            foundDir = true;
        if (r.filename().string() == "hero.obj")
            foundFile = true;
    }
    EXPECT_TRUE(foundDir);
    EXPECT_TRUE(foundFile);
}

TEST(DirectoryTree, SearchIsCaseInsensitive)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto results = tree.Search("SKY");
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].filename().string(), "sky.png");
}

TEST(DirectoryTree, SearchFindsPartialMatch)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto results = tree.Search(".png");
    // Should find sky.png and ground.png
    EXPECT_EQ(results.size(), 2u);
}

// =========================================================================
// MarkDirty
// =========================================================================

TEST(DirectoryTree, MarkDirtyFlagsNode)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* models = tree.FindDirectory("models");
    ASSERT_NE(models, nullptr);
    EXPECT_FALSE(models->NeedsRefresh);

    tree.MarkDirty("models");
    EXPECT_TRUE(models->NeedsRefresh);
}

TEST(DirectoryTree, MarkDirtyFlagsAncestors)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    tree.MarkDirty("models/hero");

    auto* hero = tree.FindDirectory("models/hero");
    auto* models = tree.FindDirectory("models");
    auto* root = tree.GetRoot();

    EXPECT_TRUE(hero->NeedsRefresh);
    EXPECT_TRUE(models->NeedsRefresh);
    EXPECT_TRUE(root->NeedsRefresh);
}

// =========================================================================
// RefreshSubtree
// =========================================================================

TEST(DirectoryTree, RefreshPicksUpNewFiles)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* textures = tree.FindDirectory("textures");
    ASSERT_NE(textures, nullptr);
    EXPECT_EQ(textures->Files.size(), 2u);

    // Add a new file on disk
    std::ofstream(fixture.Root / "textures" / "water.png");

    // Not yet visible in tree
    EXPECT_EQ(textures->Files.size(), 2u);

    // Refresh
    tree.RefreshSubtree(textures);
    EXPECT_EQ(textures->Files.size(), 3u);
}

TEST(DirectoryTree, RefreshClearsNeedsRefreshFlag)
{
    TempDirectoryFixture fixture;
    DirectoryTree tree;
    tree.Build(fixture.Root);

    auto* models = tree.FindDirectory("models");
    models->NeedsRefresh = true;

    tree.RefreshSubtree(models);
    EXPECT_FALSE(models->NeedsRefresh);
}
