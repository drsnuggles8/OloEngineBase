// =============================================================================
// TextureInPlaceReloadTest.cpp
//
// Regression coverage for issue #544 — texture hot-reload must refresh a
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
// Issue #1067 added the third case below. The first case passes with the
// asset-system hot-reload path completely broken, because it builds its texture
// with an ABSOLUTE path via Texture2D::Create — the one spelling of m_Path that
// stbi_load could always resolve. Every texture that arrives through
// TextureSerializer carries the PROJECT-RELATIVE spelling instead (deliberately,
// so saved scenes stay portable), and the editor runs with cwd = OloEditor/, one
// level above the project — so Reload()'s re-read could never find the file and
// hot-reload was dead for every real asset. That is the substituted seam
// described in docs/agent-rules/substituted-seams-compound.md: the friendlier
// construction is what made the case green. The third test drives the real
// seam — EditorAssetManager::ReloadData over a texture imported from a project.
//
// GL-gated: SKIPs cleanly when no GL 4.6 context is available (headless CI).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Texture.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <stb_image/stb_image_write.h>

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
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
            return OloEngine::Tests::TempFile("olo_texture_inplace_reload_544.png");
        }

        // Drops the process-wide Project + EditorAssetManager statics on EVERY exit
        // path. A bare call on the last line is not enough: any earlier ASSERT_*
        // returns first — including the ReloadData assert, the one that fires on a
        // real regression — and the manager would then outlive TempRoot's atexit
        // remove_all and serialize its registry into a deleted directory.
        struct ProjectScope
        {
            ~ProjectScope()
            {
                Project::Unload();
            }
        };
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
    // -------------------------------------------------------------------------
    // Issue #1067: the same in-place reload, reached the way the editor reaches
    // it — through EditorAssetManager::ReloadData for a texture that was imported
    // from a project and therefore carries a project-relative source path.
    //
    // The temp project directory is deliberately NOT the process working
    // directory, which is what makes the relative path unresolvable against the
    // CWD; before the fix, Reload() logged "failed to re-read texture" and
    // ReloadData silently fell back to replacing the object, so a Material's
    // cached Ref<Texture2D> kept showing the pre-edit pixels forever.
    //
    // ProjectScope drops both statics on every exit path. Without that the asset
    // manager — and the texture it caches — outlive the case: the renderer's
    // teardown reports a surviving GPU allocation, and the registry serializes at
    // static-destruction time, after TempRoot has already been removed ("Failed to
    // open file: ...AssetRegistry.oar"). Unloading leaves the clean no-project
    // state every other asset-backed test sets up for itself.
    // -------------------------------------------------------------------------
    TEST(TextureInPlaceReload, ReloadsATextureImportedThroughTheAssetSystem)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ProjectScope projectScope;
        const std::filesystem::path projectDir = OloEngine::Tests::TempDir("project_1067");
        const std::filesystem::path relativePath = std::filesystem::path("Assets") / "Textures" / "HotReload1067.png";
        const std::filesystem::path absolutePath = projectDir / relativePath;

        std::error_code ec;
        std::filesystem::create_directories(absolutePath.parent_path(), ec);
        ASSERT_FALSE(ec) << "could not create the temp project asset directory: " << ec.message();

        // "HotReload1067" carries none of the colour-texture name suffixes
        // (diffuse/albedo/basecolor/emissive), so TextureSerializer's naming
        // heuristic loads it linear and the readback below is byte-exact.
        ASSERT_TRUE(WriteSolidPng(absolutePath, 2, 2, 255, 0, 0, 255)) << "failed to write the initial PNG";

        {
            std::ofstream proj(projectDir / "TextureReload1067.oloproj");
            ASSERT_TRUE(proj.is_open()) << "could not write the temp .oloproj";
            proj << "Project:\n"
                    "  Name: TextureReload1067\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        ASSERT_TRUE(Project::Load(projectDir / "TextureReload1067.oloproj"))
            << "Project::Load failed for the temp project at " << projectDir.string();
        ASSERT_NE(Project::GetProjectDirectory(), std::filesystem::current_path())
            << "the temp project is the process working directory, so a CWD-relative "
               "re-read would resolve by accident and this test would prove nothing.";

        auto manager = Ref<EditorAssetManager>::Create();
        // No file watcher: this test triggers the reload itself, and a background
        // watcher outlives the case and races the next one.
        manager->Initialize(false);
        Project::SetAssetManager(manager);

        const AssetHandle handle = manager->ImportAsset(absolutePath);
        ASSERT_NE(static_cast<u64>(handle), 0ULL) << "ImportAsset returned a zero handle for the probe texture";

        auto texture = manager->GetAsset(handle).As<Texture2D>();
        ASSERT_TRUE(texture) << "GetAsset returned null or a non-Texture2D for the probe";
        ASSERT_TRUE(texture->IsLoaded());
        ASSERT_EQ(2u, texture->GetWidth());
        ASSERT_EQ(2u, texture->GetHeight());

        // The asymmetry that IS the bug: this path stores the relative spelling,
        // Texture2D::Create(absolute) stores an absolute one. If this ever becomes
        // absolute the case still passes but stops covering #1067 — hence the
        // assertion rather than a comment.
        ASSERT_TRUE(std::filesystem::path(texture->GetPath()).is_relative())
            << "the asset system no longer stores a project-relative source path ('"
            << texture->GetPath()
            << "'), so this case no longer exercises the #1067 resolution failure.";

        Texture2D* const objectBefore = texture.get();
        {
            std::vector<u8> before;
            ReadbackRgba8(texture->GetRendererID(), 2, 2, before);
            ASSERT_GE(before.size(), 4u);
            ASSERT_EQ(255u, before[0]) << "the probe did not load as solid red";
            ASSERT_EQ(0u, before[1]);
        }

        // What the editor sees when someone saves the .png: new contents, and a
        // different size so a stale upload cannot masquerade as a fresh one.
        ASSERT_TRUE(WriteSolidPng(absolutePath, 4, 4, 0, 255, 0, 255)) << "failed to overwrite the PNG";

        ASSERT_TRUE(manager->ReloadData(handle)) << "EditorAssetManager::ReloadData failed for a tracked, loaded texture";

        auto refreshed = manager->GetAsset(handle).As<Texture2D>();
        ASSERT_TRUE(refreshed) << "the texture is missing from the cache after the reload";

        // In place: the same object, so every Ref<Texture2D> a material captured
        // still points at the refreshed texture (issue #544's guarantee, now over
        // the asset-system path).
        EXPECT_EQ(objectBefore, refreshed.get())
            << "ReloadData replaced the object instead of refreshing it in place — the "
               "in-place branch declined, which is what a failed re-read looks like from here.";
        EXPECT_TRUE(refreshed->IsLoaded());
        EXPECT_EQ(4u, refreshed->GetWidth());
        EXPECT_EQ(4u, refreshed->GetHeight());

        // And the pixels actually moved.
        {
            std::vector<u8> after;
            ReadbackRgba8(refreshed->GetRendererID(), 4, 4, after);
            ASSERT_GE(after.size(), 4u);
            EXPECT_EQ(0u, after[0]) << "the texture still carries the pre-edit red pixels";
            EXPECT_EQ(255u, after[1]);
            EXPECT_EQ(0u, after[2]);
        }

        // Release the texture so the registry serializes while the temp directory
        // still exists (see the note above); ~ProjectScope drops the statics.
        texture.Reset();
        refreshed.Reset();
    }
} // namespace OloEngine::Tests
