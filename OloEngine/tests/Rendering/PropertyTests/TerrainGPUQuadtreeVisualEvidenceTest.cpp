// OLO_TEST_LAYER: L8
//
// Visual evidence for the GPU terrain LOD quadtree (issue #714).
//
// The CPU quadtree it replaces mapped each selected node to ONE chunk mesh via
// TerrainChunkManager::FindChunkForNode — the chunk at the node's centre. That
// is correct only while every selected node is a leaf (leaf == chunk); the
// moment the descent picks a node one level up, the node covers 2x2 chunks and
// three of them are simply never drawn. Nothing in the suite saw it, because no
// sandbox scene had TessellationEnabled: true, so the quadtree path had no
// coverage at all. From above — the exact camera the issue's acceptance criteria
// name — the terrain was mostly missing.
//
// The GPU path has no such mapping: a visible node IS the drawn primitive, one
// instance of a shared unit grid stretched over the node's rect. So the two
// assertions here are:
//
//   * TopDownCoverage — looking down at a terrain that spans the whole frame,
//     essentially no background pixel survives. This fails hard on the CPU path.
//   * GroundLevelSilhouette — the near view still renders substantial lit
//     terrain, i.e. the rewrite did not trade the far view for the near one.
//
// A crack is NOT detectable by an RMSE against the CPU path (the two draw
// different geometry densities by design). The exhaustive crack proof is
// TerrainGPUQuadtreeTest's edge-vertex set comparison; this file proves the
// frame that math produces is actually on screen.
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
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstdlib>
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

        // What "no terrain here" looks like in THIS fixture, measured rather
        // than assumed: the editor's infinite grid, an untextured unlit grey
        // spanning luma ~123-165 (the darker values are its lines).
        //
        // The discriminator that matters is not the luma band but **exact
        // neutrality**: the grid is untextured, so its channels come out bit
        // identical, while terrain is lit through a coloured splat material and
        // essentially never lands on r == g == b. Measured over the centre crop
        // of the two top-down captures:
        //
        //   r==g==b  AND luma in [118,170] -> CPU path 80.1%, GPU path 0.000%
        //   |r-g|<=1 AND luma in [118,170] -> CPU path 81.5%, GPU path 10.5%
        //
        // The tolerant form counts grey rock as background and is useless; the
        // exact form separates them completely. An earlier version of this file
        // used a +-6 band around luma 86 taken from a different evidence test's
        // clear colour — it matched almost nothing here, so the coverage check
        // passed while measuring nothing at all. The anti-vacuous assertion in
        // the A/B test below exists to catch exactly that.
        constexpr f32 kBackgroundLumaMin = 118.0f;
        constexpr f32 kBackgroundLumaMax = 170.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        [[nodiscard]] f32 Luma(u8 r, u8 g, u8 b)
        {
            return 0.2126f * static_cast<f32>(r) + 0.7152f * static_cast<f32>(g) + 0.0722f * static_cast<f32>(b);
        }

        // Fraction of the CENTRE of the frame that still looks like the
        // untouched clear colour. Centre-cropped on purpose: the exact ground
        // footprint of a tilted top-down camera depends on FOV and height, so a
        // whole-frame test would be asserting the framing as much as the
        // coverage. A missing quadtree node is a whole screen quadrant, not a
        // sliver at the edge, so the crop loses nothing that matters.
        [[nodiscard]] f32 CentreBackgroundFraction(const std::vector<u8>& px)
        {
            if (px.size() < static_cast<sizet>(kWidth) * kHeight * 4)
                return 1.0f;
            const u32 x0 = kWidth / 5, x1 = kWidth - kWidth / 5;
            const u32 y0 = kHeight / 5, y1 = kHeight - kHeight / 5;
            sizet background = 0, total = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4;
                    const f32 luma = Luma(px[i], px[i + 1], px[i + 2]);
                    // EXACT equality, not a tolerance — see the constants above.
                    const bool neutral = px[i] == px[i + 1] && px[i + 1] == px[i + 2];
                    if (neutral && luma >= kBackgroundLumaMin && luma <= kBackgroundLumaMax)
                        ++background;
                    ++total;
                }
            }
            return total ? static_cast<f32>(background) / static_cast<f32>(total) : 1.0f;
        }

        [[nodiscard]] f32 LitFraction(const std::vector<u8>& px)
        {
            if (px.empty())
                return 0.0f;
            sizet lit = 0, total = 0;
            for (sizet i = 0; i + 3 < px.size(); i += 4)
            {
                const bool neutral = px[i] == px[i + 1] && px[i + 1] == px[i + 2];
                const f32 luma = Luma(px[i], px[i + 1], px[i + 2]);
                if (!(neutral && luma >= kBackgroundLumaMin && luma <= kBackgroundLumaMax))
                    ++lit;
                ++total;
            }
            return total ? static_cast<f32>(lit) / static_cast<f32>(total) : 0.0f;
        }

        void MaybeWritePng(const std::string& name, const std::vector<u8>& px)
        {
            if (!GoldenRebaseRequested())
                return;
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("TerrainGpuLod_" + name + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                             4, px.data(), static_cast<int>(kWidth) * 4);
        }
    } // namespace

    class TerrainGPUQuadtreeVisualEvidenceTest : public RendererAttachedTest
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
                terrain.m_ProceduralSeed = 714714;
                terrain.m_ProceduralResolution = 256;
                terrain.m_ProceduralOctaves = 6;
                terrain.m_ProceduralFrequency = 3.0f;
                terrain.m_HeightShaping.RidgeBlend = 0.45f;
                terrain.m_HeightShaping.WarpStrength = 0.12f;
                terrain.m_WorldSizeX = 1024.0f;
                terrain.m_WorldSizeZ = 1024.0f;
                terrain.m_HeightScale = 120.0f;
                // The whole point: this is the flag that turns the quadtree LOD
                // path on, and no shipped scene set it before #714.
                terrain.m_TessellationEnabled = true;
                // Deliberately loose. The screen-space error of a level-1 node
                // (512 world units) at this camera distance is ~600 px, so the
                // shipped default of 8 would split everything to leaves and the
                // descent would never select a non-leaf node — which is exactly
                // the case where the pre-#714 chunk mapping happens to be
                // correct. A quality knob this loose is unusual but legal, and
                // it is the only way to make the coarse-node path deterministic
                // instead of waiting for a camera far enough away that the
                // terrain is a few pixels wide.
                terrain.m_TargetTriangleSize = 900.0f;

                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;
            }
        }

        void TearDown() override
        {
            // Never leak the A/B lever into another test in this process.
            TerrainChunkManager::SetGpuDrivenLODEnabled(true);
            RendererAttachedTest::TearDown();
        }

        // Pose, tick, read back. `pitch` is the EditorCamera convention, not the
        // MCP tool's — kept slightly off vertical for the top-down case because a
        // perfectly vertical look direction is parallel to the camera's up axis
        // and yields a degenerate view matrix.
        // Callers MUST wrap this in ASSERT_NO_FATAL_FAILURE: the ASSERT_ below
        // returns from this helper, not from the test, so a failed readback
        // would otherwise leave `outPixels` empty and let the assertions run on
        // nothing. With both captures empty the A/B comparison still fails, but
        // with only ONE empty it passes — a false green.
        void Capture(const glm::vec3& eye, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 6000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);

            // The first ticks build the heightmap, chunks, both quadtrees and the
            // auto-splat; the last renders the ready terrain.
            RunEditorFrames(camera, 4);

            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, w, h)) << "no composited frame";
            ASSERT_EQ(w, kWidth);
            ASSERT_EQ(h, kHeight);
        }

        Entity m_TerrainEntity;
    };

    // Looking down from high enough that the terrain more than fills the frame.
    // Every pixel must be terrain. This is the assertion the pre-#714 path fails:
    // its coarse-node draws cover one chunk each and leave the rest empty.
    TEST_F(TerrainGPUQuadtreeVisualEvidenceTest, TopDownFrameIsFullyCoveredByTerrain)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TerrainChunkManager::SetGpuDrivenLODEnabled(true);

        std::vector<u8> gpuTopDown;
        ASSERT_NO_FATAL_FAILURE(Capture(glm::vec3(512.0f, 400.0f, 512.0f), 0.0f, 1.50f, gpuTopDown));
        MaybeWritePng("gpu_topdown", gpuTopDown);

        const f32 background = CentreBackgroundFraction(gpuTopDown);
        EXPECT_LT(background, 0.02f)
            << "looking down at terrain that overfills the frame, " << (background * 100.0f)
            << "% of pixels are still the clear colour — the LOD selection is leaving holes";
    }

    TEST_F(TerrainGPUQuadtreeVisualEvidenceTest, GroundLevelViewStillRendersSubstantialTerrain)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TerrainChunkManager::SetGpuDrivenLODEnabled(true);

        std::vector<u8> gpuGround;
        ASSERT_NO_FATAL_FAILURE(Capture(glm::vec3(512.0f, 160.0f, 980.0f), 0.0f, 0.14f, gpuGround));
        MaybeWritePng("gpu_ground", gpuGround);

        // A ground-level view is half sky by construction, so this is a
        // "the near field did not regress to nothing" bound, not a coverage one.
        const f32 lit = LitFraction(gpuGround);
        EXPECT_GT(lit, 0.20f) << "ground-level terrain should fill a substantial part of the frame";
    }

    // The A/B that shows the fix rather than just asserting the fixed state:
    // same scene, same pose, only the selection path differs.
    TEST_F(TerrainGPUQuadtreeVisualEvidenceTest, GpuPathCoversMoreOfTheTopDownFrameThanTheCpuPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 512.0f, 400.0f, 512.0f };

        TerrainChunkManager::SetGpuDrivenLODEnabled(false);
        std::vector<u8> cpuTopDown;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 1.50f, cpuTopDown));
        MaybeWritePng("cpu_topdown", cpuTopDown);
        const f32 cpuBackground = CentreBackgroundFraction(cpuTopDown);

        TerrainChunkManager::SetGpuDrivenLODEnabled(true);
        std::vector<u8> gpuTopDown;
        ASSERT_NO_FATAL_FAILURE(Capture(eye, 0.0f, 1.50f, gpuTopDown));
        const f32 gpuBackground = CentreBackgroundFraction(gpuTopDown);

        // Not "<=": at this target size the CPU descent selects level-1 nodes,
        // each of which draws exactly ONE of the four chunks it covers, so the
        // gap is three quadrants wide. If this ever comes out equal, the
        // descent stopped selecting non-leaf nodes and the test has gone
        // vacuous — check the target triangle size before believing the pass.
        EXPECT_GT(cpuBackground, 0.10f)
            << "the CPU path was expected to leave visible holes at this LOD setting; it left "
            << cpuBackground << " — the comparison below would prove nothing";
        EXPECT_LT(gpuBackground, cpuBackground)
            << "GPU path background=" << gpuBackground << " CPU path background=" << cpuBackground
            << " — the GPU descent must cover at least as much of the frame as the CPU one";
    }
} // namespace OloEngine::Tests
