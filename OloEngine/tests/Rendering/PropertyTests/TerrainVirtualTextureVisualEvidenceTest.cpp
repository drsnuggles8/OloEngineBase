// OLO_TEST_LAYER: L8
//
// Visual evidence for terrain adaptive virtual texturing (issue #715, all four
// slices; the fixture pins the fixed-grid slices-1-2 config, and the adaptive /
// BC7 tests opt into slices 3-4 per test).
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
//   * **The delta publishes the same map as the full rebuild** (slice 2). The
//     headless equivalence test compares a CPU MODEL of the three kernels; this
//     compares the kernels, which is where a GLSL-only mistake — a fill
//     rectangle, an std430 header declared differently in two shaders — would
//     live.
//
// SKIPs cleanly without a GL 4.6 context, like every other evidence test here.
#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Core/DebugLevers.h"
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
#include <iostream>
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
                // PINNED to the fixed-grid, uncompressed, single-fetch path.
                // These tests and their goldens were written against slices
                // 1-2's exact behavior (the delta-vs-rebuild equivalence, the
                // VT-vs-splat reproduction), and that path must stay
                // measurable after slices 3-4 changed the defaults — the
                // adaptive/BC configs get their own evidence tests instead of
                // silently rewriting what these ones mean.
                terrain.m_VTAdaptiveEnabled = false;
                terrain.m_VTTrilinearEnabled = false;
                terrain.m_VTCompressedCache = false;

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

    // ── Slice 2: the delta must publish the same map the rebuild did ──────────
    //
    // TerrainVirtualTexture.TheDeltaProducesTheSameMapAsAFullRebuildOverRandomTraffic
    // pins this headlessly, but against a CPU MODEL of the three kernels. What it
    // cannot reach is the kernels themselves — and slice 2 changed two of them
    // (the write list became a delta carrying unmappings; the propagation gained
    // a sub-rectangle). A rect that is right on the CPU and wrong in GLSL, or an
    // std430 header whose two declarations drifted apart, passes that test and
    // renders the wrong terrain.
    //
    // So: the same scene, the same pose, published both ways, compared against a
    // measured noise floor rather than a guessed constant
    // (docs/agent-rules/live-verification-noise-floor.md). The floor here is a
    // SECOND delta-path run through the identical invalidate-and-reconverge
    // cycle, so the only thing that differs between it and the rebuild run is the
    // publish path — page-to-tile assignment, bake order and analysis-completion
    // order all vary in both.
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, TheDeltaPublishesTheSameFrameAsTheFullRebuild)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 512.0f, 200.0f, 880.0f };

        // Restored however the test leaves: the lever is process-wide state, and
        // a test that leaks it on would silently turn the delta path off for
        // everything that runs after it.
        const bool leverWasSet = Levers::TerrainVtFullRebuild();
        struct LeverGuard
        {
            bool m_Restore;
            ~LeverGuard()
            {
                Levers::SetTerrainVtFullRebuild(m_Restore);
            }
        } guard{ leverWasSet };
        Levers::SetTerrainVtFullRebuild(false);

        const auto reconverge = [this, &eye](std::vector<u8>& out)
        {
            // Invalidate, not just re-pose: dropping every page is what forces a
            // full re-publish, and without it a converged camera changes no
            // residency and the lever below would never actually run.
            ASSERT_TRUE(Terrain().m_VirtualTexture);
            Terrain().m_VirtualTexture->Invalidate();
            ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, kWarmupFrames, out));
            ASSERT_TRUE(Terrain().m_VirtualTexture->IsReadyForShading())
                << "never reconverged, so the frames being compared are the splat path twice";
        };

        std::vector<u8> deltaA;
        std::vector<u8> deltaB;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, kWarmupFrames, deltaA));
        ASSERT_TRUE(Terrain().m_VirtualTexture && Terrain().m_VirtualTexture->IsReadyForShading());
        const TerrainVirtualTexture::Stats afterFirst = Terrain().m_VirtualTexture->GetStats();

        ASSERT_NO_FATAL_FAILURE(reconverge(deltaA));
        ASSERT_NO_FATAL_FAILURE(reconverge(deltaB));
        const TerrainVirtualTexture::Stats afterDelta = Terrain().m_VirtualTexture->GetStats();

        Levers::SetTerrainVtFullRebuild(true);
        std::vector<u8> rebuilt;
        ASSERT_NO_FATAL_FAILURE(reconverge(rebuilt));
        const TerrainVirtualTexture::Stats afterRebuild = Terrain().m_VirtualTexture->GetStats();

        MaybeWritePng("publish_delta", deltaA);
        MaybeWritePng("publish_rebuild", rebuilt);

        // Anti-vacuous #1: there is terrain in these frames at all.
        ASSERT_GT(LitFraction(deltaA), 0.20f) << "the delta frame is almost entirely background";
        ASSERT_GT(LitFraction(rebuilt), 0.20f) << "the rebuild frame is almost entirely background";
        // deltaB is the NOISE FLOOR, so a blank one is worse than a blank
        // subject: it inflates the bound instead of failing, and a genuinely
        // wrong rebuild frame then passes underneath it.
        ASSERT_GT(LitFraction(deltaB), 0.20f) << "the second delta frame — the noise floor — is almost "
                                                 "entirely background, so the bound below is meaningless";

        // Anti-vacuous #2: the two runs really did take different paths. Without
        // this the test passes just as happily when the lever does nothing, which
        // is precisely the bug it would be asked to catch.
        const u32 deltaPublishes = afterDelta.m_IndirectionPublishes - afterFirst.m_IndirectionPublishes;
        const u32 deltaRebuilds = afterDelta.m_IndirectionFullRebuilds - afterFirst.m_IndirectionFullRebuilds;
        const u32 forcedPublishes = afterRebuild.m_IndirectionPublishes - afterDelta.m_IndirectionPublishes;
        const u32 forcedRebuilds = afterRebuild.m_IndirectionFullRebuilds - afterDelta.m_IndirectionFullRebuilds;
        ASSERT_GT(deltaPublishes, 0u) << "nothing was published on the delta runs";
        EXPECT_EQ(deltaRebuilds, 0u) << "the delta runs fell back to a rebuild — the comparison below is vacuous";
        ASSERT_GT(forcedPublishes, 0u) << "nothing was published on the forced-rebuild run";
        EXPECT_EQ(forcedRebuilds, forcedPublishes)
            << "OLO_TERRAIN_VT_FULL_REBUILD did not take: " << forcedRebuilds << " of " << forcedPublishes
            << " publishes rebuilt";

        // The floor: two delta runs through the identical cycle. Not zero —
        // page-to-tile assignment and bake order depend on when the analysis
        // worker finishes, so the same pose reconverges to the same SURFACE
        // through a differently-packed cache.
        const f32 noiseFloor = MeanAbsoluteDifference(deltaA, deltaB);
        const f32 difference = MeanAbsoluteDifference(deltaA, rebuilt);

        // 3x the floor, with an absolute term so a run whose floor happens to be
        // zero does not demand bit-equality of something that is not required to
        // be bit-equal. An addressing bug is not a near miss: the splat-path
        // comparison above tolerates 32/255 and a wrong page lands well beyond
        // even that.
        const f32 bound = std::max(3.0f * noiseFloor, 1.0f);
        EXPECT_LT(difference, bound)
            << "publishing the indirection map by delta and by full rebuild produced different frames: "
            << difference << "/255 per channel against a measured floor of " << noiseFloor
            << ". The two paths are required to produce the same map — suspect the fill rectangle "
               "(TerrainVTIndirectionFill.comp's u_VTFillParams), the unmap entries, or the std430 header "
               "declaration drifting between the write and fill kernels.";

        // The cost claim, pinned on the GPU path rather than only in arithmetic.
        // Both counters describe the LAST publish, and the last publish of each
        // run took the path that run was forcing, so this is like for like.
        const u64 deltaTexels = static_cast<u64>(afterDelta.m_IndirectionTexelsWritten) +
                                afterDelta.m_IndirectionTexelsFilled;
        const u64 rebuildTexels = static_cast<u64>(afterRebuild.m_IndirectionTexelsWritten) +
                                  afterRebuild.m_IndirectionTexelsFilled;
        EXPECT_LT(deltaTexels, rebuildTexels)
            << "a delta publish touched " << deltaTexels << " texels and a rebuild " << rebuildTexels
            << " — the delta is supposed to be the cheap one";

        // Reported, not asserted. The texel counts are deterministic and the
        // assertion above uses them; the GPU milliseconds are the LOWEST sample
        // each path produced during this run, which is the most a timestamp pair
        // around a handful of small dispatches can honestly say (see the Stats
        // field's note). Printed because a claim about cost with no number
        // attached is exactly what this slice was told not to make.
        std::cout << "[ vt-cost  ] indirection publish, last publish of each run:\n"
                  << "[ vt-cost  ]   delta   " << deltaTexels << " texels; best GPU sample "
                  << afterDelta.m_IndirectionDeltaGpuMs << " ms\n"
                  << "[ vt-cost  ]   rebuild " << rebuildTexels << " texels; best GPU sample "
                  << afterRebuild.m_IndirectionRebuildGpuMs << " ms\n"
                  << "[ vt-cost  ] published on " << afterRebuild.m_IndirectionPublishes << " of "
                  << afterRebuild.m_FramesUpdated << " frames\n";
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

    // ── Slice 3: images grow where the camera actually looks ─────────────────
    //
    // The headless policy tests pin VTDesiredImageSize against synthetic
    // feedback; what they cannot reach is the loop that FEEDS it — the shader
    // encoding a below-zero mip as the biased 0, the analyzer attributing words
    // to sectors, the resize remapping resident pages through the cache and the
    // delta. Any of those broken leaves every image at the minimum forever, and
    // the terrain still renders — coarse, from the pinned pages, with no error.
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, AdaptiveImagesGrowWhereTheCameraLooks)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // 4x4 sectors over a 32-page atlas, images 1..8 pages: small enough to
        // converge inside the warmup, big enough that growth is observable.
        // Uncompressed, single-fetch — this test is about the adaptive loop,
        // and the fewer moving parts under it the sharper its failure reads.
        TerrainComponent& terrain = Terrain();
        terrain.m_VTAdaptiveEnabled = true;
        terrain.m_VTSectorsWide = 4;
        terrain.m_VTMaxImagePagesWide = 8;
        terrain.m_VTTrilinearEnabled = false;
        terrain.m_VTCompressedCache = false;

        std::vector<u8> frame;
        ASSERT_NO_FATAL_FAILURE(Capture(glm::vec3(512.0f, 180.0f, 900.0f), 0.0f, 0.20f, kWarmupFrames * 2, frame));
        MaybeWritePng("adaptive_grown", frame);
        ASSERT_GT(LitFraction(frame), 0.20f) << "the adaptive frame is almost entirely background";

        ASSERT_TRUE(terrain.m_VirtualTexture);
        const auto& stats = terrain.m_VirtualTexture->GetStats();
        EXPECT_EQ(stats.m_SectorCount, 16u);
        EXPECT_GT(stats.m_SectorsReady, 0u) << "no sector's pinned page ever became resident";
        EXPECT_TRUE(terrain.m_VirtualTexture->IsReadyForShading());

        // The adaptive claims, each downstream of a different piece:
        //   resizes == 0        -> the grow signal never survived the encoding,
        //                          the analyzer, or the streak hysteresis
        //   atlas == sectors    -> every image is still at the 1-page minimum
        //   remapped == 0       -> resizes happened but carried nothing across,
        //                          i.e. every grow rebaked from scratch
        EXPECT_GT(stats.m_ImageResizesTotal, 0u)
            << "no image ever resized — the under-resolved feedback signal (biased mip 0) is not reaching "
               "VTDesiredImageSize";
        EXPECT_GT(stats.m_AtlasPagesAllocated, stats.m_SectorCount)
            << "every image is still at the 1-page minimum after " << (kWarmupFrames * 2) << " frames";
        EXPECT_GT(stats.m_PagesRemappedTotal, 0u)
            << "images resized but no resident page was carried across — a resize is required to REMAP, "
               "not drop and rebake";
        EXPECT_EQ(stats.m_ImageAllocFailures, 0u)
            << "the atlas ran out of space in a configuration sized to never run out";
    }

    // ── Slices 3+4 together: the shipping config still reproduces the splat path
    //
    // The same addressing proof as VirtualTexturePathReproducesTheSplatPath, but
    // through every new moving part at once: sector-table UV mapping, variable
    // image sizes mid-growth, trilinear's second fetch, and BC7 tiles. Each of
    // those can be wrong in a way that renders plausible content — a sector rect
    // off by one maps a neighbouring sector's terrain, a BC7 bit-packing slip
    // shifts every block's endpoints — and the splat path is the ground truth
    // that catches all of them with one comparison.
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, AdaptiveCompressedTrilinearReproducesTheSplatPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TerrainComponent& terrain = Terrain();
        terrain.m_VTAdaptiveEnabled = true;
        terrain.m_VTSectorsWide = 4;
        terrain.m_VTMaxImagePagesWide = 8;
        terrain.m_VTTrilinearEnabled = true;
        terrain.m_VTCompressedCache = true;

        const glm::vec3 eye{ 512.0f, 180.0f, 900.0f };
        std::vector<u8> vtFrame;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, kWarmupFrames * 2, vtFrame));
        MaybeWritePng("adaptive_bc7_on", vtFrame);
        ASSERT_TRUE(terrain.m_VirtualTexture && terrain.m_VirtualTexture->IsReadyForShading())
            << "the adaptive+BC7 half never converged, so both frames below would be the splat path";
        const auto& stats = terrain.m_VirtualTexture->GetStats();
        EXPECT_TRUE(stats.m_CacheCompressed);
        EXPECT_GT(stats.m_TilesCompressedTotal, 0u) << "the compress kernel never ran";

        terrain.m_VirtualTextureEnabled = false;
        std::vector<u8> splatFrame;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, 4, splatFrame));

        ASSERT_GT(LitFraction(vtFrame), 0.20f) << "the VT frame is almost entirely background";
        ASSERT_GT(LitFraction(splatFrame), 0.20f) << "the splat frame is almost entirely background";

        const f32 difference = MeanAbsoluteDifference(vtFrame, splatFrame);
        // The same bound as the fixed-grid comparison, and deliberately so: BC7
        // error is ~1-2/255 and trilinear only softens mip transitions, so the
        // new features earn no extra allowance. A sector or block addressing
        // bug lands far beyond it.
        EXPECT_LT(difference, 32.0f)
            << "the adaptive+BC7+trilinear frame differs from the splat frame by " << difference
            << "/255 per channel — a different SURFACE. Suspect the sector table's UV rects, the remap "
               "arithmetic, or the BC7 block layout.";
    }

    // ── Slice 4's cost claim, pinned on the frame and the byte counter ───────
    //
    // The BC7 A/B on the fixed grid, isolated from adaptivity: same scene, same
    // pose, converged once with compressed tiles and once without. The bytes
    // must be exactly a quarter; the frames must be the same surface. The
    // second half matters because a compressor that produced garbage would
    // still hit the byte target perfectly.
    TEST_F(TerrainVirtualTextureVisualEvidenceTest, TheCompressedCacheCostsAQuarterAndKeepsTheSurface)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 512.0f, 180.0f, 900.0f };

        Terrain().m_VTCompressedCache = true;
        std::vector<u8> compressedFrame;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, kWarmupFrames, compressedFrame));
        MaybeWritePng("bc7_cache", compressedFrame);
        ASSERT_TRUE(Terrain().m_VirtualTexture && Terrain().m_VirtualTexture->IsReadyForShading());
        const u64 compressedBytes = Terrain().m_VirtualTexture->GetStats().m_CacheBytes;
        EXPECT_GT(Terrain().m_VirtualTexture->GetStats().m_TilesCompressedTotal, 0u);

        // Flipping the knob reconfigures the whole VT (Configure() sees a
        // different config), so the second run reconverges from scratch —
        // which also makes it a fresh exercise of the uncompressed path.
        Terrain().m_VTCompressedCache = false;
        std::vector<u8> rawFrame;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 0.20f, kWarmupFrames, rawFrame));
        ASSERT_TRUE(Terrain().m_VirtualTexture && Terrain().m_VirtualTexture->IsReadyForShading());
        const u64 rawBytes = Terrain().m_VirtualTexture->GetStats().m_CacheBytes;

        EXPECT_EQ(rawBytes, compressedBytes * 4u)
            << "BC7 is 1 byte per texel against RGBA8's 4 — the cache byte accounting disagrees";

        ASSERT_GT(LitFraction(compressedFrame), 0.20f);
        ASSERT_GT(LitFraction(rawFrame), 0.20f);
        const f32 difference = MeanAbsoluteDifference(compressedFrame, rawFrame);
        // Well under the 32/255 addressing bound: BC7 mode-6 error on this
        // content measures ~1-2/255, and the two runs' independent cache
        // packing adds at most the delta test's noise floor on top.
        EXPECT_LT(difference, 8.0f)
            << "the BC7 cache differs from the RGBA8 cache by " << difference
            << "/255 per channel — that is compression damage or a block-address slip, not codec noise";
    }
} // namespace OloEngine::Tests
