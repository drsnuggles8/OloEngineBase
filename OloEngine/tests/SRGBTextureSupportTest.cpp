#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// AssetSerializer.h forward-declares Scene; SceneSerializer (declared in the
// same header) uses Ref<Scene> implicitly via virtual destructors which forces
// the test TU to see the complete Scene type before instantiation. Including
// Scene.h here resolves that without pulling more headers into the test API.
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Renderer/Texture.h"

#include <string_view>

using namespace OloEngine;

// Pins the sRGB plumbing landed for the OpenGL backend so a future refactor
// can't silently drop colour-space conversion on albedo textures (which is
// the bug the PR was raised to fix: PBR shaders assume the GPU performs
// sRGB->linear on sample, but the backend was emitting GL_RGBA8).

TEST(SRGBTextureSupport, TextureSpecificationDefaultsToLinear)
{
    TextureSpecification spec;
    EXPECT_FALSE(spec.SRGB)
        << "Default-constructed TextureSpecification must be linear so existing "
           "data textures (normal / metallic-roughness / AO) keep their current "
           "behaviour. Only colour textures (albedo / emissive) opt in.";
}

TEST(SRGBTextureSupport, TextureSpecificationCarriesSrgbFlag)
{
    TextureSpecification spec;
    spec.SRGB = true;
    EXPECT_TRUE(spec.SRGB);
}

TEST(SRGBTextureSupport, RawTextureDataKeepsLinearDefault)
{
    RawTextureData raw;
    EXPECT_FALSE(raw.SRGB)
        << "RawTextureData must default to linear — TextureSerializer flips it "
           "to sRGB by filename heuristic, but unrelated callers (e.g. tests, "
           "procedural textures) should keep the safe linear default.";
}

// Filename heuristic for the asset-pipeline path-load route. Model.cpp passes
// sRGB explicitly per aiTextureType; this heuristic only fires for standalone
// .png drag-drops that hit TextureSerializer::TryLoadData / TryLoadRawData.

TEST(SRGBTextureSupport, FilenameHeuristic_ClassifiesAlbedoAsSrgb)
{
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("character_albedo.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("Brick_BaseColor.jpg"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("wood_diffuse.tga"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_A.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("door_color.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("torch_emissive.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("lantern_emission.png"));
}

TEST(SRGBTextureSupport, FilenameHeuristic_ClassifiesDataTexturesAsLinear)
{
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("character_normal.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Brick_Roughness.jpg"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Metal_Metallic.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("wood_AO.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("terrain_height.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_N.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_M.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_R.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("road_bump.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("rock_displacement.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("packed_orm.png"));
}

TEST(SRGBTextureSupport, FilenameHeuristic_DataKeywordsOverrideColorKeywords)
{
    // Data keywords scan first, so a filename that mentions both — e.g.
    // "color" + "roughness" — is correctly classified as the data half. The
    // heuristic requires a separator on shorter ambiguous suffixes (like AO)
    // to avoid false positives on words that happen to contain the letters.
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("wood_colorRoughness.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Diffuse_Metallic.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("base_color_normal.png"));
}

TEST(SRGBTextureSupport, FilenameHeuristic_AmbiguousFilesDefaultLinear)
{
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Texture001.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("untitled.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName(""));
}

TEST(SRGBTextureSupport, FilenameHeuristic_IsCaseInsensitive)
{
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("ALBEDO.PNG"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("Diffuse.TGA"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Normal.PNG"));
}
