// OLO_TEST_LAYER: L8
// =============================================================================
// TilemapVisualEvidenceTest.cpp
//
// Visual evidence + on-GPU contracts for the 2D tilemap renderer (issue #646).
//
// The CPU contracts (atlas slicing, UV math, the biased tile encoding, collider
// merging) live in TilemapTest.cpp. This file exists because none of those can
// tell you whether the right pixels reach the screen: a V-flip in the UV rect,
// a half-tile transform offset, or a chunk culler that rejects everything all
// pass every CPU test and render a wrong or empty frame.
//
// It drives the REAL Renderer2D path — the same TilemapRenderer::Draw the Scene
// calls — into an offscreen framebuffer and reads the pixels back:
//
//   Tilemap_Overview        : the whole 24x24 map in view.
//   Tilemap_Zoom            : a few tiles filling the frame, so tile edges and
//                             the atlas sub-rect are inspectable by eye.
//   Tilemap_LargeMapSameView: a 240x240 map through the SAME camera as the
//                             24x24 one — it must look identical, which is the
//                             eyeball half of the chunk-culling claim.
//   Tilemap_OffScreen       : the camera aimed away from the map. The frame must
//                             be empty AND the chunk culler must have rejected
//                             every chunk — a culler that "works" by drawing
//                             off-screen geometry produces the same PNG.
//
// Three assertions beyond the images:
//
//   1. Tiles actually drew (a non-clear pixel fraction floor), so an empty
//      frame cannot pass by matching a blank golden.
//   2. Tile identity: the atlas is built with one flat, distinct colour per
//      tile, so the pixel at a known tile's centre must carry that tile's
//      colour. This is what a flipped V axis or an off-by-one tile index
//      breaks, and it is checked through the full GPU path rather than
//      against GetTileUV's own arithmetic.
//   3. The chunking claim: viewed through the same camera, a 24x24 map and a
//      240x240 map submit the SAME number of tiles and the SAME number of
//      Renderer2D draw calls. That is the property the chunk culling exists
//      for — cost proportional to what is on screen, not to map size — and it
//      is the one a naive "iterate every cell" implementation fails.
//
// SKIPs cleanly (does not fail) without a GL 4.6 context, like the other
// RendererAttachedTest visual-evidence tests. Never DISABLED_.
//
// Classification: L8 / integration (real Renderer2D draw path + RGBA8 readback
// + PNG).
// =============================================================================
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/OrthographicCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Tilemap/TilemapComponent.h"
#include "OloEngine/Tilemap/TilemapRenderer.h"
#include "OloEngine/Tilemap/Tileset.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 512;

        // A 4x4 atlas of 16x16 tiles = 64x64 px, 16 tiles.
        constexpr u32 kAtlasTiles = 4;
        constexpr u32 kTilePixels = 16;
        constexpr u32 kAtlasPixels = kAtlasTiles * kTilePixels;

        constexpr u32 kMapSize = 24;
        constexpr f32 kWorldTileSize = 1.0f;

        // The framebuffer clear colour, and the same value quantised to 8-bit. One
        // definition so NonClearFraction cannot drift from what Capture clears to.
        constexpr glm::vec4 kClearColor{ 0.05f, 0.05f, 0.08f, 1.0f };
        constexpr int kClearColor8[3] = { 13, 13, 20 };

        // One flat, saturated, mutually distinguishable colour per atlas tile, so
        // a sampled pixel names the tile it came from. Index i is the i'th tile in
        // reading order (left-to-right, top-to-bottom) — the same order
        // Tileset::GetTileUV uses.
        [[nodiscard]] glm::u8vec3 TileColor(u32 index)
        {
            // Spread the 16 tiles over the RGB corners in a pattern with no two
            // entries within the sampling tolerance of each other.
            const u8 r = static_cast<u8>(((index & 0x1u) ? 200 : 40) + ((index & 0x8u) ? 40 : 0));
            const u8 g = static_cast<u8>(((index & 0x2u) ? 200 : 40) + ((index & 0x8u) ? 40 : 0));
            const u8 b = static_cast<u8>(((index & 0x4u) ? 200 : 40) + ((index & 0x8u) ? 40 : 0));
            return { r, g, b };
        }

        // Builds the atlas as RGBA8 with tile 0 at the image's TOP-LEFT, which is
        // the convention Tileset::GetTileUV assumes.
        //
        // The row order below is NOT the naive one, and that matters. The engine
        // loads real image files with stbi_set_flip_vertically_on_load_thread(1)
        // (AssetSerializer.cpp), so the picture's BOTTOM row arrives first in
        // memory and GL places it at v = 0 — the picture's top ends up at v = 1.
        // A texture built by hand and uploaded through SetData gets no such flip:
        // whatever row is written first lands at v = 0. So to stand in for a
        // loaded atlas, this fixture has to emit its rows bottom-picture-row
        // first. Writing them top-first instead mirrors the atlas vertically and
        // every tile samples its vertical neighbour — which is exactly what
        // EachCellShowsItsOwnAtlasTile caught when this was first written.
        [[nodiscard]] Ref<Texture2D> MakeAtlasTexture()
        {
            TextureSpecification spec;
            spec.Width = kAtlasPixels;
            spec.Height = kAtlasPixels;
            spec.Format = ImageFormat::RGBA8;
            // Flat authored colours compared byte-for-byte after readback: an sRGB
            // internal format would convert on sample and the comparison would be
            // against a different number for no benefit here.
            spec.SRGB = false;
            spec.GenerateMips = false;

            std::vector<u8> pixels(static_cast<sizet>(kAtlasPixels) * kAtlasPixels * 4u, 0u);
            for (u32 py = 0; py < kAtlasPixels; ++py)
            {
                for (u32 px = 0; px < kAtlasPixels; ++px)
                {
                    // py counts up from the first row in memory (v = 0), which is
                    // the picture's BOTTOM; the tile index counts down from the
                    // picture's top, hence the mirror.
                    const u32 imageRow = (kAtlasPixels - 1u - py) / kTilePixels;
                    const u32 tileIndex = imageRow * kAtlasTiles + (px / kTilePixels);
                    const glm::u8vec3 c = TileColor(tileIndex);
                    const sizet o = (static_cast<sizet>(py) * kAtlasPixels + px) * 4u;
                    pixels[o + 0] = c.r;
                    pixels[o + 1] = c.g;
                    pixels[o + 2] = c.b;
                    pixels[o + 3] = 255u;
                }
            }

            Ref<Texture2D> texture = Texture2D::Create(spec);
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        [[nodiscard]] Ref<Tileset> MakeTileset(const Ref<Texture2D>& texture)
        {
            auto tileset = Ref<Tileset>::Create();
            tileset->SetTileSize(kTilePixels, kTilePixels);
            tileset->SetTextureSize(texture->GetWidth(), texture->GetHeight());
            return tileset;
        }

        // A recognisable map: a solid border of tile 0, a diagonal of tile 5, and
        // a background checker of tiles 1 / 2. Distinct enough that a wrong UV or
        // a transposed grid is obvious in the PNG, not just in a number.
        [[nodiscard]] TilemapComponent MakeMap(u32 extent)
        {
            TilemapComponent map;
            map.Width = extent;
            map.Height = extent;
            map.TileSize = kWorldTileSize;
            const sizet ground = map.AddLayer("Ground");

            for (u32 y = 0; y < extent; ++y)
            {
                for (u32 x = 0; x < extent; ++x)
                {
                    u32 tile = ((x + y) % 2 == 0) ? 1u : 2u;
                    if (x == 0 || y == 0 || x == extent - 1 || y == extent - 1)
                        tile = 0u;
                    else if (x == y)
                        tile = 5u;
                    map.SetTile(ground, x, y, tile + 1); // biased
                }
            }
            return map;
        }

        // Fraction of pixels that differ from the framebuffer clear colour - a
        // "something actually drew here" measure.
        //
        // Compared against kClearColor8 rather than a fixed brightness cutoff on
        // purpose. A cutoff has to sit above the clear colour but below the
        // darkest thing the scene can legitimately draw, and this atlas's darkest
        // tile is a deliberately dark grey; the first version of this helper used
        // `> 40` and classified every border tile as background, putting the
        // measured fraction below its own threshold on a perfectly good frame.
        [[nodiscard]] f64 NonClearFraction(const std::vector<u8>& px)
        {
            // Tolerance covers 8-bit rounding of the float clear colour only; the
            // nearest real tile colour is far outside it.
            constexpr int kTol = 6;
            sizet nonClear = 0;
            sizet total = 0;
            for (sizet i = 0; i + 3 < px.size(); i += 4)
            {
                const bool isClear = std::abs(static_cast<int>(px[i]) - kClearColor8[0]) <= kTol &&
                                     std::abs(static_cast<int>(px[i + 1]) - kClearColor8[1]) <= kTol &&
                                     std::abs(static_cast<int>(px[i + 2]) - kClearColor8[2]) <= kTol;
                if (!isClear)
                    ++nonClear;
                ++total;
            }
            return total ? static_cast<f64>(nonClear) / static_cast<f64>(total) : 0.0;
        }
    } // namespace

    class TilemapVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // Renderer2D is driven directly here, so the fixture only has to bring up
        // the GL context and the renderer — no scene, no EnableRendering.
        void BuildScene() override {}

        struct CaptureResult
        {
            std::vector<u8> Pixels;
            TilemapRenderer::DrawStats Stats;
            u32 DrawCalls = 0;
        };

        // Renders `map` through TilemapRenderer::Draw into an offscreen
        // framebuffer with an orthographic camera centred on `cameraCenter` and
        // covering +-`halfExtent` world units, then reads the pixels back
        // top-row-first and (when `tag` is non-empty) writes the PNG.
        void Capture(const std::string& tag, const TilemapComponent& map, const Ref<Tileset>& tileset,
                     const Ref<Texture2D>& texture, const glm::vec3& cameraCenter, f32 halfExtent,
                     CaptureResult& out)
        {
            FramebufferSpecification spec;
            spec.Width = kSize;
            spec.Height = kSize;
            // Same two-colour-attachment layout as the editor scene framebuffer,
            // so the entity-id write in the 2D shader has somewhere to land.
            spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
            Ref<Framebuffer> fb = Framebuffer::Create(spec);

            {
                GLStateGuard guard("TilemapVisualEvidence", GLStateGuard::Policy::Restore);

                fb->Bind();
                fb->ClearAllAttachments(kClearColor, -1);

                RenderCommand::SetDepthTest(false);
                RenderCommand::EnableBlending();
                RenderCommand::DisableCulling();

                OrthographicCamera camera(-halfExtent, halfExtent, -halfExtent, halfExtent);
                camera.SetPosition(cameraCenter);

                Renderer2D::ResetStats();
                Renderer2D::BeginScene(camera);
                out.Stats = TilemapRenderer::Draw(map, tileset, texture, glm::mat4(1.0f),
                                                  Frustum(camera.GetViewProjectionMatrix()), -1);
                Renderer2D::EndScene();
                out.DrawCalls = Renderer2D::GetStats().DrawCalls;

                fb->Unbind();
            }

            // Read after unbinding — NVIDIA rejects glGetTextureImage on a texture
            // still attached to the bound FBO (see the sibling 2D evidence test).
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kSize, kSize, out.Pixels);
            ASSERT_EQ(out.Pixels.size(), static_cast<sizet>(kSize) * kSize * 4u);

            if (tag.empty())
                return;

            // GL readback is bottom-up; flip to top-row-first for the PNG.
            const sizet rowBytes = static_cast<sizet>(kSize) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kSize / 2u; ++y)
            {
                u8* top = out.Pixels.data() + static_cast<sizet>(y) * rowBytes;
                u8* bot = out.Pixels.data() + static_cast<sizet>(kSize - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("Tilemap_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kSize), static_cast<int>(kSize),
                                               4, out.Pixels.data(), static_cast<int>(kSize) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
            OLO_CORE_INFO("TilemapVisualEvidence: wrote {} (abs: {})", path, fs::absolute(path).string());
        }
    };

    TEST_F(TilemapVisualEvidenceTest, DrawsTheMapAndTheAtlasTileUnderEachCell)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto texture = MakeAtlasTexture();
        auto tileset = MakeTileset(texture);
        const TilemapComponent map = MakeMap(kMapSize);

        // Camera centred on the map, framing all of it with a small margin.
        const f32 half = 0.5f * static_cast<f32>(kMapSize) * kWorldTileSize;
        const glm::vec3 center{ half, half, 0.0f };

        CaptureResult overview;
        ASSERT_NO_FATAL_FAILURE(Capture("Overview", map, tileset, texture, center, half * 1.05f, overview));

        EXPECT_EQ(overview.Stats.TilesSubmitted, kMapSize * kMapSize)
            << "Every cell of the fully-painted map should have been submitted when it is all on screen.";
        EXPECT_GT(NonClearFraction(overview.Pixels), 0.8)
            << "The map fills the frame, so almost every pixel should be a tile, not the clear colour.";

        CaptureResult zoom;
        ASSERT_NO_FATAL_FAILURE(Capture("Zoom", map, tileset, texture,
                                        glm::vec3(4.0f, 4.0f, 0.0f), 3.0f, zoom));
        EXPECT_GT(NonClearFraction(zoom.Pixels), 0.9);
    }

    TEST_F(TilemapVisualEvidenceTest, EachCellShowsItsOwnAtlasTile)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The tightest contract in this file: sample the framebuffer at the centre
        // of specific cells and require the colour the atlas painted for that
        // tile. A flipped V axis, a swapped row/column, or an off-by-one in the
        // biased index all change which colour lands here.
        auto texture = MakeAtlasTexture();
        auto tileset = MakeTileset(texture);
        const TilemapComponent map = MakeMap(kMapSize);

        const f32 half = 0.5f * static_cast<f32>(kMapSize) * kWorldTileSize;
        const glm::vec3 center{ half, half, 0.0f };
        const f32 viewHalf = half; // exactly frames the map: 1 tile == kSize/kMapSize px

        CaptureResult shot;
        ASSERT_NO_FATAL_FAILURE(Capture("", map, tileset, texture, center, viewHalf, shot));
        // Not flipped (tag was empty), so row 0 of `Pixels` is world-space BOTTOM,
        // which matches the tile grid's own +Y-up ordering.

        const f32 pixelsPerTile = static_cast<f32>(kSize) / static_cast<f32>(kMapSize);
        auto sampleCell = [&](u32 tx, u32 ty)
        {
            const u32 px = static_cast<u32>((static_cast<f32>(tx) + 0.5f) * pixelsPerTile);
            const u32 py = static_cast<u32>((static_cast<f32>(ty) + 0.5f) * pixelsPerTile);
            const sizet o = (static_cast<sizet>(py) * kSize + px) * 4u;
            return glm::u8vec3{ shot.Pixels[o], shot.Pixels[o + 1], shot.Pixels[o + 2] };
        };

        // Tolerance absorbs blend/rounding in the 8-bit path; the palette's
        // entries are 160 apart per channel, far outside it.
        constexpr int kTol = 24;
        auto expectTile = [&](u32 tx, u32 ty, u32 expectedTileIndex)
        {
            const glm::u8vec3 got = sampleCell(tx, ty);
            const glm::u8vec3 want = TileColor(expectedTileIndex);
            EXPECT_NEAR(static_cast<int>(got.r), static_cast<int>(want.r), kTol)
                << "cell (" << tx << ", " << ty << ") expected atlas tile " << expectedTileIndex;
            EXPECT_NEAR(static_cast<int>(got.g), static_cast<int>(want.g), kTol)
                << "cell (" << tx << ", " << ty << ") expected atlas tile " << expectedTileIndex;
            EXPECT_NEAR(static_cast<int>(got.b), static_cast<int>(want.b), kTol)
                << "cell (" << tx << ", " << ty << ") expected atlas tile " << expectedTileIndex;
        };

        // Border cells carry tile 0 (atlas top-left).
        expectTile(0, 0, 0);
        expectTile(kMapSize - 1, 0, 0);
        expectTile(0, kMapSize - 1, 0);
        // The diagonal carries tile 5 (atlas row 1, column 1) — an interior tile,
        // so both the U and the V offset have to be right for it to match.
        expectTile(5, 5, 5);
        expectTile(10, 10, 5);
        // Checker cells: (x + y) even -> tile 1, odd -> tile 2.
        expectTile(4, 6, 1);
        expectTile(4, 7, 2);
    }

    TEST_F(TilemapVisualEvidenceTest, ChunkCullingKeepsTheCostProportionalToTheView)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto texture = MakeAtlasTexture();
        auto tileset = MakeTileset(texture);

        // The same camera over a small map and a map 100x its area. Culling means
        // the visible tile count and the draw-call count must not move.
        const glm::vec3 center{ 4.0f, 4.0f, 0.0f };
        constexpr f32 kViewHalf = 3.0f;

        const TilemapComponent small = MakeMap(kMapSize);
        const TilemapComponent large = MakeMap(kMapSize * 10);

        CaptureResult smallShot;
        CaptureResult largeShot;
        ASSERT_NO_FATAL_FAILURE(Capture("", small, tileset, texture, center, kViewHalf, smallShot));
        ASSERT_NO_FATAL_FAILURE(Capture("LargeMapSameView", large, tileset, texture, center, kViewHalf, largeShot));

        EXPECT_GT(largeShot.Stats.ChunksTotal, smallShot.Stats.ChunksTotal)
            << "The larger map must actually have more chunks, or this test proves nothing.";
        EXPECT_EQ(largeShot.Stats.ChunksVisible, smallShot.Stats.ChunksVisible)
            << "The same view over a bigger map visited a different number of chunks — the culler "
               "is scaling with map size.";
        EXPECT_EQ(largeShot.Stats.TilesSubmitted, smallShot.Stats.TilesSubmitted)
            << "Tile submissions scaled with map size instead of with the view.";
        EXPECT_EQ(largeShot.DrawCalls, smallShot.DrawCalls)
            << "Draw calls scaled with map size instead of with the view.";
    }

    TEST_F(TilemapVisualEvidenceTest, AMapEntirelyOffScreenSubmitsNothing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto texture = MakeAtlasTexture();
        auto tileset = MakeTileset(texture);
        const TilemapComponent map = MakeMap(kMapSize);

        // Aim far away from the map's world extent, which is [0, 24] on both axes.
        CaptureResult shot;
        ASSERT_NO_FATAL_FAILURE(Capture("OffScreen", map, tileset, texture,
                                        glm::vec3(500.0f, 500.0f, 0.0f), 5.0f, shot));

        // Both halves matter: an empty PNG alone would also be produced by a
        // culler that submits every tile and lets the rasteriser discard them.
        EXPECT_EQ(shot.Stats.ChunksVisible, 0u) << "The chunk culler kept off-screen chunks.";
        EXPECT_EQ(shot.Stats.TilesSubmitted, 0u);
        EXPECT_LT(NonClearFraction(shot.Pixels), 0.01);
    }
} // namespace OloEngine::Tests
