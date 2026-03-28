#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "ContentBrowserItem.h"

#include <filesystem>
#include <vector>
#include <algorithm>

using namespace OloEngine;
namespace fs = std::filesystem;

// =========================================================================
// GetFileTypeFromExtension
// =========================================================================

TEST(FileTypeMapping, ImageExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("texture.png"), ContentFileType::Image);
    EXPECT_EQ(GetFileTypeFromExtension("photo.jpg"), ContentFileType::Image);
    EXPECT_EQ(GetFileTypeFromExtension("photo.jpeg"), ContentFileType::Image);
    EXPECT_EQ(GetFileTypeFromExtension("photo.tga"), ContentFileType::Image);
    EXPECT_EQ(GetFileTypeFromExtension("photo.bmp"), ContentFileType::Image);
    EXPECT_EQ(GetFileTypeFromExtension("env.hdr"), ContentFileType::Image);
}

TEST(FileTypeMapping, ModelExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("hero.obj"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("hero.fbx"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("hero.gltf"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("hero.glb"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("hero.dae"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("hero.3ds"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("hero.blend"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("cube.primitive"), ContentFileType::Model3D);
}

TEST(FileTypeMapping, SceneExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("level.olo"), ContentFileType::Scene);
    EXPECT_EQ(GetFileTypeFromExtension("level.scene"), ContentFileType::Scene);
}

TEST(FileTypeMapping, ScriptExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("player.cs"), ContentFileType::Script);
    EXPECT_EQ(GetFileTypeFromExtension("player.lua"), ContentFileType::Script);
}

TEST(FileTypeMapping, AudioExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("music.wav"), ContentFileType::Audio);
    EXPECT_EQ(GetFileTypeFromExtension("music.mp3"), ContentFileType::Audio);
    EXPECT_EQ(GetFileTypeFromExtension("music.ogg"), ContentFileType::Audio);
    EXPECT_EQ(GetFileTypeFromExtension("music.flac"), ContentFileType::Audio);
}

TEST(FileTypeMapping, MaterialExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("ground.mat"), ContentFileType::Material);
    EXPECT_EQ(GetFileTypeFromExtension("ground.material"), ContentFileType::Material);
}

TEST(FileTypeMapping, ShaderExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("post.glsl"), ContentFileType::Shader);
    EXPECT_EQ(GetFileTypeFromExtension("post.vert"), ContentFileType::Shader);
    EXPECT_EQ(GetFileTypeFromExtension("post.frag"), ContentFileType::Shader);
    EXPECT_EQ(GetFileTypeFromExtension("post.hlsl"), ContentFileType::Shader);
}

TEST(FileTypeMapping, EngineSpecificExtensions)
{
    EXPECT_EQ(GetFileTypeFromExtension("map.oloregion"), ContentFileType::StreamingRegion);
    EXPECT_EQ(GetFileTypeFromExtension("talk.olodialogue"), ContentFileType::Dialogue);
    EXPECT_EQ(GetFileTypeFromExtension("surface.olosg"), ContentFileType::ShaderGraph);
    EXPECT_EQ(GetFileTypeFromExtension("slot1.olosave"), ContentFileType::SaveGame);
}

TEST(FileTypeMapping, UnknownExtensionReturnsUnknown)
{
    EXPECT_EQ(GetFileTypeFromExtension("readme.txt"), ContentFileType::Unknown);
    EXPECT_EQ(GetFileTypeFromExtension("data.xml"), ContentFileType::Unknown);
    EXPECT_EQ(GetFileTypeFromExtension("noext"), ContentFileType::Unknown);
}

TEST(FileTypeMapping, CaseInsensitive)
{
    EXPECT_EQ(GetFileTypeFromExtension("TEXTURE.PNG"), ContentFileType::Image);
    EXPECT_EQ(GetFileTypeFromExtension("Hero.FBX"), ContentFileType::Model3D);
    EXPECT_EQ(GetFileTypeFromExtension("Level.OLO"), ContentFileType::Scene);
}

// =========================================================================
// GetDragDropPayloadType
// =========================================================================

TEST(DragDropPayload, ScenePayload)
{
    EXPECT_STREQ(GetDragDropPayloadType(ContentFileType::Scene), "CONTENT_BROWSER_SCENE");
}

TEST(DragDropPayload, ModelPayload)
{
    EXPECT_STREQ(GetDragDropPayloadType(ContentFileType::Model3D), "CONTENT_BROWSER_MODEL");
}

TEST(DragDropPayload, GenericFallback)
{
    EXPECT_STREQ(GetDragDropPayloadType(ContentFileType::Unknown), "CONTENT_BROWSER_ITEM");
    EXPECT_STREQ(GetDragDropPayloadType(ContentFileType::Image), "CONTENT_BROWSER_ITEM");
    EXPECT_STREQ(GetDragDropPayloadType(ContentFileType::Directory), "CONTENT_BROWSER_ITEM");
}

// =========================================================================
// SortContentBrowserItems  (no ImGui context needed — sorting is pure logic)
// =========================================================================

TEST(SortContentBrowserItems, DirectoriesBeforeFiles)
{
    std::vector<ContentBrowserItem> items;
    items.emplace_back(fs::path("assets/readme.txt"), ContentFileType::Unknown, nullptr);
    items.emplace_back(fs::path("assets/textures"), ContentFileType::Directory, nullptr);
    items.emplace_back(fs::path("assets/hero.fbx"), ContentFileType::Model3D, nullptr);

    SortContentBrowserItems(items);

    EXPECT_TRUE(items[0].IsDirectory());
    EXPECT_FALSE(items[1].IsDirectory());
    EXPECT_FALSE(items[2].IsDirectory());
}

TEST(SortContentBrowserItems, AlphabeticalWithinSameType)
{
    std::vector<ContentBrowserItem> items;
    items.emplace_back(fs::path("assets/Zebra.png"), ContentFileType::Image, nullptr);
    items.emplace_back(fs::path("assets/apple.png"), ContentFileType::Image, nullptr);
    items.emplace_back(fs::path("assets/Banana.png"), ContentFileType::Image, nullptr);

    SortContentBrowserItems(items);

    EXPECT_EQ(items[0].GetDisplayName(), "apple.png");
    EXPECT_EQ(items[1].GetDisplayName(), "Banana.png");
    EXPECT_EQ(items[2].GetDisplayName(), "Zebra.png");
}

TEST(SortContentBrowserItems, MixedDirsAndFilesSorted)
{
    std::vector<ContentBrowserItem> items;
    items.emplace_back(fs::path("assets/file_b.txt"), ContentFileType::Unknown, nullptr);
    items.emplace_back(fs::path("assets/dir_z"), ContentFileType::Directory, nullptr);
    items.emplace_back(fs::path("assets/file_a.txt"), ContentFileType::Unknown, nullptr);
    items.emplace_back(fs::path("assets/dir_a"), ContentFileType::Directory, nullptr);

    SortContentBrowserItems(items);

    EXPECT_EQ(items[0].GetDisplayName(), "dir_a");
    EXPECT_EQ(items[1].GetDisplayName(), "dir_z");
    EXPECT_EQ(items[2].GetDisplayName(), "file_a.txt");
    EXPECT_EQ(items[3].GetDisplayName(), "file_b.txt");
}

TEST(SortContentBrowserItems, EmptyVectorSafe)
{
    std::vector<ContentBrowserItem> items;
    SortContentBrowserItems(items);
    EXPECT_TRUE(items.empty());
}

// =========================================================================
// CBActionResult helpers
// =========================================================================

TEST(ActionResultHelpers, HasActionWorks)
{
    CBActionResult r = 0;
    r = r | ContentBrowserAction::Selected;
    r = r | ContentBrowserAction::Activated;

    EXPECT_TRUE(HasAction(r, ContentBrowserAction::Selected));
    EXPECT_TRUE(HasAction(r, ContentBrowserAction::Activated));
    EXPECT_FALSE(HasAction(r, ContentBrowserAction::Deleted));
}

TEST(ActionResultHelpers, SetActionWorks)
{
    CBActionResult r = 0;
    SetAction(r, ContentBrowserAction::Renamed);
    EXPECT_TRUE(HasAction(r, ContentBrowserAction::Renamed));
}
