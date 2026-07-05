// =============================================================================
// TextureInPlaceReloadTest.cpp
//
// Regression coverage for issue #544 Part B — texture hot-reload must refresh a
// Texture2D *in place* (same object identity behind the existing Ref<Texture2D>)
// instead of handing back a brand-new object. Materials store their textures as
// Ref<Texture2D> members with no per-frame re-resolution from a handle, so an
// object-replacing reload leaves the material pointing at the pre-edit texture
// until the whole scene is reloaded. Texture2D::Reload() (backing
// EditorAssetManager::ReloadData's in-place path) fixes that by recreating the
// GL storage on the SAME object.
//
// What this pins:
//   1. Reload() returns true and preserves object identity (tex.get() unchanged).
//   2. The refreshed object reports the new file's dimensions and IsLoaded().
//   3. Reading the texture back yields the NEW pixels, not the pre-edit ones —
//      proving the GL contents were actually replaced, not just the metadata.
//
// (The underlying GL texture *name* may or may not change across the reload — the
// old immutable storage is recreated, but GL is free to recycle the freed name.
// That's why consumers must read the RendererID off the object each frame rather
// than caching it; the reload's inspector/binding-cache teardown accounts for the
// swap either way. The test deliberately does not assert on the raw name value.)
//
// GL-gated: SKIPs cleanly when no GL 4.6 context is available (headless CI).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Texture.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <stb_image/stb_image_write.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>
#include <vector>

// OLO_TEST_LAYER: L3

namespace OloEngine::Tests
{
    namespace
    {
        // Write a solid-color RGBA8 PNG. Solid fills sidestep any vertical-flip /
        // orientation concern — the readback assertion only needs the color to differ
        // before vs after the reload.
        bool WriteSolidPng(const std::filesystem::path& path, int w, int h, u8 r, u8 g, u8 b, u8 a)
        {
            std::vector<u8> pixels(static_cast<sizet>(w) * static_cast<sizet>(h) * 4u);
            for (sizet i = 0; i < pixels.size(); i += 4)
            {
                pixels[i + 0] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
                pixels[i + 3] = a;
            }
            return ::stbi_write_png(path.string().c_str(), w, h, 4, pixels.data(), w * 4) != 0;
        }

        // Unique per-process temp path so parallel test runs don't collide.
        std::filesystem::path MakeTempTexturePath()
        {
            return std::filesystem::temp_directory_path() / "olo_texture_inplace_reload_544.png";
        }
    } // namespace

    TEST(TextureInPlaceReload, PreservesIdentityAndRefreshesContents)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = MakeTempTexturePath();

        // Initial content: solid red, 2x2. Load as a data texture (srgb=false) so the
        // stored texels equal the bytes we wrote — the readback below is byte-exact.
        ASSERT_TRUE(WriteSolidPng(path, 2, 2, 255, 0, 0, 255)) << "failed to write initial PNG";

        Ref<Texture2D> texture = Texture2D::Create(path.string(), /*srgb=*/false);
        ASSERT_TRUE(texture);
        ASSERT_TRUE(texture->IsLoaded());
        ASSERT_EQ(2u, texture->GetWidth());
        ASSERT_EQ(2u, texture->GetHeight());

        Texture2D* const objectBefore = texture.get();

        {
            std::vector<u8> before;
            ReadbackRgba8(texture->GetRendererID(), 2, 2, before);
            ASSERT_GE(before.size(), 4u);
            EXPECT_EQ(255u, before[0]); // red
            EXPECT_EQ(0u, before[1]);
            EXPECT_EQ(0u, before[2]);
        }

        // Edit on disk: solid green, and a different size to prove storage recreation.
        ASSERT_TRUE(WriteSolidPng(path, 4, 4, 0, 255, 0, 255)) << "failed to overwrite PNG";

        EXPECT_TRUE(texture->Reload());

        // (1) Same object behind the Ref — a material's cached Ref stays valid.
        EXPECT_EQ(objectBefore, texture.get());
        // (2) Metadata reflects the new file.
        EXPECT_TRUE(texture->IsLoaded());
        EXPECT_EQ(4u, texture->GetWidth());
        EXPECT_EQ(4u, texture->GetHeight());

        // (3) The GL contents are actually the new pixels, not the stale red.
        {
            std::vector<u8> after;
            ReadbackRgba8(texture->GetRendererID(), 4, 4, after);
            ASSERT_GE(after.size(), 4u);
            EXPECT_EQ(0u, after[0]); // green
            EXPECT_EQ(255u, after[1]);
            EXPECT_EQ(0u, after[2]);
        }

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // Cooked block-compressed textures have no live-edit path — Reload() must refuse
    // (return false) so the asset manager falls back to replacing the object rather
    // than mis-routing a .olotex through the uncompressed upload path.
    TEST(TextureInPlaceReload, RefusesWhenNoReReadablePath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // A spec-created texture has an empty source path, so there is nothing to
        // re-read: Reload() must decline in place.
        TextureSpecification spec;
        spec.Width = 4;
        spec.Height = 4;
        spec.Format = ImageFormat::RGBA8;
        Ref<Texture2D> texture = Texture2D::Create(spec);
        ASSERT_TRUE(texture);
        EXPECT_TRUE(texture->GetPath().empty());

        EXPECT_FALSE(texture->Reload());
    }
} // namespace OloEngine::Tests
