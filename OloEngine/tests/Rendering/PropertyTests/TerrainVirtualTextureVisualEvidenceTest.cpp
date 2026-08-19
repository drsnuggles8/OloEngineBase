// OLO_TEST_LAYER: L8
//
// Visual evidence for terrain adaptive virtual texturing (issue #715, slice 1).
//
// TerrainVirtualTextureTest pins the arithmetic — the packings, the fallback
// resolution, the LRU touch order. None of that proves the LOOP runs: the
// feedback buffer is written by a fragment stage, copied under a fence, read
// back on a later frame, reduced on a Task worker, turned into page allocations,
// composited by a compute kernel and published through three more kernels. Every
// one of those is a real GPU/threading step that a headless contract test cannot
// reach, and the failure mode of any of them is the same — the terrain keeps
// rendering, from the splat path or from the coarsest page, and nothing logs an
// error.
//
// So this file asserts the three things only a live frame can answer:
//
//   * **The loop converges.** Residency, request and bake counters move off zero
//     within a bounded number of frames. This is the spike the issue made a
//     precondition, run against real hardware.
//   * **The VT path reproduces the splat path.** Same scene, same pose, the two
//     surfacing paths differ by less than a calibrated bound. They composite the
//     same blend, so a large difference means an addressing bug, not a quality
//     difference — and this is the assertion that catches a page key, a physical
//     UV or a tile-border mistake, all of which render plausible, wrong content.
//   * **Motion does not pop.** A camera flying in over the terrain produces a
//     frame-to-frame difference that stays inside the same band the splat path
//     produces. A page arriving mid-motion must sharpen the surface, not replace
//     it — and the coarse-mip fallback is the only reason that holds.
//
// SKIPs cleanly without a GL 4.6 context, like every other evidence test here.
#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTexture.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        [[nodiscard]] f32 Luma(u8 r, u8 g, u8 b)
        {
            return 0.2126f * static_cast<f32>(r) + 0.7152f * static_cast<f32>(g) + 0.0722f * static_cast<f32>(b);
        }

        // Mean absolute per-channel difference, 0-255. Mean rather than max: a
        // single-pixel outlier at a page boundary is expected (the two paths pick
        // their sampling LOD differently), while a systematic addressing error
        // moves whole regions.
        [[nodiscard]] f32 MeanAbsoluteDifference(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
            {
                return 255.0f;
            }
            f64 total = 0.0;
            sizet samples = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                for (sizet c = 0; c < 3; ++c)
                {
                    total += std::abs(static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]));
                    ++samples;
                }
            }
            return samples ? static_cast<f32>(total / static_cast<f64>(samples)) : 255.0f;
        }

        // Fraction of pixels that are NOT the editor grid's exact-neutral grey —
        // i.e. actually shaded terrain. Same discriminator, and the same reason
        // for it, as TerrainGPUQuadtreeVisualEvidenceTest: the grid is untextured
        // so its channels come out bit-identical, and lit terrain essentially
        // never does.
        [[nodiscard]] f32 LitFraction(const std::vector<u8>& px)
        {
            if (px.empty())
            {
                return 0.0f;
            }
            sizet lit = 0;
            sizet total = 0;
            for (sizet i = 0; i + 3 < px.size(); i += 4)
            {
                const bool neutral = px[i] == px[i + 1] && px[i + 1] == px[i + 2];
                const f32 luma = Luma(px[i], px[i + 1], px[i + 2]);
                if (!(neutral && luma >= 118.0f && luma <= 170.0f))
                {
                    ++lit;
                }
                ++total;
            }
            return total ? static_cast<f32>(lit) / static_cast<f32>(total) : 0.0f;
        }

        void MaybeWritePng(const std::string& name, const std::vector<u8>& px)
        {
            if (!GoldenRebaseRequested())
            {
                return;
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("TerrainVirtualTexture_" + name + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, px.data(),
                             static_cast<int>(kWidth) * 4);
        }
    } // namespace

    class TerrainVirtualTextureVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            {
                m_TerrainEntity = scene.CreateEntity("Terrain");
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 715715;
                terrain.m_ProceduralResolution = 256;
                terrain.m_ProceduralOctaves = 6;
                terrain.m_ProceduralFrequency = 3.0f;
                terrain.m_HeightShaping.RidgeBlend = 0.45f;
                terrain.m_HeightShaping.WarpStrength = 0.12f;
                terrain.m_WorldSizeX = 1024.0f;
                terrain.m_WorldSizeZ = 1024.0f;
                terrain.m_HeightScale = 120.0f;
                terrain.m_TessellationEnabled = true;

                // Small on purpose. The default (256 pages of 128 texels into a
                // 16x16 tile cache) is ~38 MB and needs tens of frames to fill;
                // this converges inside kWarmupFrames while exercising every step
                // of the loop identically.
                terrain.m_VirtualTextureEnabled = true;
                terrain.m_VTVirtualPagesWide = 32;
                terrain.m_VTPageTexels = 64;
                terrain.m_VTBorderTexels = 4;
                terrain.m_VTCacheTilesWide = 12;
                terrain.m_VTMaxTileBakesPerFrame = 16;

                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                {
                    terrain.m_Material->AddLayer(layer);
                }
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;
            }
        }

        // Long enough for the whole loop to turn over several times: feedback is
        // written on frame N, captured on N+1, reduced on a worker, and only then
        // does the first allocation happen — so nothing at all is resident before
        // roughly frame 3, and the fine pages need the bake budget to catch up.
        static constexpr u32 kWarmupFrames = 40;

        [[nodiscard]] TerrainComponent& Terrain()
        {
            return m_TerrainEntity.GetComponent<TerrainComponent>();
        }

        // Pose, tick, read back. Callers MUST wrap this in
        // ASSERT_NO_FATAL_FAILURE — the ASSERTs below return from the helper, not
        // from the test, and an empty capture would otherwise be compared against
        // another empty one and pass.
        void Capture(const glm::vec3& eye, f32 yaw, f32 pitch, u32 frames, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 6000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);

            RunEditorFrames(camera, frames);

            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, w, h)) << "no composited frame";
            ASSERT_EQ(w, kWidth);
            ASSERT_EQ(h, kHeight);
        }

        Entity m_TerrainEntity;
    };

    // ── The spike, run for real ───────────────────────────────────────────────
    //
    // Every counter here is downstream of a different step of the loop, so which
    // one is zero says which step is broken:
    //   feedback texels == 0  -> the fragment stage is not writing, or the
    //                            readback never completes
    //   pages requested <= 1  -> the analysis ran but decoded nothing (only the
    //                            pinned page survives)
    //   tiles baked == 0      -> requests exist but no allocation happened
    //   not ready for shading -> the pinned page never became resident, so the
    //                            fallback chain has no terminator
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, TheFeedbackLoopConvergesOnRealHardware)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> frame;
        ASSERT_NO_FATAL_FAILURE(Capture(glm::vec3(512.0f, 180.0f, 900.0f), 0.0f, 0.20f, kWarmupFrames, frame));
        MaybeWritePng("converged", frame);

        const TerrainComponent& terrain = Terrain();
        ASSERT_TRUE(terrain.m_VirtualTexture) << "the terrain never created a virtual texture";
        const auto& stats = terrain.m_VirtualTexture->GetStats();

        EXPECT_GT(stats.m_FeedbackTexelsWritten, 0u)
            << "no feedback texel carried a request after " << kWarmupFrames
            << " frames — the fragment stage is not writing the buffer, or the fenced readback never "
               "completed";
        EXPECT_GT(stats.m_PagesRequested, 1u)
            << "the analysis produced only the pinned page — the feedback words decoded to nothing";
        EXPECT_GT(stats.m_TilesBakedTotal, 0u) << "no cache tile was ever composited";
        EXPECT_GT(stats.m_ResidentTiles, 1u) << "the page cache holds nothing beyond the pinned page";
        EXPECT_LE(stats.m_ResidentTiles, stats.m_CacheTileCount);
        EXPECT_TRUE(terrain.m_VirtualTexture->IsReadyForShading())
            << "the coarsest page never became resident, so every lookup falls back to an unmapped texel";

        // Anti-vacuous: all of the above can be true of a frame that renders no
        // terrain at all.
        EXPECT_GT(LitFraction(frame), 0.20f) << "the converged frame is almost entirely background";
    }

    // ── The addressing proof ──────────────────────────────────────────────────
    //
    // The two paths composite the SAME blend from the same inputs, so once the
    // cache has converged they must look alike. They are not expected to be
    // identical: the bake picks an explicit LOD from the page's texel density
    // while the fragment path uses screen-space derivatives, and a page one mip
    // coarser than the pixel wanted is legitimately blurrier.
    //
    // What a large difference means is an ADDRESS being wrong — a page key
    // packed off by a bit, a physical UV landing in a tile's border, a fallback
    // resolving at the requested mip instead of the resident one. All three
    // render plausible content in the wrong place, which no "does it look like
    // terrain" check can see.
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, VirtualTexturePathReproducesTheSplatPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 512.0f, 180.0f, 900.0f };

        std::vector<u8> vtFrame;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, kWarmupFrames, vtFrame));
        MaybeWritePng("vt_on", vtFrame);
        ASSERT_TRUE(Terrain().m_VirtualTexture && Terrain().m_VirtualTexture->IsReadyForShading())
            << "the VT half of the comparison never converged, so the two frames would both be the splat path";

        Terrain().m_VirtualTextureEnabled = false;
        std::vector<u8> splatFrame;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, 4, splatFrame));
        MaybeWritePng("vt_off", splatFrame);

        // Anti-vacuous, in both directions: two blank frames match perfectly.
        ASSERT_GT(LitFraction(vtFrame), 0.20f) << "the VT frame is almost entirely background";
        ASSERT_GT(LitFraction(splatFrame), 0.20f) << "the splat frame is almost entirely background";

        const f32 difference = MeanAbsoluteDifference(vtFrame, splatFrame);
        // Deliberately loose. The bound that matters is "the same surface", not
        // "the same pixels": a systematic addressing error moves whole regions
        // and lands far above this, while the legitimate LOD difference between
        // the two paths is a few levels of blur.
        EXPECT_LT(difference, 32.0f)
            << "the virtual-texture frame differs from the splat frame by " << difference
            << "/255 per channel on average — that is a different SURFACE, not a different sharpness. "
               "Suspect the page key, the physical-UV mapping or the fallback mip.";
    }

    // ── Criterion 2's second half: no visible pop under movement ──────────────
    //
    // A still screenshot cannot prove this, so the shape of the test is: fly the
    // camera in along a path that forces new pages every step, and require the
    // frame-to-frame difference to stay inside the band the SPLAT path produces
    // over the identical path. The splat path cannot pop by construction (it has
    // no residency), so it IS the noise floor for "how much does this scene
    // change per step of this camera" — which is the calibration
    // live-verification-noise-floor.md asks for, done inside the test instead of
    // being guessed at as a constant.
    //
    // A page arriving without the coarse-mip fallback replaces a whole screen
    // region in one frame, which is far outside that band. A page arriving WITH
    // it changes the same region's sharpness, which is inside it.
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, CameraMotionProducesNoStepLargerThanTheSplatPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // A fly-in: each step roughly halves the distance to the surface, so the
        // wanted mip drops by about one level per step and every step needs pages
        // that were not resident before. That is the worst case for popping.
        const std::vector<glm::vec3> path{
            glm::vec3(512.0f, 420.0f, 1100.0f),
            glm::vec3(512.0f, 320.0f, 980.0f),
            glm::vec3(512.0f, 240.0f, 880.0f),
            glm::vec3(512.0f, 180.0f, 800.0f),
            glm::vec3(512.0f, 140.0f, 740.0f),
            glm::vec3(512.0f, 110.0f, 700.0f),
        };

        const auto walk = [this, &path](std::vector<f32>& outSteps, const char* tag)
        {
            std::vector<u8> previous;
            for (sizet i = 0; i < path.size(); ++i)
            {
                std::vector<u8> current;
                // Two frames per step: enough to render the new pose, not enough
                // to let the cache fully converge — which is the point. A test
                // that waited for convergence at every step would never see a
                // page arrive mid-motion.
                ASSERT_NO_FATAL_FAILURE(Capture(path[i], 0.0f, 0.22f, 2, current));
                if (!previous.empty())
                {
                    outSteps.push_back(MeanAbsoluteDifference(previous, current));
                }
                if (i + 1 == path.size())
                {
                    MaybeWritePng(std::string("motion_") + tag, current);
                }
                previous = std::move(current);
            }
        };

        // Warm the cache first, so the walk measures steady-state motion rather
        // than the cold-start ramp (which legitimately changes the frame a lot,
        // and which IsReadyForShading already gates the VT branch behind).
        std::vector<u8> warm;
        ASSERT_NO_FATAL_FAILURE(Capture(path.front(), 0.0f, 0.22f, kWarmupFrames, warm));
        ASSERT_TRUE(Terrain().m_VirtualTexture && Terrain().m_VirtualTexture->IsReadyForShading());

        std::vector<f32> vtSteps;
        ASSERT_NO_FATAL_FAILURE(walk(vtSteps, "vt"));

        Terrain().m_VirtualTextureEnabled = false;
        std::vector<u8> splatWarm;
        ASSERT_NO_FATAL_FAILURE(Capture(path.front(), 0.0f, 0.22f, 4, splatWarm));
        std::vector<f32> splatSteps;
        ASSERT_NO_FATAL_FAILURE(walk(splatSteps, "splat"));

        ASSERT_EQ(vtSteps.size(), splatSteps.size());
        ASSERT_FALSE(vtSteps.empty());

        const f32 vtWorst = *std::ranges::max_element(vtSteps);
        const f32 splatWorst = *std::ranges::max_element(splatSteps);

        // Anti-vacuous: a camera that did not actually move produces zero steps
        // on BOTH paths and the ratio below is meaningless.
        EXPECT_GT(splatWorst, 1.0f) << "the reference walk barely changed the frame — the path is not exercising "
                                       "anything, so the comparison proves nothing";

        // The allowance is for the one thing the VT path legitimately does that
        // the splat path does not: a step can also change the RESOLUTION of a
        // region, on top of the geometry change both paths see.
        EXPECT_LT(vtWorst, splatWorst * 2.0f + 4.0f)
            << "the largest VT frame-to-frame step was " << vtWorst << "/255 against the splat path's "
            << splatWorst
            << " over the identical camera path — a page is replacing a screen region rather than sharpening "
               "it. Suspect the coarse-to-fine fill (TerrainVTIndirectionFill.comp) or the pinned coarsest "
               "page.";
    }
} // namespace OloEngine::Tests
